#!/usr/bin/env python3
"""Batch-convert audio files to the sk-engines streaming/load format (tape, shuttle).

The `tape` and `shuttle` engines stream (tape) or load (shuttle) **MONO 32-bit IEEE
float WAV at 48 kHz** (`AudioFormat 3`). The firmware does NO conversion on the audio
path - it reads file body bytes straight into float frames - so any other encoding is
reinterpreted as garbage. This script decodes arbitrary inputs to that target format
with ffmpeg (or sox) and writes deck-ready `.wav` files, optionally named as slot files.

Output framing - native by default:
    The firmware's WAV reader (`WavStreamReader::begin`, src/memory/wav_stream.h) now
    seek/read chunk-walks the header with no offset ceiling, so it accepts externally
    authored files whose `fact`/`LIST` metadata pushes `data` past offset 44. We therefore
    let the converter write the WAV container itself by default. The legacy `--canonical-
    header` mode (decode to raw float, then prepend the exact 44-byte header the firmware's
    own recorder writes) remains as a fallback for firmware predating that robustness fix.

Supported/target format (what the firmware accepts):
    container : WAV (RIFF)
    encoding  : 32-bit IEEE float  (ffmpeg pcm_f32le / sox -e floating-point -b 32)
    channels  : 1 (mono; stereo is down-mixed)
    rate      : 48000 Hz (no resampling on-device, so 44.1k etc. must be converted)

Slot layout on the card (8 slots per deck, selected with Alt+PITCH):
    tape    -> /tapes/tape_<a|b>_<1..8>.wav
    shuttle -> /shuttle/tape_<a|b>_<1..8>.wav   (RAM-capped: ~30 s per track)

Examples:
    # Convert a few files, keeping their names, into ./out
    scripts/convert_tape_audio.py convert -o out kick.wav loop.aiff vocal.mp3

    # Fill deck A's slots 1.. for the tape engine (-> out/tapes/tape_a_1.wav ...)
    scripts/convert_tape_audio.py convert --engine tape --deck a -o out beat1.wav beat2.wav

    # One-shot: convert every audio file in a directory (sorted) into deck B's shuttle slots
    scripts/convert_tape_audio.py from-dir --engine shuttle --deck b -o card ./samples

    # Recurse into subdirectories, and use sox instead of ffmpeg
    scripts/convert_tape_audio.py from-dir --recursive --tool sox -o out ./library
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
from abc import ABC, abstractmethod
from dataclasses import dataclass

TARGET_RATE = 48000
TARGET_BITS = 32
TARGET_FMT = 3            # WAVE_FORMAT_IEEE_FLOAT
BYTES_PER_SAMPLE = TARGET_BITS // 8
TARGET_TUPLE = (TARGET_FMT, 1, TARGET_RATE, TARGET_BITS)
SLOTS_PER_DECK = 8
SHUTTLE_MAX_SECONDS = 30  # per-track RAM cap (kBufSeconds in shuttle_engine.h)
ENGINE_DIR = {"tape": "tapes", "shuttle": "shuttle"}
MAX_CHUNKS = 64           # mirrors kMaxChunks in WavStreamReader::begin
# Extensions globbed by `from-dir`; the converters can read more, but this keeps the
# directory sweep to plausible audio and skips sidecar/project files.
AUDIO_EXTS = {".wav", ".aif", ".aiff", ".aifc", ".mp3", ".flac",
              ".ogg", ".oga", ".opus", ".m4a", ".aac", ".wma", ".caf", ".au"}


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


# --- decode strategy: ffmpeg vs sox -----------------------------------------

class Converter(ABC):
    """Strategy for decoding an arbitrary input to mono / 48 kHz / float32.

    `write_wav` produces the deck-ready container directly (the default path).
    `decode_raw` yields headerless f32le bytes for the `--canonical-header` path,
    where we prepend the firmware recorder's exact 44-byte header ourselves.
    """

    name = ""

    def available(self):
        return shutil.which(self.name) is not None

    @abstractmethod
    def decode_raw(self, src):
        """Decode `src` to headerless mono/48k/f32le bytes on stdout."""

    @abstractmethod
    def write_wav(self, src, dst):
        """Decode `src` and write a native mono/48k/float32 WAV to `dst`."""


class FfmpegConverter(Converter):
    name = "ffmpeg"

    def decode_raw(self, src):
        return subprocess.run(
            ["ffmpeg", "-nostdin", "-loglevel", "error", "-i", src,
             "-ac", "1", "-ar", str(TARGET_RATE), "-f", "f32le", "-"],
            check=True, stdout=subprocess.PIPE).stdout

    def write_wav(self, src, dst):
        subprocess.run(
            ["ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", src,
             "-ac", "1", "-ar", str(TARGET_RATE), "-c:a", "pcm_f32le", dst],
            check=True)


class SoxConverter(Converter):
    name = "sox"

    def decode_raw(self, src):
        # `-t f32` = raw 32-bit float; output rate/channels make sox resample + down-mix.
        return subprocess.run(
            ["sox", src, "-t", "f32", "-r", str(TARGET_RATE), "-c", "1", "-"],
            check=True, stdout=subprocess.PIPE).stdout

    def write_wav(self, src, dst):
        subprocess.run(
            ["sox", src, "-e", "floating-point", "-b", str(TARGET_BITS),
             "-r", str(TARGET_RATE), "-c", "1", dst],
            check=True)


CONVERTERS = {c.name: c for c in (FfmpegConverter(), SoxConverter())}


# --- WAV helpers ------------------------------------------------------------

def canonical_header(data_bytes):
    """The exact 44-byte header the firmware's recorder writes (see src/memory/wav.h WavHeader):
    `fmt ` size 16, float/mono/48k, then `data` - no `fact`/`LIST`, so the body sits at offset 44."""
    byte_rate = TARGET_RATE * BYTES_PER_SAMPLE          # mono: blockalign == bytes/sample
    block_align = BYTES_PER_SAMPLE
    return (b"RIFF" + struct.pack("<I", 36 + data_bytes) + b"WAVE"
            + b"fmt " + struct.pack("<IHHIIHH", 16, TARGET_FMT, 1, TARGET_RATE,
                                    byte_rate, block_align, TARGET_BITS)
            + b"data" + struct.pack("<I", data_bytes))


def walk_wav(f):
    """Seek/read chunk-walk a WAV, mirroring WavStreamReader::begin (no header-offset ceiling).
    Returns (fmt_tuple_or_None, data_off_or_None, data_size_or_None). fmt is (af, ch, sr, bits)."""
    head = f.read(12)
    if len(head) < 12 or head[0:4] != b"RIFF" or head[8:12] != b"WAVE":
        return None, None, None
    fmt = data_off = data_sz = None
    pos = 12
    for _ in range(MAX_CHUNKS):
        f.seek(pos)
        hdr = f.read(8)
        if len(hdr) < 8:                                # ran off the end before `data`
            break
        cid, sz = hdr[0:4], struct.unpack("<I", hdr[4:8])[0]
        body = pos + 8
        if cid == b"fmt ":
            fb = f.read(16)
            if len(fb) >= 16:
                af, ch = struct.unpack("<HH", fb[0:4])
                sr = struct.unpack("<I", fb[4:8])[0]
                bits = struct.unpack("<H", fb[14:16])[0]
                # WAVE_FORMAT_EXTENSIBLE (0xFFFE): the real format tag is the first 2 bytes of the
                # SubFormat GUID at body+24 (past cbSize(2) + wValidBitsPerSample(2) + dwChannelMask(4)).
                # Mirrors WavStreamReader::begin (src/memory/wav_stream.h) - this function claims to
                # parse the way the firmware does, and without this it does not: ffmpeg writes 0xFFFE
                # for float WAVs, so a perfectly playable file was reported as the wrong format. The
                # error was in the safe direction (rejecting a good file, never accepting a bad one),
                # which is why it stayed hidden - `--canonical` rewrites the header and masks it, and
                # sox writes tag 3 directly.
                if af == 0xFFFE and sz >= 40:
                    f.seek(body + 24)
                    tag = f.read(2)
                    if len(tag) == 2:
                        af = struct.unpack("<H", tag)[0]
                fmt = (af, ch, sr, bits)
        elif cid == b"data":
            data_off, data_sz = body, sz
            break
        pos = body + sz + (sz & 1)                      # chunks are word-aligned
    return fmt, data_off, data_sz


def verify(path):
    """Parse the WAV header the way the firmware does; return (ok, detail)."""
    with open(path, "rb") as f:
        fmt, data_off, data_sz = walk_wav(f)
    if fmt is None or data_off is None:
        return False, "not RIFF/WAVE or missing fmt/data chunk"
    if fmt != TARGET_TUPLE:
        return False, f"format {fmt} != {TARGET_TUPLE} (float/mono/48k)"
    seconds = data_sz / (TARGET_RATE * BYTES_PER_SAMPLE)
    return True, f"mono float32 48k, {seconds:.1f}s, data@{data_off}"


def wav_seconds(path):
    """Duration in seconds from a target-format WAV's data chunk size (0.0 if unreadable)."""
    with open(path, "rb") as f:
        _, _, data_sz = walk_wav(f)
    return (data_sz or 0) / (TARGET_RATE * BYTES_PER_SAMPLE)


