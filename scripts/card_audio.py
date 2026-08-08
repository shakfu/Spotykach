#!/usr/bin/env python3
"""WAV inspection and generation for the SD card tooling - stdlib only, no decoder required.

Two jobs, neither of which needs ffmpeg/sox/cysox:

1. **Inspect** a WAV already on the card, the way the FIRMWARE does. `verify`'s whole value is
   predicting what the device will do with a file, so `parse_wav` deliberately mirrors the firmware's
   own chunk walk (`src/memory/raw_stream.h:39-75` for the streaming engines, `src/hw/card.cpp:50-66`
   for granular) rather than using the `wave` module. `wave` rejects WAVE_FORMAT_EXTENSIBLE outright
   and hides the data offset, so it would disagree with the device on exactly the files users get
   wrong. Where the firmware is lenient (extensible-format GUID, chunks before `data`) this is lenient
   in the same way; where it is strict (fmt must precede data, 64-chunk bound) so is this.

2. **Generate** the demo content for `sk_card.py init`. Synthesized rather than sampled, so the base
   card carries no licensing questions and `make sdcard` stays a stdlib-only step in the release path.
"""

from __future__ import annotations

import math
import random
import struct
from dataclasses import dataclass
from pathlib import Path

from card_layout import F32, INT16

WAVE_FORMAT_PCM = 1
WAVE_FORMAT_FLOAT = 3
WAVE_FORMAT_EXTENSIBLE = 0xFFFE

MAX_CHUNKS = 64  # raw_stream.h:24 - kMaxChunks, the firmware's chunk-walk bound


@dataclass
class WavInfo:
    """What the firmware would conclude about a WAV file."""

    fmt: int
    channels: int
    rate: int
    bits: int
    data_offset: int
    data_size: int

    @property
    def encoding(self) -> str | None:
        if self.fmt == WAVE_FORMAT_FLOAT and self.bits == 32:
            return F32
        if self.fmt == WAVE_FORMAT_PCM and self.bits == 16:
            return INT16
        return None

    @property
    def frames(self) -> int:
        bytes_per_frame = max(1, (self.bits // 8) * max(1, self.channels))
        return self.data_size // bytes_per_frame

    @property
    def seconds(self) -> float:
        return self.frames / self.rate if self.rate else 0.0

    def describe(self) -> str:
        enc = {
            (WAVE_FORMAT_FLOAT, 32): "32-bit float",
            (WAVE_FORMAT_PCM, 16): "16-bit PCM",
            (WAVE_FORMAT_PCM, 8): "8-bit PCM",
            (WAVE_FORMAT_PCM, 24): "24-bit PCM",
            (WAVE_FORMAT_PCM, 32): "32-bit INTEGER PCM",
        }.get((self.fmt, self.bits), f"format tag {self.fmt}, {self.bits}-bit")
        ch = {1: "mono", 2: "stereo"}.get(self.channels, f"{self.channels}ch")
        return f"{enc}, {ch}, {self.rate} Hz"


class WavError(Exception):
    """The firmware's header parse would fail on this file (so it reads as empty / is skipped)."""


def _u32(b: bytes, o: int) -> int:
    return struct.unpack_from("<I", b, o)[0]


def _u16(b: bytes, o: int) -> int:
    return struct.unpack_from("<H", b, o)[0]


def parse_wav(path: Path) -> WavInfo:
    """Parse `path` the way the firmware does. Raises WavError where the firmware returns false.

    The chunk walk matters: a converter that writes `fact`/`LIST` metadata pushes `data` well past the
    canonical offset 44, and older firmware that assumed 44 could not read those files at all. Current
    firmware walks properly, so this does too.
    """
    data = path.read_bytes()
    size = len(data)
    if size < 12:
        raise WavError("file is too short to be a WAV (under 12 bytes)")
    if data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise WavError("not a RIFF/WAVE file (missing the 'RIFF'/'WAVE' magic)")

    fmt = channels = bits = rate = 0
    have_fmt = False
    pos = 12
    for _ in range(MAX_CHUNKS):
        if pos + 8 > size:
            raise WavError("ran off the end of the file without finding a 'data' chunk")
        cid = data[pos:pos + 4]
        csize = _u32(data, pos + 4)
        body = pos + 8
        if cid == b"fmt ":
            if csize < 16 or body + 16 > size:
                raise WavError("'fmt ' chunk is shorter than the 16 bytes the parser needs")
            fmt = _u16(data, body + 0)
            channels = _u16(data, body + 2)
            rate = _u32(data, body + 4)
            bits = _u16(data, body + 14)
            if fmt == WAVE_FORMAT_EXTENSIBLE and csize >= 40 and body + 26 <= size:
                # WAVE_FORMAT_EXTENSIBLE: the real format tag is the first 2 bytes of the GUID.
                # ffmpeg writes this for some inputs; the firmware unwraps it (raw_stream.h:57-61).
                fmt = _u16(data, body + 24)
            have_fmt = True
        elif cid == b"data":
            if not have_fmt:
                raise WavError("'data' chunk appears before 'fmt ' - the parser needs fmt first")
            return WavInfo(fmt, channels, rate, bits, body, min(csize, size - body))
        pos = body + csize + (csize & 1)  # RIFF chunks are word-aligned
    raise WavError(f"no 'data' chunk within the first {MAX_CHUNKS} chunks")


# --- writing ------------------------------------------------------------------------------------


def _pack(samples: list[float], encoding: str) -> bytes:
    if encoding == F32:
        return struct.pack(f"<{len(samples)}f", *samples)
    clipped = [max(-1.0, min(1.0, s)) for s in samples]
    return struct.pack(f"<{len(clipped)}h", *(int(s * 32767.0) for s in clipped))


def write_wav(path: Path, samples: list[float], rate: int, channels: int, encoding: str) -> None:
    """Write a canonical 44-byte-header WAV. Interleaved samples for channels == 2.

    We emit the plain 44-byte layout (no `fact`, no `LIST`) even though the firmware now chunk-walks:
    it is exactly what the device's own recorder writes (`src/memory/wav.h:21,46-48` - a fixed
    `BlocSize = 16` and a 44-byte `static_assert`), so a card built here is byte-shaped like a card the
    device made, and it stays readable by firmware predating the chunk-walk fix.

    Note for float files this is technically under-specified: the spec wants an 18-byte `fmt ` (with
    `cbSize`) plus a `fact` chunk for WAVE_FORMAT_IEEE_FLOAT, and strict readers say so - libsox warns
    "wave header missing extended part of fmt chunk" when it reads one. It is a warning, not a failure,
    and matching the hardware matters more here than satisfying a linter, so this is deliberate.
    """
    body = _pack(samples, encoding)
    bits = 32 if encoding == F32 else 16
    fmt = WAVE_FORMAT_FLOAT if encoding == F32 else WAVE_FORMAT_PCM
    block_align = channels * bits // 8
    header = b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, fmt, channels, rate,
                                    rate * block_align, block_align, bits)
    header += b"data" + struct.pack("<I", len(body))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + body)


