#!/usr/bin/env python3
"""Prepare audiobooks for the `bard` engine: convert, rename to 8.3, and author bookmark sidecars.

The `bard` engine (the storyteller) streams **signed 16-bit MONO PCM** books from numbered shelves and
navigates them by BOOKMARKS listed in a plain-text sidecar next to each book:

    /bard/BARD.CFG                  optional: resume=on|off, rate=<hz>
    /bard/<shelf>/NAME.WAV          the book (shelf = 0..15, up to 32 books each)
    /bard/<shelf>/NAME.TXT          its bookmarks (optional; the engine invents some if absent)
    /bard/<shelf>/BOOKS.TXT         8.3 name -> real title, for the human at the card reader

Three things make hand-authoring a card unpleasant, and this script exists to do all three:

1. **8.3 names are mandatory.** The firmware's directory scan skips any filename longer than 12
   characters, so `The Hobbit - Chapter 3.mp3` is simply invisible to the device. Books are renamed to
   short numbered names and the real titles are recorded in `BOOKS.TXT`.

2. **24 kHz mono is the right format for speech**, not the 48 kHz the radio engine wants: half the bytes
   per hour (~173 MB vs ~345 MB, so ~185 h on a 32 GB card instead of ~92 h) and half the SD bandwidth
   per deck, for 12 kHz of audio bandwidth that voice never exceeds. A `.wav` carries its own rate, so
   the engine plays it at correct pitch with no configuration.

3. **Bookmarks want to come from real chapter boundaries**, and both common free sources have them - so
   silence detection is the LAST resort here, not the first. Mark sources in priority order:
   an explicit `--marks-from` list; chapters embedded in the container; the join points of a per-chapter
   set (`--join`); and only then silence detection.

The two libraries this was written against:

* **LibriVox** publishes a book as **one 64 kbps MP3 per chapter**. Pass `--join`: the chapters become ONE
  book with a bookmark at every chapter boundary, exact because each boundary is a file boundary. Without
  it, a 40-chapter book would eat 40 of a shelf's 32 slots as 40 separate "books".

* **LoyalBooks** offers a single **`.m4b`**, which carries an **embedded chapter list**. Nothing to pass -
  the chapters are read straight out of the container, with their titles as sidecar labels.

A note on quality: a low-bitrate mono encode is already band-limited, so the 24 kHz default (12 kHz of
audio bandwidth) throws away little of a spoken recording while halving the bytes per hour against 48 kHz.
Rather than assume what any library ships, the script probes each source and says so if the target rate is
HIGHER than the source's - upsampling only spends card space. `--rate` overrides; the engine reads the rate
from the WAV header either way, so any choice plays at correct pitch.

Examples:
    # LibriVox: a folder of per-chapter MP3s -> ONE book on shelf 0, one mark per chapter
    scripts/prepare_audiobooks.py from-dir --join --shelf 0 -o /Volumes/SD ~/Downloads/hobbit_librivox

    # LoyalBooks: a single .m4b -> marks from its embedded chapter list
    scripts/prepare_audiobooks.py convert --shelf 0 -o /Volumes/SD ~/Downloads/hobbit.m4b

    # Several independent books (not chapters of one) onto a shelf, marks detected from silence
    scripts/prepare_audiobooks.py from-dir --shelf 1 -o card ./lectures

    # Keep 48 kHz and let the engine invent its own marks
    scripts/prepare_audiobooks.py from-dir --shelf 2 --rate 48000 --no-marks -o card ./talks

    # Just write a sidecar for a book already on the card (no audio conversion)
    scripts/prepare_audiobooks.py marks /Volumes/SD/bard/0/BOOK1.WAV

Requires ffmpeg and ffprobe on PATH.
"""

import argparse
import bisect
import json
import os
import re
import shutil
import random
import subprocess
import sys
import tempfile
import zlib

# The engine's own limits (src/engine/bard/{bookmarks,resume_table}.h and bard_engine.h).
MAX_SHELVES = 16
MAX_BOOKS = 32
MAX_MARKS = 64
MAX_SIDECAR_BYTES = 4096
MIN_BOOK_BYTES = 32 * 1024          # scan_bank drops anything smaller (AppleDouble stubs, empty files)
DEFAULT_RATE = 24000

