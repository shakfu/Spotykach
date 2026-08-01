#!/usr/bin/env python3
"""sk-card - build, check, and fill an SD card for the spotykach engines.

Ten engines read the card, using nine different directory layouts and four incompatible audio
formats (`python3 scripts/sk_card.py layout` prints them). The firmware converts nothing: a file in
the wrong format is not rejected, it is reinterpreted as garbage, and a file whose name is too long is
invisible to the directory scan with no error shown. That combination makes a hand-built card hard to
debug - the device's only feedback is an LED.

    sk_card.py init   CARD     build a complete, correct card (folders, configs, demo audio)
    sk_card.py verify CARD     check an existing card and explain anything that will not work
    sk_card.py convert ...     put your own audio on the card in the right place and format
    sk_card.py layout          print what every engine expects

`init` and `verify` are stdlib-only on purpose: `verify` has to run for someone whose problem IS a
broken toolchain, and `init` is called from `make sdcard` on the release path, which must work with a
plain python3 and no venv. Only `convert` needs a decoder, and it picks one at runtime.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import card_audio as ca
import card_layout as cl

REPO = Path(__file__).resolve().parent.parent


# ================================================================================================
# verify
# ================================================================================================


@dataclass
class Finding:
    level: str  # "error" (will not work) | "warn" (works, but probably not what you meant)
    path: str
    problem: str
    fix: str = ""


# Extensions a user is likely to drop on the card untouched. None of them is readable by the
# firmware; recognising them lets the diagnostic say "convert this" rather than "unexpected file".
# Declared in card_layout so the web front-end gets it from the JSON export rather than a second copy.
SOURCE_EXTENSIONS = set(cl.SOURCE_EXTENSIONS)


def _short_name_suggestion(f: Path) -> str:
    """An 8.3-safe name derived from the original, for the rename hint.

    Naive truncation produces junk like `THE .WAV` (trailing space, no information), so strip to
    alphanumerics first and fall back to a generic stem when nothing usable survives.
    """
    stem = "".join(c for c in f.stem if c.isalnum()).upper()[:8]
    return f"{stem or 'TRACK01'}{f.suffix.upper()}"


def _check_scan_visibility(rel: str, f: Path, bank: cl.Bank, out: list[Finding]) -> bool:
    """Rules from StreamDeck::scan_bank. Each of these makes a perfectly-encoded file INVISIBLE, and
    none of them produces any feedback on the device - which is why they are checked first and hard."""
    name = f.name
    ok = True
    if name.startswith("."):
        out.append(Finding("warn", rel, "name starts with a dot, so the scan skips it",
                           "This is usually a macOS metadata stub (._NAME or .DS_Store). Delete it; "
                           "on macOS use `dot_clean` on the card before ejecting."))
        return False
    if len(name) > cl.SCAN_MAX_NAME:
        ok = False
        out.append(Finding("error", rel,
                           f"filename is {len(name)} characters; the scan skips anything over "
                           f"{cl.SCAN_MAX_NAME}, so this file is INVISIBLE to the device",
                           f"Rename to {cl.SCAN_MAX_NAME} characters or fewer including the extension "
                           f"(e.g. {_short_name_suggestion(f)}). For a whole library, "
                           f"scripts/prepare_audiobooks.py does the renaming and records the real "
                           f"titles in BOOKS.TXT."))
    if "." not in name or f.suffix.lower().lstrip(".") not in cl.SCAN_EXTENSIONS:
        ok = False
        if f.suffix.lower() in SOURCE_EXTENSIONS:
            out.append(Finding("error", rel,
                               f"{f.suffix} is a compressed/unsupported source format - the firmware "
                               f"has no decoder, and the scan only indexes .raw/.wav",
                               f"Convert it: sk_card.py convert --engine {bank.engine} CARD {f.name}"))
        else:
            out.append(Finding("error", rel, f"extension {f.suffix or '(none)'} is not indexed by the scan",
                               f"Use .raw or .wav ({bank.fmt.describe()})."))
    try:
        size = f.stat().st_size
    except OSError:
        return False
    if size < cl.SCAN_MIN_BYTES:
        ok = False
        out.append(Finding("error", rel,
                           f"file is {size / 1024:.1f} KB; the scan skips anything under "
                           f"{cl.SCAN_MIN_BYTES // 1024} KB, so this file is INVISIBLE to the device",
                           "Make the clip longer (the floor exists to drop macOS metadata stubs, and "
                           "catches genuinely short clips too)."))
    return ok


def _check_audio_format(rel: str, f: Path, bank: cl.Bank, out: list[Finding]) -> None:
    """Does the file's actual encoding match what this bank's engine will read it as?"""
    fmt = bank.fmt
    if fmt.container == cl.RAW and f.suffix.lower() == ".raw":
        size = f.stat().st_size
        if size % 2:
            out.append(Finding("warn", rel, "odd byte count for a 16-bit format (last frame is partial)",
                               "Harmless - the firmware floors to a whole frame - but usually means the "
                               "file was truncated or is not actually int16."))
        return  # headerless: nothing else is checkable without guessing

    try:
        info = ca.parse_wav(f)
    except ca.WavError as e:
        out.append(Finding("error", rel, f"the firmware's WAV parser would reject this file: {e}",
                           "Re-encode it: sk_card.py convert --engine "
                           f"{bank.engine} CARD {f.name}"))
        return

    problems = []
    if info.encoding not in fmt.encodings:
        problems.append(f"encoding is {info.describe().split(',')[0]}")
    if fmt.channels is not None and info.channels != fmt.channels:
        problems.append(f"{info.channels} channel(s)")
    if fmt.rate is not None and info.rate != fmt.rate:
        problems.append(f"{info.rate} Hz")
    if problems:
        out.append(Finding("error", rel,
                           f"wrong format ({', '.join(problems)}) - the firmware reads the bytes as-is, "
                           f"so this plays as noise or not at all",
                           f"Needs: {fmt.describe()}. "
                           f"Fix with: sk_card.py convert --engine {bank.engine} CARD {f.name}"))
        return

    if bank.max_seconds and info.seconds > bank.max_seconds * 1.02:
        out.append(Finding("warn", rel,
                           f"{info.seconds:.0f} s exceeds the ~{bank.max_seconds:.0f} s this engine "
                           f"holds in RAM",
                           "It will load truncated. Trim it, or use the tape engine, which streams."))


def _check_slot_name(rel: str, f: Path, bank: cl.Bank, out: list[Finding]) -> bool:
    """Slot banks open exact filenames. A near-miss is never opened and the slot reads as empty."""
    names = {s.lower(): s for s in bank.slots}
    if f.name.lower() not in names:
        if f.suffix.lower() in SOURCE_EXTENSIONS:
            # The commonest newcomer mistake: copy the source file across and expect the device to
            # cope. Say what it actually needs rather than listing slot names at them.
            out.append(Finding("error", rel,
                               f"{f.suffix} is a compressed/unsupported source format - the firmware "
                               f"has no decoder and never opens this file",
                               f"Convert it: sk_card.py convert --engine {bank.engine} CARD {f.name}"))
        else:
            out.append(Finding("warn", rel, "not one of this engine's slot filenames, so it is never opened",
                               f"Expected one of: {', '.join(bank.slots[:6])}"
                               f"{' ...' if len(bank.slots) > 6 else ''}"))
        return False
    canonical = names[f.name.lower()]
    if f.name != canonical and canonical.isupper():
        out.append(Finding("warn", rel, f"name is {f.name}, documented as {canonical}",
                           "FAT is case-insensitive so this generally still opens, but match the "
                           "documented case to be safe."))
    return True


def _check_config(root: Path, out: list[Finding]) -> None:
    """SK/config.txt is a property name on one line and its value on the NEXT line - not key=value,
    which is the natural thing to write by hand and silently parses as nothing."""
    cfg = root / "SK" / "config.txt"
    if not cfg.exists():
        return
    rel = "SK/config.txt"
    lines = [ln.strip() for ln in cfg.read_text(errors="replace").splitlines() if ln.strip()]
    if any("=" in ln for ln in lines):
        out.append(Finding("error", rel, "looks like `key=value`, but the parser expects the property "
                                         "name and its value on separate lines",
                           "Write:\n    pre_load\n    1"))
        return
    known = cl.CONFIG_PROPERTIES
    for i in range(0, len(lines) - 1, 2):
        key, val = lines[i], lines[i + 1]
        if key not in known:
            out.append(Finding("warn", rel, f"unknown property {key!r}", f"Known: {', '.join(known)}"))
            continue
        lo, hi = known[key]
        if not val.lstrip("-").isdigit() or not (lo <= int(val) <= hi):
            out.append(Finding("error", rel, f"{key} = {val!r} is outside {lo}..{hi}",
                               f"Set a value in {lo}..{hi}."))
    if len(lines) % 2:
        out.append(Finding("warn", rel, "odd number of lines - the last property has no value",
                           "Every property name needs a value on the following line."))


def verify_card(root: Path) -> list[Finding]:
    """Walk the card and report everything that will not behave as the user expects."""
    out: list[Finding] = []
    if not root.is_dir():
        return [Finding("error", str(root), "not a directory", "Point at the card's root, e.g. /Volumes/SK.")]

    present = {d for d in cl.all_dirs() if (root / d).is_dir()}
    if not present:
        out.append(Finding("error", ".", "no recognised engine folders found here",
                           "Is this the card's root? Build a fresh one with: sk_card.py init CARD"))
        return out

    seen_banks: set[str] = set()
    for dirpath, dirnames, filenames in os.walk(root):
        d = Path(dirpath)
        rel_dir = d.relative_to(root).as_posix()
        if rel_dir == ".":
            rel_dir = ""
        # Skip the device's own state directory and FS bookkeeping.
        dirnames[:] = [x for x in dirnames if x not in cl.SKIP_DIRS]
        bank = cl.bank_for_path(rel_dir) if rel_dir else None
        counted = 0
        for name in sorted(filenames):
            f = d / name
            rel = f.relative_to(root).as_posix()
            if bank is None:
                if not rel.startswith(".") and rel_dir == "" and name.upper() != "README.TXT":
                    out.append(Finding("warn", rel, "file in the card root belongs to no engine",
                                       "Harmless, but the device never reads it."))
                continue
            seen_banks.add(bank.engine)
            if name.upper() in cl.SIDECAR_NAMES:
                continue
            if bank.fmt.container == cl.TEXT:
                if bank.slots and name.lower() not in {s.lower() for s in bank.slots}:
                    out.append(Finding("warn", rel, "not a slot the engine loads",
                                       f"Expected {', '.join(bank.slots)}"))
                continue
            if bank.scanned:
                if name.upper().endswith(".TXT"):
                    continue  # bard bookmark sidecars live beside the books
                if _check_scan_visibility(rel, f, bank, out):
                    _check_audio_format(rel, f, bank, out)
                    counted += 1
            else:
                if _check_slot_name(rel, f, bank, out):
                    _check_audio_format(rel, f, bank, out)
                    counted += 1
        if bank and bank.scanned and bank.max_files and counted > bank.max_files:
            out.append(Finding("warn", rel_dir,
                               f"{counted} playable files but only the first {bank.max_files} "
                               f"(alphabetically) are indexed",
                               f"Move the rest to another {bank.engine} folder."))

    _check_config(root, out)

    for bank in cl.LAYOUT:
        if bank.fmt.container == cl.TEXT or bank.engine in seen_banks:
            continue
        if any((root / d).is_dir() for d in bank.dirs):
            out.append(Finding("warn", bank.dirs[0], f"no files for the {bank.engine} engine",
                               f"{bank.blurb.split('.')[0]}."))
    return out


def cmd_verify(args: argparse.Namespace) -> int:
    root = Path(args.card)
    findings = verify_card(root)
    errors = [f for f in findings if f.level == "error"]
    warns = [f for f in findings if f.level == "warn"]

    print(f"sk-card verify: {root}")
    if not findings:
        print("\n  No problems found. Every file present is in a format the firmware accepts.")
        return 0
    for group, label in ((errors, "WILL NOT WORK"), (warns, "worth checking")):
        if not group:
            continue
        print(f"\n{label} ({len(group)}):")
        for f in group:
            print(f"\n  {f.path}")
            print(f"    {f.problem}")
            if f.fix:
                for i, line in enumerate(f.fix.splitlines()):
                    print(f"    {'-> ' if i == 0 else '   '}{line}")
    print(f"\n{len(errors)} error(s), {len(warns)} warning(s).")
    return 1 if errors and not args.lenient else 0


# ================================================================================================
# init
# ================================================================================================


def _demo_files(root: Path, quiet: bool) -> None:
    """Synthesized demo content, one or two files per engine, in each engine's exact format.

    Generated rather than sampled so the base card carries no licensing questions and `make sdcard`
    needs no decoder. Seeded, so the output is byte-reproducible and the release checksum is stable.
    """
    def say(msg: str) -> None:
        if not quiet:
            print(f"  {msg}")

    # granular - SK/<tape>/<slot>.WAV, 48k STEREO f32
    say("SK/B - granular demo tapes (48k stereo float)")
    ca.write_wav(root / "SK/B/1.WAV", ca.stereo(ca.tone(3.0, 220.0, 48000)), 48000, 2, ca.F32)
    ca.write_wav(root / "SK/B/2.WAV", ca.stereo(ca.noise_bed(3.0, 48000, seed=11)), 48000, 2, ca.F32)

    # tape - streams, so length is free. 48k MONO f32.
    say("tapes/ - tape demo loops (48k mono float)")
    ca.write_wav(root / "tapes/tape_a_1.wav", ca.pulse_pattern(6.0, 48000, seed=21), 48000, 1, ca.F32)
    ca.write_wav(root / "tapes/tape_b_1.wav", ca.sweep(6.0, 80.0, 4000.0, 48000), 48000, 1, ca.F32)

    # shuttle - same format, but RAM-capped, so stay well under 30 s.
    say("shuttle/ - shuttle demo tracks (48k mono float, short)")
    ca.write_wav(root / "shuttle/tape_a_1.wav", ca.tone(5.0, 165.0, 48000), 48000, 1, ca.F32)
    ca.write_wav(root / "shuttle/tape_b_1.wav", ca.pulse_pattern(5.0, 48000, seed=22), 48000, 1, ca.F32)

    # softcut - the same format again, into its own folder so it cannot overwrite a tape take. Loops
    # are normally recorded on the device; these just give Play something to load on a fresh card.
    # Well under the ~10.9 s buffer.
    say("softcut/ - softcut demo loops (48k mono float, short)")
    ca.write_wav(root / "softcut/loop_a_1.wav", ca.tone(4.0, 110.0, 48000, harmonics=4), 48000, 1, ca.F32)
    ca.write_wav(root / "softcut/loop_b_1.wav", ca.pulse_pattern(4.0, 48000, seed=23), 48000, 1, ca.F32)

    # radio - headerless int16 mono 48k, names <= 12 chars, each >= 32 KB.
    say("radio/0 - four demo stations (headerless int16 .raw)")
    for i, maker in enumerate((
        lambda: ca.tone(10.0, 196.0, 48000, harmonics=5),
        lambda: ca.noise_bed(10.0, 48000, seed=31),
        lambda: ca.sweep(10.0, 200.0, 2000.0, 48000),
        lambda: ca.pulse_pattern(10.0, 48000, seed=32),
    ), start=1):
        samples = ca.pad_to_bytes(maker(), cl.SCAN_MIN_BYTES, 2)
        ca.write_raw(root / f"radio/0/{i:02d}.raw", samples)

    # bard - 16-bit mono; 24k is the right rate for speech. Bookmarks + title map beside it.
    say("bard/0 - one demo book (24k mono int16) with bookmarks")
    book = ca.pad_to_bytes(ca.speech_like(45.0, 24000, seed=41), cl.SCAN_MIN_BYTES, 2)
    ca.write_wav(root / "bard/0/DEMO01.WAV", book, 24000, 1, ca.INT16)
    (root / "bard/0/DEMO01.TXT").write_text("0\n15\n30\n", encoding="ascii")
    (root / "bard/0/BOOKS.TXT").write_text("DEMO01.WAV  Demo book (synthesized placeholder)\n",
                                           encoding="ascii")

    # pstretch - 16-bit mono, any rate. Long source material is the point of the engine.
    say("pstretch/ - two demo clips (48k mono int16)")
    for i, maker in enumerate((
        lambda: ca.noise_bed(30.0, 48000, seed=51),
        lambda: ca.tone(30.0, 110.0, 48000, harmonics=6),
    ), start=1):
        samples = ca.pad_to_bytes(maker(), cl.SCAN_MIN_BYTES, 2)
        ca.write_wav(root / f"pstretch/CLIP{i:02d}.WAV", samples, 48000, 1, ca.INT16)


def build_card(root: Path, *, demo: bool = True, quiet: bool = False) -> None:
    """Create a complete, correct card at `root`."""
    def say(msg: str) -> None:
        if not quiet:
            print(msg)

    say(f"Building card at {root}")
    for d in cl.all_dirs():
        (root / d).mkdir(parents=True, exist_ok=True)

    # Per-folder README.TXT: the rules arrive where the user is standing, not in a repo doc they
    # will not read while holding a card reader.
    for bank in cl.LAYOUT:
        for d in bank.dirs:
            (root / d / "README.TXT").write_text(cl.readme_for(bank, d), encoding="ascii")

    (root / "SK/config.txt").write_text(cl.DEFAULT_CONFIG, encoding="ascii")
    for bank in cl.LAYOUT:
        for path, content in bank.extras.items():
            (root / path).write_text(content, encoding="ascii")
    say("  folder skeleton + README.TXT in every folder")

    # The example patches already in the repo are exactly what these two banks want.
    for engine, src_dir, pattern in (("chuck", REPO / "examples/chuck", "*.ck"),
                                     ("csound", REPO / "examples/csound", "*.csd")):
        if not src_dir.is_dir():
            continue
        slots = set(cl.BANKS[engine].slots)
        n = 0
        for f in sorted(src_dir.glob(pattern)):
            if f.name in slots:
                shutil.copy2(f, root / engine / f.name)
                n += 1
        say(f"  {engine}/ - {n} example patches from examples/{engine}/")

    if demo:
        say("  demo audio (synthesized):")
        _demo_files(root, quiet)

    # The wording lives in card_layout so the JSON export carries it and the web builder emits the
    # identical file rather than paraphrasing it.
    (root / "README.TXT").write_text(cl.root_readme(demo), encoding="ascii")
    say("Done.")


def cmd_init(args: argparse.Namespace) -> int:
    root = Path(args.card)
    if root.exists() and any(root.iterdir()) and not args.force:
        print(f"error: {root} is not empty. Re-run with --force to write into it anyway.",
              file=sys.stderr)
        return 2
    root.mkdir(parents=True, exist_ok=True)
    build_card(root, demo=not args.no_demo, quiet=args.quiet)
    return 0


# ================================================================================================
# convert
# ================================================================================================
#
# The only subcommand that needs to decode arbitrary user audio, and therefore the only one with a
# third-party dependency. Backends are probed at runtime and reported, so a failure names the tool.


class Backend:
    name = "?"

    def available(self) -> bool:
        raise NotImplementedError

    def supports(self, src: Path) -> bool:
        """Can this backend read `src`? Extension-level, cheap, and allowed to be optimistic - a
        backend that says yes and then fails is handled by the fallback chain in `decode_any`."""
        return True

    def decode(self, src: Path, rate: int, channels: int) -> list[float]:
        """Decode `src` to interleaved floats at the given rate/channel count."""
        raise NotImplementedError


class CysoxBackend(Backend):
    """In-process libsox via https://github.com/shakfu/cysox.

    Preferred when present: structured metadata and real exceptions instead of parsing another
    process's stderr. Note it needs SYSTEM libsox (libsox-dev / brew install sox) - it does not bundle
    it - and mp3 support depends on how that libsox was built (libsox-fmt-mp3 is a separate package on
    Debian), which is why ffmpeg stays in the list as a fallback.
    """

    name = "cysox"

    def available(self) -> bool:
        try:
            import cysox  # noqa: F401
        except Exception:
            return False
        return True

    def supports(self, src: Path) -> bool:
        """Ask libsox whether it has a handler for this extension ON THIS MACHINE.

        Necessary rather than fussy: libsox's format support is a build-time property, and mp3/flac/ogg
        are commonly absent (they are separate `libsox-fmt-*` packages on Debian). Probing means `auto`
        can use cysox where it genuinely works and fall back silently where it does not, instead of
        emitting a libsox error to stderr for every mp3 - which is most of what users have.
        """
        try:
            from cysox.sox import find_format
            return bool(find_format(src.suffix.lstrip(".").lower(), False))
        except Exception:
            return False

    def decode(self, src: Path, rate: int, channels: int) -> list[float]:
        import struct

        import cysox
        with tempfile.TemporaryDirectory() as td:
            # Decode to a headerless .f32 (raw 32-bit float), NOT to .wav. Asking libsox for a .wav
            # here yields 32-bit INTEGER PCM, which is the single most damaging format to hand this
            # device - it is bit-for-bit the "looks like float, plays as noise" trap. Raw floats have
            # no header to get wrong, and we re-encode through write_wav anyway.
            tmp = Path(td) / "out.f32"
            cysox.convert(str(src), str(tmp), sample_rate=rate, channels=channels)
            raw = tmp.read_bytes()
        n = len(raw) // 4
        return list(struct.unpack(f"<{n}f", raw[:n * 4]))


class _SubprocessBackend(Backend):
    def _run(self, cmd: list[str]) -> bytes:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if p.returncode != 0:
            msg = p.stderr.decode(errors="replace").strip().splitlines()
            raise RuntimeError(f"{self.name} failed: {msg[-1] if msg else 'unknown error'}")
        return p.stdout

    def available(self) -> bool:
        return shutil.which(self.name) is not None


class FfmpegBackend(_SubprocessBackend):
    name = "ffmpeg"

    def decode(self, src: Path, rate: int, channels: int) -> list[float]:
        raw = self._run(["ffmpeg", "-nostdin", "-loglevel", "error", "-i", str(src),
                         "-f", "f32le", "-ac", str(channels), "-ar", str(rate), "-"])
        import struct
        n = len(raw) // 4
        return list(struct.unpack(f"<{n}f", raw[:n * 4]))


class SoxBackend(_SubprocessBackend):
    name = "sox"

    def decode(self, src: Path, rate: int, channels: int) -> list[float]:
        raw = self._run(["sox", str(src), "-t", "f32", "-r", str(rate), "-c", str(channels), "-"])
        import struct
        n = len(raw) // 4
        return list(struct.unpack(f"<{n}f", raw[:n * 4]))


BACKENDS = {b.name: b for b in (CysoxBackend(), FfmpegBackend(), SoxBackend())}
BACKEND_ORDER = ("cysox", "ffmpeg", "sox")


def available_backends(preference: str = "auto") -> list[Backend]:
    """The backends to try, in order. An explicit --tool pins to exactly one."""
    if preference != "auto":
        b = BACKENDS[preference]
        if not b.available():
            raise SystemExit(f"error: backend {preference!r} is not available on this system.")
        return [b]
    found = [BACKENDS[n] for n in BACKEND_ORDER if BACKENDS[n].available()]
    if not found:
        raise SystemExit(
            "error: no audio decoder found. Install ONE of:\n"
            "  ffmpeg    (recommended - widest format support, and mp3 always works)\n"
            "  cysox     pip install cysox   (also needs system libsox: libsox-dev / brew install sox)\n"
            "  sox       the command-line binary")
    return found


def decode_any(backends: list[Backend], src: Path, rate: int, channels: int) -> tuple[list[float], str]:
    """Decode with the first backend that both claims the format and succeeds.

    Two stages on purpose: `supports()` skips a backend that is known not to handle this extension
    (avoiding a pointless error on stderr), and a raised exception still falls through to the next one,
    because format support is not always knowable up front.
    """
    errors: list[str] = []
    for b in sorted(backends, key=lambda b: not b.supports(src)):
        if not b.supports(src) and len(backends) > 1:
            errors.append(f"{b.name}: no handler for {src.suffix}")
            continue
        try:
            return b.decode(src, rate, channels), b.name
        except Exception as e:
            errors.append(f"{b.name}: {e}")
    raise RuntimeError("; ".join(errors) or "no backend could read it")


def _target_names(engine: str, count: int, args: argparse.Namespace) -> list[str]:
    """Where each input file lands, per the engine's naming rules.

    The rule is the bank's `target` template rather than a branch per engine, so the web front-end
    places files identically off the same JSON export instead of re-deriving six special cases.
    """
    bank = cl.BANKS[engine]
    if not bank.target:
        raise SystemExit(f"error: {engine} does not take audio files ({bank.fmt.describe()}).")
    return [cl.format_target(bank.target, i, deck=args.deck, bank=args.bank, tape=args.tape)
            for i in range(args.slot, args.slot + count)]


def cmd_convert(args: argparse.Namespace) -> int:
    engine = args.engine
    if engine not in cl.BANKS:
        raise SystemExit(f"error: unknown engine {engine!r}")
    bank = cl.BANKS[engine]
    if bank.fmt.container == cl.TEXT:
        raise SystemExit(f"error: the {engine} engine reads text patches, not audio - copy your "
                         f"{'.ck' if engine == 'chuck' else '.csd'} files into {bank.dirs[0]}/ directly.")

    root = Path(args.card)
    inputs = [Path(p) for p in args.inputs]
    missing = [p for p in inputs if not p.is_file()]
    if missing:
        raise SystemExit(f"error: no such file: {missing[0]}")

    backends = available_backends(args.tool)
    fmt = bank.fmt
    rate = fmt.rate or args.rate
    channels = fmt.channels or 1
    encoding = fmt.encodings[0]
    targets = _target_names(engine, len(inputs), args)

    print(f"sk-card convert: {engine} <- {len(inputs)} file(s)")
    print(f"  decoder: {', '.join(b.name for b in backends)}")
    print(f"  target format: {fmt.describe()}")

    failures = 0
    for src, rel in zip(inputs, targets):
        dst = root / rel
        try:
            samples, used = decode_any(backends, src, rate, channels)
        except Exception as e:  # backend-specific failures all surface the same way
            print(f"  FAILED {src.name}: {e}", file=sys.stderr)
            failures += 1
            continue

        if bank.max_seconds:
            cap = int(bank.max_seconds * rate * channels)
            if len(samples) > cap:
                print(f"  note: {src.name} trimmed to {bank.max_seconds:.0f} s "
                      f"({engine} loads into RAM)")
                samples = samples[:cap]

        if bank.scanned:
            bps = 2 if encoding == cl.INT16 else 4
            before = len(samples)
            samples = ca.pad_to_bytes(samples, cl.SCAN_MIN_BYTES, bps)
            if len(samples) > before:
                print(f"  note: {src.name} looped up to {cl.SCAN_MIN_BYTES // 1024} KB - shorter "
                      f"files are skipped by the directory scan")

        if fmt.container == cl.RAW:
            ca.write_raw(dst, samples, rate)
        else:
            ca.write_wav(dst, samples, rate, channels, encoding)
        via = f" [{used}]" if len(backends) > 1 else ""
        print(f"  {src.name} -> {rel} ({dst.stat().st_size / 1024:.0f} KB){via}")

    print("\nCheck the result with: sk_card.py verify " + str(root))
    return 1 if failures else 0


# ================================================================================================
# dist - package the base card as a release artifact
# ================================================================================================


def cmd_dist(args: argparse.Namespace) -> int:
    """Build the base card and zip it into dist/<version>/, beside the firmware binaries.

    Version comes from build_release.py's own helper rather than a second `git describe`, so the card
    and the binaries in the same dist/ directory can never disagree about what release they are. That
    matters because `make gh-release` globs `dist/<version>/*` - the card rides along automatically.
    """
    import zipfile

    version = args.version
    if not version:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import build_release
        version = build_release.default_version()

    out_dir = Path(args.out) if args.out else REPO / "dist" / version
    out_dir.mkdir(parents=True, exist_ok=True)
    zip_path = out_dir / f"sk-card-{version}.zip"

    with tempfile.TemporaryDirectory() as td:
        staging = Path(td) / "sk-card"
        build_card(staging, demo=not args.no_demo, quiet=args.quiet)
        files = sorted(p for p in staging.rglob("*") if p.is_file())
        # ZIP_DEFLATED matters more than usual here: synthesized tones are extremely compressible,
        # so the download is a fraction of the on-card size.
        #
        # Reproducibility: the CONTENT is already deterministic (seeded synthesis, no timestamps), but
        # a plain `z.write()` stamps each entry with the file's mtime, so two builds of the same commit
        # produce different bytes and a published SHA-256 means nothing. Writing explicit ZipInfos with
        # a fixed date makes the archive itself reproducible. 1980-01-01 is the DOS epoch, the earliest
        # a ZIP can represent.
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
            for p in files:
                info = zipfile.ZipInfo(p.relative_to(staging).as_posix(),
                                       date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                z.writestr(info, p.read_bytes())

    print(f"\nsk-card-{version}.zip  ({zip_path.stat().st_size / 1_000_000:.1f} MB compressed, "
          f"{len(files)} files)")
    print(f"  -> {zip_path}")
    return 0


# ================================================================================================
# layout
# ================================================================================================


def cmd_layout(_args: argparse.Namespace) -> int:
    print("What each engine expects on the SD card\n")
    for bank in cl.LAYOUT:
        dirs = bank.dirs[0] if len(bank.dirs) == 1 else f"{bank.dirs[0]} .. {bank.dirs[-1]}"
        print(f"{bank.engine.upper()}  ({dirs})")
        print(f"  format   {bank.fmt.describe()}")
        if bank.slots:
            shown = ", ".join(bank.slots[:5]) + (" ..." if len(bank.slots) > 5 else "")
            print(f"  names    {shown}")
        if bank.scanned:
            print(f"  scanned  name <= {cl.SCAN_MAX_NAME} chars, >= {cl.SCAN_MIN_BYTES // 1024} KB, "
                  f".raw/.wav, max {bank.max_files} per folder")
        if bank.max_seconds:
            print(f"  length   ~{bank.max_seconds:.0f} s max (loaded into RAM)")
        print(f"  source   {bank.source}\n")
    return 0


# ================================================================================================


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="sk_card.py", description=__doc__.split("\n")[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    pi = sub.add_parser("init", help="build a complete, correct card")
    pi.add_argument("card", help="destination directory (the card's root)")
    pi.add_argument("--no-demo", action="store_true", help="skeleton and configs only, no demo audio")
    pi.add_argument("--force", action="store_true", help="write into a non-empty directory")
    pi.add_argument("--quiet", action="store_true")
    pi.set_defaults(func=cmd_init)

    pv = sub.add_parser("verify", help="check a card and explain anything that will not work")
    pv.add_argument("card", help="the card's root directory")
    pv.add_argument("--lenient", action="store_true", help="always exit 0, even with errors")
    pv.set_defaults(func=cmd_verify)

    pc = sub.add_parser("convert", help="put your own audio on the card in the right format")
    pc.add_argument("--engine", required=True, help="target engine (tape, shuttle, radio, bard, "
                                                    "pstretch, granular)")
    pc.add_argument("card", help="the card's root directory")
    pc.add_argument("inputs", nargs="+", help="input audio files (any format the decoder can read)")
    pc.add_argument("--deck", choices=("a", "b"), default="a", help="tape/shuttle deck (default a)")
    pc.add_argument("--slot", type=int, default=1, help="first slot/index to write (default 1)")
    pc.add_argument("--bank", type=int, default=0, help="radio bank / bard shelf, 0..15 (default 0)")
    pc.add_argument("--tape", default="B", choices=list(cl.GRANULAR_TAPES), help="granular tape folder")
    pc.add_argument("--rate", type=int, default=48000, help="rate for banks that accept any (bard: 24000)")
    pc.add_argument("--tool", default="auto", choices=("auto",) + BACKEND_ORDER,
                    help="decoder backend (default auto: cysox, then ffmpeg, then sox)")
    pc.set_defaults(func=cmd_convert)

    pd = sub.add_parser("dist", help="package the base card as a release zip in dist/<version>/")
    pd.add_argument("version", nargs="?", help="version stamp (default: git describe, as build_release.py)")
    pd.add_argument("--out", help="output directory (default: dist/<version>)")
    pd.add_argument("--no-demo", action="store_true", help="skeleton and configs only, no demo audio")
    pd.add_argument("--quiet", action="store_true")
    pd.set_defaults(func=cmd_dist)

    pl = sub.add_parser("layout", help="print what every engine expects")
    pl.set_defaults(func=cmd_layout)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