def write_raw(path: Path, samples: list[float], _rate: int = 48000) -> None:
    """Write headerless signed-16-bit mono PCM - the radio engine's station format.

    There is no header, so nothing in the file records the sample rate: it is fixed by convention
    (48 kHz) or overridden globally by radio/rate.txt. That is the whole reason this format is easy to
    get wrong by hand and worth generating.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(_pack(samples, INT16))


def read_samples(path: Path) -> tuple[list[float], WavInfo]:
    """Decode a WAV's body to floats in [-1, 1]. Used to read back what a decoder backend produced,
    so everything downstream re-encodes through `write_wav`/`write_raw` and gets firmware-shaped
    headers regardless of which external tool did the decoding."""
    info = parse_wav(path)
    body = path.read_bytes()[info.data_offset:info.data_offset + info.data_size]
    if info.encoding == F32:
        n = len(body) // 4
        return list(struct.unpack(f"<{n}f", body[:n * 4])), info
    if info.encoding == INT16:
        n = len(body) // 2
        return [s / 32768.0 for s in struct.unpack(f"<{n}h", body[:n * 2])], info
    raise WavError(f"cannot decode {info.describe()} - expected 32-bit float or 16-bit PCM")


# --- synthesis ----------------------------------------------------------------------------------
#
# Demo content, generated rather than sampled: no licensing questions, no download weight, and every
# engine makes sound on a fresh card. Seeded per-file so `make sdcard` is byte-reproducible - a
# release artifact that changed on every build would be impossible to checksum meaningfully.


def _env(i: int, n: int, attack: float = 0.01, release: float = 0.3) -> float:
    """Attack/release envelope, so nothing starts or ends on a click."""
    a = max(1, int(n * attack))
    r = max(1, int(n * release))
    if i < a:
        return i / a
    if i > n - r:
        return max(0.0, (n - i) / r)
    return 1.0


def tone(seconds: float, freq: float, rate: int, *, harmonics: int = 3, gain: float = 0.5) -> list[float]:
    """A soft harmonic tone - musical enough to hear pitch/varispeed changes, dull enough not to grate."""
    n = int(seconds * rate)
    out = []
    for i in range(n):
        t = i / rate
        s = sum(math.sin(2 * math.pi * freq * h * t) / (h * h) for h in range(1, harmonics + 1))
        out.append(s * gain * _env(i, n))
    return out


def sweep(seconds: float, f0: float, f1: float, rate: int, gain: float = 0.4) -> list[float]:
    """Exponential sweep - makes filter/pitch behaviour obvious across the whole range."""
    n = int(seconds * rate)
    out = []
    phase = 0.0
    for i in range(n):
        f = f0 * (f1 / f0) ** (i / max(1, n - 1))
        phase += 2 * math.pi * f / rate
        out.append(math.sin(phase) * gain * _env(i, n))
    return out


def noise_bed(seconds: float, rate: int, seed: int, gain: float = 0.25) -> list[float]:
    """Filtered noise - a texture bed for the granular/stretch engines to chew on."""
    rng = random.Random(seed)
    n = int(seconds * rate)
    out = []
    lp = 0.0
    for i in range(n):
        lp += 0.02 * (rng.uniform(-1.0, 1.0) - lp)
        out.append(lp * gain * 4.0 * _env(i, n))
    return out


def pulse_pattern(seconds: float, rate: int, seed: int, bpm: float = 110.0) -> list[float]:
    """A rhythmic pattern - gives the delay/glitch/shuttle engines something with transients."""
    rng = random.Random(seed)
    n = int(seconds * rate)
    out = [0.0] * n
    step = int(rate * 60.0 / bpm / 4)
    for k in range(0, n // max(1, step)):
        if rng.random() < 0.55:
            f = rng.choice([110.0, 165.0, 220.0, 330.0])
            start = k * step
            dur = min(step * 2, n - start)
            for i in range(dur):
                t = i / rate
                out[start + i] += math.sin(2 * math.pi * f * t) * 0.5 * math.exp(-t * 18.0)
    return [max(-1.0, min(1.0, s)) for s in out]


def speech_like(seconds: float, rate: int, seed: int) -> list[float]:
    """A formant-ish babble for the bard shelves - not speech, but voice-shaped and clearly not music,
    so the demo book is obviously a placeholder rather than something the user might mistake for content."""
    rng = random.Random(seed)
    n = int(seconds * rate)
    out = []
    f0, target = 120.0, 120.0
    phase = 0.0
    for i in range(n):
        if i % (rate // 6) == 0:
            target = rng.uniform(90.0, 160.0)
        f0 += 0.0005 * (target - f0)
        phase += 2 * math.pi * f0 / rate
        # two crude formants over a buzzy source
        s = (math.sin(phase) * 0.5 + math.sin(phase * 4.6) * 0.25 + math.sin(phase * 7.3) * 0.12)
        gate = 1.0 if (i // (rate // 3)) % 4 != 3 else 0.15  # pauses, like sentences
        out.append(s * 0.5 * gate * _env(i, n, attack=0.002, release=0.05))
    return out


def stereo(mono: list[float], spread: float = 0.15) -> list[float]:
    """Interleave a mono signal to stereo with a slight haas-style offset, for the granular bank."""
    delay = max(1, int(len(mono) * spread / 1000))
    out: list[float] = []
    for i, s in enumerate(mono):
        out.append(s)
        out.append(mono[i - delay] if i >= delay else 0.0)
    return out


def pad_to_bytes(samples: list[float], min_bytes: int, bytes_per_sample: int) -> list[float]:
    """Extend by looping until the encoded file clears a byte floor.

    Needed because the scanned banks skip anything under 32 KB (SCAN_MIN_BYTES): a demo clip that is
    correct in every other respect but 20 KB long would be silently invisible - exactly the failure
    this whole tool exists to prevent, and an embarrassing one to ship on the official card.
    """
    if not samples:
        return samples
    need = -(-min_bytes // bytes_per_sample)
    out = list(samples)
    while len(out) < need:
        out.extend(samples)
    return out[:max(need, len(samples))]