# --- batch orchestration ----------------------------------------------------

@dataclass
class ConvertOptions:
    converter: Converter
    out: str = "."
    engine: str = None
    deck: str = None
    start_slot: int = 1
    canonical: bool = False
    verify: bool = True
    recursive: bool = False


class TapeAudioBatch:
    """Drives a batch of conversions under one ConvertOptions, tracking deck slots."""

    def __init__(self, opts):
        self.opts = opts
        self.out_dir = opts.out
        if opts.engine:
            self.out_dir = os.path.join(self.out_dir, ENGINE_DIR[opts.engine])
        self.max_seconds = SHUTTLE_MAX_SECONDS if opts.engine == "shuttle" else None
        self.slot = opts.start_slot

    def gather_dir(self, root):
        """Sorted list of audio files under `root` (recursively if opts.recursive)."""
        found = []
        if self.opts.recursive:
            for base, _, files in os.walk(root):
                for fn in files:
                    if os.path.splitext(fn)[1].lower() in AUDIO_EXTS:
                        found.append(os.path.join(base, fn))
        else:
            for fn in os.listdir(root):
                p = os.path.join(root, fn)
                if os.path.isfile(p) and os.path.splitext(fn)[1].lower() in AUDIO_EXTS:
                    found.append(p)
        return sorted(found)

    def target_name(self, src):
        """Next output filename, or None if deck slots are exhausted. Advances the slot."""
        if not self.opts.deck:
            return os.path.splitext(os.path.basename(src))[0] + ".wav"
        if self.slot > SLOTS_PER_DECK:
            return None
        name = f"tape_{self.opts.deck}_{self.slot}.wav"
        self.slot += 1
        return name

    def convert_one(self, src, dst):
        """Produce `dst` from `src`; return duration in seconds.
        Raises subprocess.CalledProcessError if the converter cannot decode `src`."""
        if self.opts.canonical:
            raw = self.opts.converter.decode_raw(src)
            raw = raw[:len(raw) - (len(raw) % BYTES_PER_SAMPLE)]   # whole float frames only
            with open(dst, "wb") as f:
                f.write(canonical_header(len(raw)))
                f.write(raw)
            return len(raw) / (TARGET_RATE * BYTES_PER_SAMPLE)
        self.opts.converter.write_wav(src, dst)
        return wav_seconds(dst)

    def run(self, inputs):
        """Convert each input file; return a process exit code (0 = all ok)."""
        os.makedirs(self.out_dir, exist_ok=True)
        rc = 0
        for src in inputs:
            name = self.target_name(src)
            if name is None:
                print(f"skip (only {SLOTS_PER_DECK} slots/deck): {src}", file=sys.stderr)
                rc = 1
                continue
            dst = os.path.join(self.out_dir, name)
            try:
                seconds = self.convert_one(src, dst)
            except subprocess.CalledProcessError:
                print(f"FAIL  {src}: {self.opts.converter.name} could not decode it", file=sys.stderr)
                rc = 1
                continue

            detail = ""
            if self.opts.verify:
                ok, detail = verify(dst)
                if not ok:
                    print(f"FAIL  {src} -> {dst}: {detail}", file=sys.stderr)
                    rc = 1
                    continue
            if self.max_seconds and seconds > self.max_seconds:
                print(f"WARN  {dst}: {seconds:.1f}s exceeds the shuttle ~{self.max_seconds}s RAM "
                      f"cap (it will be truncated on load)", file=sys.stderr)
            print(f"ok    {src} -> {dst}" + (f"   [{detail}]" if detail else ""))
        return rc