AUDIO_EXTS = {".wav", ".mp3", ".m4a", ".m4b", ".aac", ".flac", ".ogg", ".opus",
              ".aif", ".aiff", ".wma", ".raw"}


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def need_tool(name):
    if shutil.which(name) is None:
        die(f"{name} not found on PATH (install ffmpeg)")


def duration_seconds(path):
    """Media duration via ffprobe, or None if it cannot be determined."""
    try:
        out = subprocess.run(
            ["ffprobe", "-v", "quiet", "-print_format", "json", "-show_format", str(path)],
            capture_output=True, text=True, check=True).stdout
        return float(json.loads(out)["format"]["duration"])
    except Exception:
        return None


def probe_rate(path):
    """Source sample rate in Hz, or None. Used only to warn about pointless upsampling."""
    try:
        out = subprocess.run(
            ["ffprobe", "-v", "quiet", "-print_format", "json", "-select_streams", "a:0",
             "-show_streams", str(path)], capture_output=True, text=True, check=True).stdout
        streams = json.loads(out).get("streams", [])
        return int(streams[0]["sample_rate"]) if streams else None
    except Exception:
        return None


def warn_if_upsampling(src, rate, seen):
    """Note when the target rate exceeds the source's - that spends card space for no extra detail.

    Worth checking at runtime rather than assuming: free audiobook libraries vary (LibriVox publishes
    64 kbps MP3, LoyalBooks a single .m4b) and a low-bitrate mono encode may already be at or below the
    24 kHz default. Reported once per run, since a joined book has dozens of parts at the same rate.
    """
    if seen.get("warned"):
        return
    src_rate = probe_rate(src)
    if src_rate and rate > src_rate:
        seen["warned"] = True
        print(f"  note: source is {src_rate} Hz but --rate is {rate} Hz - upsampling costs card space "
              f"without adding detail. Consider --rate {src_rate}.", file=sys.stderr)


def convert(src, dst, rate):
    """Transcode `src` to 16-bit mono PCM WAV at `rate`."""
    subprocess.run(
        ["ffmpeg", "-nostdin", "-y", "-loglevel", "error", "-i", str(src),
         "-ac", "1", "-ar", str(rate), "-c:a", "pcm_s16le", str(dst)],
        check=True)


def natural_key(path):
    """Sort key that orders chapter_2 before chapter_10.

    LibriVox names are usually zero-padded (`hobbit_01_tolkien_64kb.mp3`), which sorts correctly either
    way - but plenty of collections are not, and getting the JOIN order wrong silently scrambles a book.
    """
    name = os.path.basename(path).lower()
    return [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", name)]


def wav_frames(path):
    """Frame count of a 16-bit mono PCM WAV, from its data chunk. None if it cannot be read.

    Read from the chunk table rather than inferred from the file size: ffmpeg may write a LIST/INFO chunk,
    so `(filesize - 44) / 2` is not reliable, and a wrong frame count would put every JOIN mark adrift.
    """
    try:
        with open(path, "rb") as f:
            if f.read(4) != b"RIFF":
                return None
            f.read(4)
            if f.read(4) != b"WAVE":
                return None
            while True:
                hdr = f.read(8)
                if len(hdr) < 8:
                    return None
                cid, size = hdr[:4], int.from_bytes(hdr[4:8], "little")
                if cid == b"data":
                    return size // 2                     # 16-bit mono: 2 bytes per frame
                f.seek(size + (size & 1), os.SEEK_CUR)
    except OSError:
        return None


def probe_chapters(path):
    """Embedded chapter start times in seconds, or [] if the file carries none.

    This is the whole reason a LoyalBooks-style `.m4b` is nicer to prepare than a pile of MP3s: the
    chapter list is already in the container, so the marks are exact and free rather than guessed from
    silence.
    """
    try:
        out = subprocess.run(
            ["ffprobe", "-v", "quiet", "-print_format", "json", "-show_chapters", str(path)],
            capture_output=True, text=True, check=True).stdout
        chapters = json.loads(out).get("chapters", [])
    except Exception:
        return []
    starts = []
    for ch in chapters:
        try:
            starts.append(float(ch["start_time"]))
        except (KeyError, ValueError, TypeError):
            continue
    return sorted(set(starts))


def chapter_titles(path):
    """Embedded chapter titles, parallel to probe_chapters(), for the sidecar's (human-only) labels."""
    try:
        out = subprocess.run(
            ["ffprobe", "-v", "quiet", "-print_format", "json", "-show_chapters", str(path)],
            capture_output=True, text=True, check=True).stdout
        chapters = json.loads(out).get("chapters", [])
    except Exception:
        return []
    return [str(ch.get("tags", {}).get("title", "")).strip() for ch in chapters]


def convert_join(sources, dst, rate, tmp_dir):
    """Convert every file in `sources` and concatenate them into ONE book at `dst`.

    Returns (marks_seconds, labels) with a mark at each JOIN - i.e. at every chapter boundary, exactly,
    because the boundary is a file boundary rather than something detected. This is what a LibriVox
    download wants: dozens of per-chapter MP3s become one book with a real chapter list, instead of dozens
    of separate "books" eating a shelf that only holds 32.

    Offsets are accumulated from each converted part's ACTUAL frame count, not from ffprobe's duration
    estimate, so the marks cannot drift over a ten-hour book.
    """
    parts, marks, labels = [], [], []
    seen = {}
    frames = 0
    for i, src in enumerate(sources):
        warn_if_upsampling(src, rate, seen)
        part = os.path.join(tmp_dir, f"part{i:04d}.wav")
        convert(src, part, rate)
        n = wav_frames(part)
        if n is None:
            die(f"could not read back the converted part for {src}")
        marks.append(frames / float(rate))
        labels.append(os.path.splitext(os.path.basename(src))[0])
        frames += n
        parts.append(part)

    listing = os.path.join(tmp_dir, "concat.txt")
    with open(listing, "w") as f:
        for part in parts:
            f.write(f"file '{os.path.abspath(part)}'\n")
    subprocess.run(
        ["ffmpeg", "-nostdin", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0",
         "-i", listing, "-ac", "1", "-ar", str(rate), "-c:a", "pcm_s16le", str(dst)],
        check=True)

    joined = wav_frames(dst)
    if joined is not None and abs(joined - frames) > rate // 10:      # tolerate < 100 ms of container slop
        print(f"  warning: joined length {joined} frames but parts summed to {frames}; "
              f"marks past the first may be adrift", file=sys.stderr)
    return marks, labels


# ---- bookmark authoring ---------------------------------------------------------------------------

def detect_silences(path, noise_db, min_silence):
    """Silence midpoints (seconds) via ffmpeg's silencedetect - the candidate chapter boundaries."""
    proc = subprocess.run(
        ["ffmpeg", "-nostdin", "-i", str(path), "-af",
         f"silencedetect=noise={noise_db}dB:d={min_silence}", "-f", "null", "-"],
        capture_output=True, text=True)
    starts, marks = [], []
    for line in proc.stderr.splitlines():
        m = re.search(r"silence_start:\s*([0-9.]+)", line)
        if m:
            starts.append(float(m.group(1)))
            continue
        m = re.search(r"silence_end:\s*([0-9.]+)", line)
        if m and starts:
            end = float(m.group(1))
            marks.append((starts.pop() + end) / 2.0)
    return sorted(marks)


def thin_marks(marks, duration, min_gap, limit=MAX_MARKS, exact=False):
    """Drop marks closer than `min_gap`, then, if still over `limit`, keep the most spread-out subset.

    The engine caps a book at 64 marks, so overshooting is not a soft failure - the tail would simply be
    dropped at parse time, and silently losing the END of a book's marks is worse than thinning evenly.

    `exact=True` means the marks are real chapter boundaries (embedded metadata or join points) rather than
    guesses: the minimum-gap filter is then skipped, because a genuinely short chapter is still a chapter,
    and dropping any of them is worth a warning.
    """
    # Seed with the start-of-book mark BEFORE filtering, not after: appending it afterwards would let it
    # sit closer than min_gap to the first detected boundary, which is exactly what min_gap forbids.
    out = [0.0]
    for t in marks:
        if t < 1.0 or (duration and t > duration - 1.0):
            continue
        if exact or (t - out[-1]) >= min_gap:
            out.append(t)
    if len(out) <= limit:
        return out
    if exact:
        print(f"  warning: {len(out)} chapters but the engine holds {limit} marks per book; thinning "
              f"evenly. Split the work into parts (adjacent books) to keep every chapter.", file=sys.stderr)
    step = len(out) / float(limit)
    return [out[int(i * step)] for i in range(limit)]


def random_marks(duration, count, silences, min_gap, seed, snap_window=None, report=print):
    """`count` random marks across the book, each snapped to the nearest detected silence.

    STRATIFIED rather than plain uniform: the book is divided into `count` equal strata and one point is
    drawn in each. That keeps the marks spread over the WHOLE recording instead of clumping by chance, and
    it is the difference between this and sampling the detected silences directly - there, mark density
    would follow *pause* density, so a passage full of short pauses would attract most of the marks while
    a densely-read passage got none.

    Snapping is what makes a random mark usable: an unsnapped random time lands mid-word about as often as
    not. A draw with no pause within `snap_window` keeps its raw time rather than being dropped, so the
    coverage guarantee survives a stretch of continuous speech.
    """
    if duration <= 0 or count <= 0:
        return []
    rng = random.Random(seed)
    stratum = duration / float(count)
    # A min-gap wider than a stratum would reject nearly every draw; clamp it and say so, rather than
    # silently returning three marks when the caller asked for twenty.
    gap = min(min_gap, stratum * 0.5)
    if gap < min_gap:
        report(f"  note: --min-gap {min_gap:g}s exceeds the {stratum:.0f}s spacing implied by {count} "
               f"marks; using {gap:.0f}s so the draws are not all rejected")
    if snap_window is None:
        snap_window = max(1.0, stratum / 2.0)
    sil = sorted(silences)

    out = [0.0]                                     # a book always gets a mark at its start
    for i in range(1, count):                       # stratum 0 belongs to the start mark
        lo = i * stratum
        t = rng.uniform(lo, min(duration, lo + stratum))
        if sil:                                     # snap to the nearest pause within the window
            j = bisect.bisect_left(sil, t)
            best, best_d = None, None
            for k in (j - 1, j):
                if 0 <= k < len(sil):
                    d = abs(sil[k] - t)
                    if best_d is None or d < best_d:
                        best, best_d = sil[k], d
            if best is not None and best_d <= snap_window:
                t = best
        if t - out[-1] >= gap and 1.0 < t < duration - 1.0:
            out.append(t)
    return out


def seed_for(path, explicit):
    """Stable seed: an explicit --seed, else a hash of the filename.

    Defaulting to the filename mirrors the engine's own convention for auto-marks (`book_seed` in
    bookmarks.h): the scatter is deterministic per book, so it can be learned and performed, and changing
    it is a deliberate act rather than an accident of re-running the script. Python's hash() is salted per
    process, so it cannot be used here.
    """
    if explicit is not None:
        return explicit
    return zlib.crc32(os.path.basename(path).encode()) & 0xffffffff


def parse_mark_file(path):
    """Read timestamps from a chapter list: one `[[HH:]MM:]SS[.mmm]` (or bare seconds) per line."""
    marks = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            tok = re.match(r"^([0-9]+(?::[0-9]{1,2})*(?:\.[0-9]+)?)", line)
            if not tok:
                continue
            fields = tok.group(1).split(":")
            try:
                secs = 0.0
                for f_ in fields:
                    secs = secs * 60.0 + float(f_)
            except ValueError:
                continue
            marks.append(secs)
    return sorted(set(marks))


def format_time(seconds):
    """`H:MM:SS.mmm` - the format bookmarks.h emits, so a round trip through the device is stable."""
    if seconds < 0:
        seconds = 0.0
    ms = int(round(seconds * 1000.0))
    h, rem = divmod(ms // 1000, 3600)
    m, s = divmod(rem, 60)
    return f"{h}:{m:02d}:{s:02d}.{ms % 1000:03d}"


def write_sidecar(wav_path, marks, order=None, loop=None, title=None, labels=None, out_path=None):
    """Write NAME.TXT next to NAME.WAV. Returns the sidecar path, or None if there was nothing to write."""
    if not marks:
        return None
    sidecar = out_path or (os.path.splitext(wav_path)[0] + ".TXT")
    lines = []
    if order or loop:
        directive = "#!bard"
        if order:
            directive += f" order={order}"
        if loop:
            directive += f" loop={loop}"
        lines.append(directive)
    if title:
        lines.append(f"# {title}")
    for i, t in enumerate(marks):
        label = ""
        if labels and i < len(labels) and labels[i]:
            label = re.sub(r"\s+", " ", labels[i]).strip()[:40]
        lines.append(f"{format_time(t)}   {label or f'mark {i + 1}'}")
    text = "\n".join(lines) + "\n"
    if len(text.encode()) > MAX_SIDECAR_BYTES:
        # read_text() reads only the first 4 KB and truncates SILENTLY, so an over-long sidecar would lose
        # its tail on the device with no diagnostic. Trim here, where we can say so.
        print(f"  warning: sidecar over {MAX_SIDECAR_BYTES} B; dropping labels to fit", file=sys.stderr)
        lines = [l.split("   ")[0] for l in lines]
        text = "\n".join(lines) + "\n"
        while len(text.encode()) > MAX_SIDECAR_BYTES and len(lines) > 1:
            lines.pop()
            text = "\n".join(lines) + "\n"
    with open(sidecar, "w") as f:
        f.write(text)
    return sidecar


# ---- card layout ---------------------------------------------------------------------------------

# Directory names that describe a FORMAT rather than a work. A LibriVox download is often unpacked as
# <book>/wav/, and taking the basename then labels the book "wav" - useless, since BOOKS.TXT is the only
# place a human can see what GOODEVIL.WAV actually is.
CONTAINER_DIRS = {"wav", "wavs", "mp3", "mp3s", "m4a", "m4b", "flac", "ogg", "opus", "aac",
                  "audio", "audiobook", "64kb", "128kb", "converted", "out", "output"}


def title_from_dir(directory):
    """Human-readable book title from a directory path, skipping format-named containers."""
    parts = [p for p in os.path.normpath(os.path.abspath(directory)).split(os.sep) if p]
    while parts and parts[-1].lower() in CONTAINER_DIRS:
        parts.pop()
    name = parts[-1] if parts else "book"
    return re.sub(r"[_-]+", " ", name).strip() or "book"


def shelf_dir(out_root, shelf):
    return os.path.join(out_root, "bard", str(shelf))


def book_name(index, keep_names, src):
    """8.3-safe destination filename: `NN.WAV` by default, or the source stem when it already fits."""
    if keep_names:
        stem = os.path.splitext(os.path.basename(src))[0]
        stem = re.sub(r"[^A-Za-z0-9_-]", "", stem).upper()[:8]
        if stem:
            return f"{stem}.WAV"
    return f"{index:02d}.WAV"


def read_titles(dir_path):
    """Existing BOOKS.TXT as {8.3 name: title}, or {} if there is none."""
    path = os.path.join(dir_path, "BOOKS.TXT")
    out = {}
    if not os.path.isfile(path):
        return out
    with open(path, "r", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(None, 1)
            if parts:
                out[parts[0]] = parts[1].strip() if len(parts) > 1 else ""
    return out


def write_titles(dir_path, rows):
    """BOOKS.TXT: the 8.3 name -> real title map. The firmware ignores it; the human needs it.

    MERGES with any existing map rather than replacing it. A shelf is usually filled by several runs (one
    per book, since --join takes one book at a time), and truncating would leave the card describing only
    whichever book was converted last - the map is the only place a human can see what 01.WAV actually is.
    """
    merged = read_titles(dir_path)
    merged.update(dict(rows))
    if not merged:
        return
    with open(os.path.join(dir_path, "BOOKS.TXT"), "w") as f:
        f.write("# bard shelf index: 8.3 filename -> original title\n")
        for name in sorted(merged):
            f.write(f"{name}  {merged[name]}\n")


def next_free_index(dir_path):
    """First NN unused by an existing NN.WAV in this shelf, so a second run cannot overwrite the first.

    Numbering restarted at 01 on every invocation, which silently clobbered earlier books when a shelf was
    filled incrementally.
    """
    used = set()
    if os.path.isdir(dir_path):
        for e in os.listdir(dir_path):
            stem, ext = os.path.splitext(e)
            if ext.lower() in (".wav", ".raw") and stem.isdigit():
                used.add(int(stem))
    i = 1
    while i in used:
        i += 1
    return i


def choose_marks(args, audio_path, source_path=None):
    """Pick the best available mark source and return (marks, labels, how).

    Priority, best first - the point being that guessing from silence is the LAST resort, because both of
    the common free sources carry exact boundaries already:
      1. an explicit chapter list (--marks-from)
      2. chapters embedded in the container (a LoyalBooks-style .m4b)
      3. silence detection (a single opaque file, e.g. one long MP3)
    JOIN boundaries outrank all of these and are handled by the caller, since only it knows them.
    """
    if args.no_marks:
        return [], [], "none (--no-marks)"
    if args.marks_from:
        return parse_mark_file(args.marks_from), [], f"the chapter list {os.path.basename(args.marks_from)}"
    if args.random:
        dur = duration_seconds(audio_path)
        sil = detect_silences(audio_path, args.noise_db, args.min_silence)
        if not sil:
            print(f"  warning: no silences detected, so the random marks cannot be snapped to pauses - "
                  f"they will land mid-word. Try --noise-db -35 --min-silence 0.5.", file=sys.stderr)
        seed = seed_for(audio_path, args.seed)
        marks = random_marks(dur, args.random, sil, args.min_gap, seed, args.snap_window)
        how = (f"{args.random} random marks (seed {seed}"
               f"{', snapped to ' + str(len(sil)) + ' detected pauses' if sil else ', UNSNAPPED'})")
        return marks, [], how

    probe_src = source_path or audio_path
    embedded = probe_chapters(probe_src)
    if embedded:
        dur = duration_seconds(audio_path)
        titles = chapter_titles(probe_src)
        marks = thin_marks(embedded, dur, args.min_gap, exact=True)
        return marks, titles, f"{len(embedded)} embedded chapter(s)"

    dur = duration_seconds(audio_path)
    raw = detect_silences(audio_path, args.noise_db, args.min_silence)
    marks = thin_marks(raw, dur, args.min_gap)
    if len(marks) <= 1:
        # Returning the lone start-of-book mark would be WORSE than returning nothing: a sidecar with one
        # mark counts as a real sidecar, so it suppresses the engine's deterministic auto-marks and leaves
        # the whole book as a single segment. Write no sidecar instead and say why.
        if not raw:
            print(f"  warning: silence detection found NO boundaries - the recording's noise floor is "
                  f"probably above --noise-db {args.noise_db:g}. Try a higher threshold "
                  f"(e.g. --noise-db -35 --min-silence 0.5).", file=sys.stderr)
        else:
            print(f"  warning: {len(raw)} boundaries found but --min-gap {args.min_gap:g}s thinned them to "
                  f"one. Lower --min-gap for short material (a poem wants ~15-20s).", file=sys.stderr)
        print("  warning: writing NO sidecar, so the engine will generate its own auto-marks instead of "
              "seeing a single useless mark.", file=sys.stderr)
        return [], [], "silence detection (nothing usable - falling back to the engine's auto-marks)"
    return marks, [], f"silence detection ({len(raw)} boundaries thinned to {len(marks)})"


def process_book(src, dst_dir, name, args, title):
    dst = os.path.join(dst_dir, name)
    print(f"  {os.path.basename(src)} -> {name}", flush=True)
    warn_if_upsampling(src, args.rate, {})
    convert(src, dst, args.rate)

    size = os.path.getsize(dst)
    if size < MIN_BOOK_BYTES:
        print(f"  warning: {name} is only {size} B; the engine's scan skips anything under "
              f"{MIN_BOOK_BYTES} B", file=sys.stderr)

    marks, labels, how = choose_marks(args, dst, src)
    print(f"    {len(marks)} mark(s) from {how}")
    if marks:
        sidecar = write_sidecar(dst, marks, args.order, args.loop, title, labels)
        if sidecar:
            print(f"    wrote {os.path.basename(sidecar)}")
    return name


def process_joined(sources, dst_dir, name, args, title):
    """One book out of many per-chapter files - the LibriVox shape."""
    dst = os.path.join(dst_dir, name)
    print(f"  joining {len(sources)} file(s) -> {name}", flush=True)
    with tempfile.TemporaryDirectory(prefix="bard-join-") as tmp:
        marks, labels = convert_join(sources, dst, args.rate, tmp)

    size = os.path.getsize(dst)
    minutes = size / float(args.rate * 2) / 60.0
    length = f"{minutes / 60.0:.1f} h" if minutes >= 60 else f"{minutes:.1f} min"
    print(f"    {size / (1024 * 1024):.1f} MiB, {length}")
    if size > 4 * 1024 ** 3:
        print(f"  warning: {name} exceeds FAT32's 4 GB per-file limit; split the chapters into parts "
              f"(adjacent books) or use a lower --rate", file=sys.stderr)

    if args.random:                                  # random scatter deliberately replaces the chapters
        print(f"  note: --random replaces the {len(marks)} exact chapter boundaries with a random scatter",
              file=sys.stderr)
        marks, labels, how = choose_marks(args, dst)
    elif args.marks_from:                            # an explicit list overrides the join points
        marks, labels, how = parse_mark_file(args.marks_from), [], \
            f"the chapter list {os.path.basename(args.marks_from)}"
    elif args.no_marks:
        marks, labels, how = [], [], "none (--no-marks)"
    else:
        marks = thin_marks(marks, None, args.min_gap, exact=True)
        how = "the join points (one per source file, exact)"
    print(f"    {len(marks)} mark(s) from {how}")
    if marks:
        sidecar = write_sidecar(dst, marks, args.order, args.loop, title, labels)
        if sidecar:
            print(f"    wrote {os.path.basename(sidecar)}")
    return name


def check_mark_flags(args):
    """Mark sources are mutually exclusive - silently letting one win would hide a typo'd intent."""
    chosen = [n for n, v in (("--no-marks", args.no_marks), ("--marks-from", args.marks_from),
                             ("--random", args.random)) if v]
    if len(chosen) > 1:
        die(f"{' and '.join(chosen)} are mutually exclusive - pick one source of marks")
    if args.random is not None and args.random < 1:
        die("--random needs a positive count")
    if args.random and args.random > MAX_MARKS:
        die(f"--random {args.random} exceeds the engine's {MAX_MARKS}-mark cap")


def cmd_convert(args, sources, join_title=None):
    need_tool("ffmpeg")
    need_tool("ffprobe")
    check_mark_flags(args)
    if not 0 <= args.shelf < MAX_SHELVES:
        die(f"--shelf must be 0..{MAX_SHELVES - 1}")
    if not args.join and len(sources) > MAX_BOOKS:
        die(f"{len(sources)} books but a shelf holds at most {MAX_BOOKS}. Use --join to make them one "
            f"book with a mark per chapter (this is what a LibriVox download wants).")

    dst_dir = shelf_dir(args.out, args.shelf)
    os.makedirs(dst_dir, exist_ok=True)
    print(f"shelf {args.shelf} -> {dst_dir}  ({args.rate} Hz, 16-bit mono)")

    rows = []
    start = next_free_index(dst_dir)
    if args.join:
        title = args.title or join_title or os.path.basename(sources[0])
        name = args.name or book_name(start, bool(join_title), join_title or sources[0])
        process_joined(sources, dst_dir, name, args, title)
        rows.append((name, f"{title} ({len(sources)} chapters joined)"))
    else:
        if args.name and len(sources) > 1:
            die("--name names a single book; drop it or pass one input")
        for i, src in enumerate(sources, start=start):
            name = args.name or book_name(i, args.keep_names, src)
            title = args.title if (args.title and len(sources) == 1) else os.path.basename(src)
            process_book(src, dst_dir, name, args, title)
            rows.append((name, title))
    write_titles(dst_dir, rows)
    print(f"done: {len(rows)} book(s) on shelf {args.shelf}")


def cmd_from_dir(args):
    if not os.path.isdir(args.directory):
        die(f"not a directory: {args.directory}")
    files = sorted(
        (os.path.join(args.directory, e) for e in os.listdir(args.directory)
         if not e.startswith(".") and os.path.splitext(e)[1].lower() in AUDIO_EXTS),
        key=natural_key)
    if not files:
        die(f"no audio files in {args.directory}")
    if len(files) > MAX_BOOKS and not args.join:
        print(f"note: {len(files)} files found. If these are the chapters of ONE book (a LibriVox "
              f"download), pass --join to make them one book with a mark per chapter.", file=sys.stderr)
    cmd_convert(args, files, join_title=args.title or title_from_dir(args.directory))


def cmd_marks(args):
    """Author a sidecar for a book already on the card, without touching the audio."""
    need_tool("ffmpeg")
    need_tool("ffprobe")
    check_mark_flags(args)
    if args.out and len(args.books) > 1:
        die("-o names a single sidecar; pass one book or drop -o")
    for path in args.books:
        if not os.path.isfile(path):
            die(f"not a file: {path}")
        print(f"{os.path.basename(path)}")
        marks, labels, how = choose_marks(args, path)
        print(f"  {len(marks)} mark(s) from {how}")
        sidecar = write_sidecar(path, marks, args.order, args.loop, None, labels,
                                out_path=getattr(args, "out", None))
        if sidecar:
            print(f"  wrote {sidecar}")


def build_parser():
    p = argparse.ArgumentParser(
        description="Prepare audiobooks for the bard engine (convert, 8.3-rename, author bookmarks).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Examples:", 1)[1] if "Examples:" in __doc__ else None)
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(sp, with_shelf=True):
        if with_shelf:
            sp.add_argument("--shelf", type=int, default=0, help=f"shelf 0..{MAX_SHELVES - 1} (default 0)")
            sp.add_argument("-o", "--out", default="out", help="card root (default: out)")
            sp.add_argument("--rate", type=int, default=DEFAULT_RATE,
                            help=f"output sample rate (default {DEFAULT_RATE}; 48000 for full bandwidth)")
            sp.add_argument("--keep-names", action="store_true",
                            help="keep 8.3-safe source basenames instead of numbering")
            sp.add_argument("--join", action="store_true",
                            help="concatenate the inputs into ONE book with a mark per chapter "
                                 "(what a LibriVox per-chapter MP3 set wants)")
            sp.add_argument("--name", metavar="8.3",
                            help="explicit 8.3 destination filename for the book (e.g. HOBBIT.WAV)")
            sp.add_argument("--title", metavar="TEXT",
                            help="real book title for BOOKS.TXT and the sidecar comment "
                                 "(default: derived from the folder name)")
        sp.add_argument("--no-marks", action="store_true", help="skip silence detection (engine auto-marks)")
        sp.add_argument("--random", type=int, metavar="N",
                        help="place N random marks across the book, each SNAPPED to the nearest detected "
                             "pause (stratified, so they cover the whole recording)")
        sp.add_argument("--seed", type=int, metavar="S",
                        help="seed for --random (default: derived from the filename, so a re-run "
                             "reproduces the same card)")
        sp.add_argument("--snap-window", type=float, metavar="S",
                        help="how far a random draw may move to reach a pause (default: half a stratum)")
        sp.add_argument("--marks-from", metavar="FILE", help="import timestamps from a chapter list")
        sp.add_argument("--noise-db", type=float, default=-40.0, help="silence threshold in dB (default -40)")
        sp.add_argument("--min-silence", type=float, default=1.2,
                        help="seconds of silence that counts as a boundary (default 1.2)")
        sp.add_argument("--min-gap", type=float, default=60.0,
                        help="minimum seconds between marks (default 60)")
        sp.add_argument("--order", choices=["file", "time", "shuffle"], help="write an order= directive")
        sp.add_argument("--loop", choices=["off", "segment", "book"], help="write a loop= directive")

    sp = sub.add_parser("convert", help="convert the named files onto a shelf")
    common(sp)
    sp.add_argument("files", nargs="+")

    sp = sub.add_parser("from-dir", help="convert every audio file in a directory onto a shelf")
    common(sp)
    sp.add_argument("directory")

    sp = sub.add_parser("marks", help="author a sidecar for a book already on the card")
    common(sp, with_shelf=False)
    sp.add_argument("-o", "--out", metavar="PATH",
                    help="write the sidecar here instead of beside the audio (single book only) - useful "
                         "for comparing mark sets without overwriting the one on the card")
    sp.add_argument("books", nargs="+")
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    if args.cmd == "convert":
        cmd_convert(args, args.files)
    elif args.cmd == "from-dir":
        cmd_from_dir(args)
    else:
        cmd_marks(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