# --- CLI --------------------------------------------------------------------

def add_common_opts(p):
    p.add_argument("-o", "--out", default=".", help="output directory (default: current dir)")
    p.add_argument("--engine", choices=ENGINE_DIR,
                   help="place outputs in the engine's card subdir (tapes/ or shuttle/)")
    p.add_argument("--deck", choices=["a", "b"],
                   help="name outputs as deck slot files tape_<deck>_<n>.wav")
    p.add_argument("--start-slot", type=int, default=1,
                   help="first slot number when --deck is given (1..8)")
    p.add_argument("--tool", choices=CONVERTERS, default="ffmpeg",
                   help="converter to use (default: ffmpeg)")
    p.add_argument("--canonical-header", action="store_true",
                   help="legacy: write the firmware recorder's exact 44-byte header instead of "
                        "the converter's native container (only needed for pre-chunk-walk firmware)")
    p.add_argument("--no-verify", action="store_true", help="skip the output header check")


def build_parser():
    ap = argparse.ArgumentParser(
        description="Batch-convert audio to the sk-engines tape/shuttle format.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    pc = sub.add_parser("convert", help="convert one or more input files")
    pc.add_argument("inputs", nargs="+", help="input audio files (any format ffmpeg/sox can read)")
    add_common_opts(pc)

    pd = sub.add_parser("from-dir", help="one-shot: convert every audio file in a directory")
    pd.add_argument("directory", help="directory of input audio files")
    pd.add_argument("--recursive", action="store_true", help="recurse into subdirectories")
    add_common_opts(pd)

    return ap


def main():
    args = build_parser().parse_args()

    converter = CONVERTERS[args.tool]
    if not converter.available():
        die(f"{converter.name} not found on PATH")
    if args.deck and not (1 <= args.start_slot <= SLOTS_PER_DECK):
        die(f"--start-slot must be 1..{SLOTS_PER_DECK}")

    opts = ConvertOptions(
        converter=converter,
        out=args.out,
        engine=args.engine,
        deck=args.deck,
        start_slot=args.start_slot,
        canonical=args.canonical_header,
        verify=not args.no_verify,
        recursive=getattr(args, "recursive", False),
    )
    batch = TapeAudioBatch(opts)

    if args.cmd == "from-dir":
        if not os.path.isdir(args.directory):
            die(f"not a directory: {args.directory}")
        inputs = batch.gather_dir(args.directory)
        if not inputs:
            die(f"no audio files found in {args.directory}")
    else:
        inputs = []
        for src in args.inputs:
            if not os.path.isfile(src):
                print(f"skip (not a file): {src}", file=sys.stderr)
                continue
            inputs.append(src)
        if not inputs:
            die("no input files to convert")

    sys.exit(batch.run(inputs))


if __name__ == "__main__":
    main()
