#!/usr/bin/env python3
"""The SD card layout: one machine-readable source of truth for every engine that reads the card.

Ten engines read the SD card and between them use NINE distinct directory layouts. They share ONE
audio format rule: the firmware converts sample depth and channel count as a file loads, but never the
sample RATE, and a headerless .raw states nothing about itself. So two questions live here and must
not be conflated - what the device will LOAD (`Accepts`, what `verify` predicts against) and what to
PUT on the card (`Fmt`, what `convert` writes). This module encodes both, so `sk_card.py` can build a
correct card (`init`), diagnose an existing one (`verify`), and place converted audio (`convert`)
without three separate copies of the rules drifting apart.

Every constant here is mirrored from firmware source, cited per entry. When the firmware changes,
change it here and `scripts/test_sk_card.py` will tell you what else moved.

Stdlib only, deliberately: `verify` must run for a user whose problem IS a broken toolchain, and
`init` feeds `make dist`, which is stdlib-only so plain python3 suffices with no venv.

Run as a script (`python3 scripts/card_layout.py --json`) it dumps the whole table as JSON. That
export is what the web front-end in `web/` consumes, so the layout is never typed twice: a hand-ported
JavaScript copy would reintroduce exactly the drift this module exists to prevent, and the
firmware-parity tests in `test_sk_card.py` only guard the Python.
"""

from __future__ import annotations

from dataclasses import dataclass, field

# --- the directory scanner's rules (src/hw/stream_deck.cpp:156-205) ----------------------------
#
# radio / bard / pstretch browse their folders with StreamDeck::scan_bank, which is far pickier than
# "the file is in the folder". Each of these is a real, silent way for a correctly-encoded file to be
# invisible to the device, so `verify` checks all of them.

SCAN_MAX_NAME = 12
"""Filenames longer than 12 chars are SKIPPED by the scan (`stream_deck.cpp:169`). Not truncated -
skipped. `The Hobbit Chapter 3.wav` simply does not exist as far as the device is concerned."""

SCAN_MIN_BYTES = 32 * 1024
"""Files under 32 KB are skipped (`kMinStationBytes`, `stream_deck.cpp:14`, applied at :175). This
exists to drop macOS AppleDouble `._NAME.raw` stubs, which are ~4 KB and would otherwise index as
bogus stations. Side effect: a real but very short clip (<0.34 s at 48k int16) is also invisible."""

SCAN_EXTENSIONS = ("raw", "wav")
"""Only these two extensions are indexed, case-insensitively (`stream_deck.cpp:171-174`)."""

SCAN_SKIP_DOT = True
"""Leading-dot names are skipped, as are FAT hidden/system files (`stream_deck.cpp:162-164`)."""

# --- audio format specs -----------------------------------------------------------------------
#
# TWO distinct questions live here, and conflating them is what produced the old four-format table:
#
#   `Accepts` - what the FIRMWARE WILL LOAD. Since the unified read path (docs/dev/unified-wav-reader.md)
#               this is one permissive rule per access pattern, not one per engine: any PCM depth the
#               device can decode, mono or multichannel, converted to the engine's own frames as it
#               loads. Used by `verify` to predict what the device will do with a file.
#
#   `Fmt`     - what `convert` WRITES, and what a README recommends. Deliberately narrow and unchanged:
#               the engine's native frame format, so a converted file takes the device's zero-conversion
#               fast path. Read wide, write narrow.
#
# A file that satisfies `Accepts` but is not the `Fmt` plays correctly - it just costs a main-loop
# conversion on the way to the ring. A file outside `Accepts` does not play at all.

WAV = "wav"
RAW = "raw"
TEXT = "text"

U8 = "u8"
INT16 = "int16"
INT24 = "int24"
INT32 = "int32"
F32 = "f32"

# Every encoding the firmware can decode, in the order pcm_convert.h names them. `verify` accepts all
# of these; `convert` only ever writes INT16 or F32.
DECODABLE = (U8, INT16, INT24, INT32, F32)

ANY_RATE = None

# The downmix bound: kPcmMaxChannels in src/memory/pcm_convert.h.
MAX_CHANNELS = 8

# Sanity bounds on a header's stated rate, mirrored from WavStreamReader::kRateMin/kRateMax (and the
# same pair in hw/card.cpp). Outside these the firmware refuses the file rather than divide by it.
MIN_RATE = 4000
MAX_RATE = 192000

_ENCODING_LABEL = {
    U8: "unsigned 8-bit PCM (WAV AudioFormat 1)",
    INT16: "signed 16-bit PCM (WAV AudioFormat 1)",
    INT24: "signed 24-bit PCM (WAV AudioFormat 1)",
    INT32: "signed 32-bit PCM (WAV AudioFormat 1)",
    F32: "32-bit IEEE float (WAV AudioFormat 3)",
}

_ENCODING_LABEL_BARE = {
    U8: "unsigned 8-bit PCM",
    INT16: "signed 16-bit PCM, little-endian",
    INT24: "signed 24-bit PCM, little-endian",
    INT32: "signed 32-bit PCM, little-endian",
    F32: "32-bit IEEE float",
}

_ENCODING_SHORT = {
    U8: "8-bit", INT16: "16-bit", INT24: "24-bit", INT32: "32-bit int", F32: "32-bit float",
}


@dataclass(frozen=True)
class Accepts:
    """What the firmware will actually LOAD for a bank - the rule `verify` predicts against."""

    containers: tuple[str, ...]
    encodings: tuple[str, ...] = ()
    max_channels: int = MAX_CHANNELS
    rate: int | None = None
    note: str = ""

    def describe(self) -> str:
        if TEXT in self.containers:
            return "plain text"
        parts = [" or ".join("headerless RAW" if c == RAW else "WAV" for c in self.containers)]
        parts.append(" / ".join(_ENCODING_SHORT[e] for e in self.encodings))
        parts.append("mono or stereo" if self.max_channels >= 2 else "mono")
        parts.append(f"{self.rate} Hz" if self.rate else "any sample rate")
        return ", ".join(parts)


@dataclass(frozen=True)
class Fmt:
    """The format `convert` writes for a bank: the engine's own frame format.

    Not an acceptance rule - see `Accepts`. This is what to PUT on the card: it needs no conversion on
    the device, and for the slot engines it is byte-identical to what they record.
    """

    container: str
    encodings: tuple[str, ...]
    channels: int | None = None
    rate: int | None = None
    note: str = ""

    def describe(self) -> str:
        if self.container == TEXT:
            return "plain text"
        parts = []
        parts.append("headerless RAW" if self.container == RAW else "WAV")
        # The "(WAV AudioFormat N)" hint is only meaningful for a container that HAS a format tag -
        # a headerless file states nothing about itself, which is the whole hazard of that format.
        label = _ENCODING_LABEL if self.container == WAV else _ENCODING_LABEL_BARE
        parts.append(" or ".join(label[e] for e in self.encodings))
        if self.channels == 1:
            parts.append("MONO")
        elif self.channels == 2:
            parts.append("STEREO")
        parts.append(f"{self.rate} Hz" if self.rate else "any sample rate")
        return ", ".join(parts)


# What the firmware LOADS. ONE rule for every audio engine now: depth, channel count and sample rate
# are all converted on the way in. The two names below differ only in container - the scanned banks
# also index headerless .raw - not in what they will accept from a WAV.
ACCEPT_WAV = Accepts(
    (WAV,), DECODABLE, rate=ANY_RATE,
    note="Depth, channel count and sample rate are all converted as the file loads, so any of these "
         "combinations plays correctly. Converting on a computer first is still worth it - see BEST "
         "below - but it is an optimisation now, not a requirement.")
ACCEPT_SCANNED = Accepts(
    (RAW, WAV), DECODABLE, rate=ANY_RATE,
    note="A .wav is converted as it loads, so any depth, channel count or rate works. A .raw is "
         "HEADERLESS - nothing in the file states its format - so it must be exactly 16-bit mono, at "
         "the rate fixed by convention (or radio/rate.txt).")
ACCEPT_TEXT = Accepts((TEXT,))

# What `convert` WRITES. These are the four narrow native formats; they did not change when the read
# path widened, and they are still what you want on a card.
# No note: the generic "BEST:" wording already says this is the deck's own recording format.
TARGET_TAPE = Fmt(WAV, (F32,), channels=1, rate=48000)
TARGET_GRANULAR = Fmt(WAV, (F32, INT16), channels=2, rate=48000,
                      note="The loop buffer is stereo, so a stereo file loads with no channel fold. "
                           "Either depth is written straight into the buffer's own storage width.")
TARGET_RADIO_RAW = Fmt(RAW, (INT16,), channels=1, rate=48000,
                       note="Headerless, for RadioMusic-compatible cards: nothing in the file states "
                            "its format, so the rate is fixed by convention (or radio/rate.txt).")
TARGET_SCAN_WAV = Fmt(WAV, (INT16,), channels=1, rate=ANY_RATE,
                      note="int16 mono is what these engines stream natively; the header carries the "
                           "rate, so an off-rate file plays at correct pitch.")
TARGET_TEXT = Fmt(TEXT, ())


# --- the layout -------------------------------------------------------------------------------


@dataclass(frozen=True)
class Bank:
    """One engine's card presence: the folders it reads and the rules for what goes in them.

    `kind` distinguishes the two access patterns, which have completely different failure modes:

    * ``slots``  - fixed filenames the engine opens directly (`tapes/tape_a_1.wav`). A misnamed file
                   is simply never opened; the slot reads as empty. NOT subject to the scan rules.
    * ``scanned``- a folder enumerated by StreamDeck::scan_bank. Subject to SCAN_* above, so a file
                   can be present, correctly encoded, and still invisible.
    """

    engine: str
    kind: str  # "slots" | "scanned" | "config"
    dirs: tuple[str, ...]
    fmt: Fmt        # what `convert` writes here (the native format)
    accepts: Accepts  # what the firmware will load here (the wider rule `verify` predicts against)
    slots: tuple[str, ...] = ()
    max_files: int | None = None
    max_seconds: float | None = None
    sidecars: tuple[str, ...] = ()
    source: str = ""
    blurb: str = ""
    extras: dict[str, str] = field(default_factory=dict)
    target: str = ""
    also_read_by: tuple[str, ...] = ()
    """Other engines that read these exact folders. `SK/{B,G,P,R,T,Y}` is not granular's private
    store - it is the PLATFORM's tape store, used by every engine declaring `CapTapeStorage`, so a
    second engine reading it is normal rather than exceptional. Recording it here keeps two questions
    answerable that a bare `engine` field cannot: whether content on the card is playable at all (see
    `readers`), and whether a folder can be renamed without breaking an engine nobody was thinking
    about."""

    @property
    def scanned(self) -> bool:
        return self.kind == "scanned"

    @property
    def readers(self) -> tuple[str, ...]:
        """Every engine that reads this bank's folders, the owning one first."""
        return (self.engine, *self.also_read_by)


# Where `convert` puts the Nth input file for a bank. A template rather than per-engine code because
# both front-ends need it and a JS copy would drift; the placeholder set is deliberately tiny so the
# formatter is three lines in any language:
#
#   {i}     the running index, bare        {i02}   the same, zero-padded to two digits
#   {deck}  a|b (tape/shuttle)             {bank}  0..15 (radio shelf / bard shelf)
#   {tape}  B|G|P|R|T|Y (granular)
#
# A bank with no template takes no audio at all (the text-patch banks, and the platform config entry).


def format_target(template: str, i: int, *, deck: str = "a", bank: int = 0, tape: str = "B") -> str:
    """Expand one `target` template. Mirrored by `formatTarget` in web/js/convert.js."""
    return (template.replace("{i02}", f"{i:02d}").replace("{i}", str(i))
                    .replace("{deck}", deck).replace("{bank}", str(bank)).replace("{tape}", tape))


def _slots(prefix: str, n: int = 8) -> tuple[str, ...]:
    """tape/shuttle slot filenames: tape_a_1.wav .. tape_b_8.wav (8 per deck, Alt+PITCH selects)."""
    return tuple(f"{prefix}_{deck}_{i}.wav" for deck in ("a", "b") for i in range(1, n + 1))


def _numbered(base: str, n: int) -> tuple[str, ...]:
    return tuple(f"{base}/{i}" for i in range(n))


GRANULAR_TAPES = ("B", "G", "P", "R", "T", "Y")
"""The six colour-coded tape folders under SK/ (docs/manual.md). Names are UPPERCASE."""

LAYOUT: tuple[Bank, ...] = (
    Bank(
        engine="granular",
        kind="slots",
        dirs=tuple(f"SK/{t}" for t in GRANULAR_TAPES),
        fmt=TARGET_GRANULAR,
        accepts=ACCEPT_WAV,
        slots=tuple(f"{i}.WAV" for i in range(1, 7)),
        target="SK/{tape}/{i}.WAV",
        # The folder name is the PLATFORM's, not granular's: `kRootDir = "SK"` in
        # src/memory/storage.cpp serves every engine with CapTapeStorage. That is why it is not named
        # after an engine, and why renaming it would reach further than it looks.
        also_read_by=("graincloud",),
        source="src/hw/card.cpp:61-66,131; src/memory/storage.cpp:14",
        blurb="Six colour-coded tapes (B G P R T Y), six slots each. Save with Alt+Play, load with Play. "
              "This is the platform's shared tape store, so the graincloud engine reads and writes the "
              "same folders.",
    ),
    Bank(
        engine="tape",
        kind="slots",
        dirs=("tapes",),
        fmt=TARGET_TAPE,
        accepts=ACCEPT_WAV,
        slots=_slots("tape"),
        target="tapes/tape_{deck}_{i}.wav",
        source="src/engine/tape/tape_engine.cpp:397",
        blurb="8 slots per deck, selected with Alt+PITCH. Streams from the card, so files can be any length.",
    ),
    Bank(
        engine="shuttle",
        kind="slots",
        dirs=("shuttle",),
        fmt=TARGET_TAPE,
        accepts=ACCEPT_WAV,
        slots=_slots("tape"),
        max_seconds=30.0,
        target="shuttle/tape_{deck}_{i}.wav",
        source="src/engine/shuttle/shuttle_engine.cpp:520",
        blurb="Same format and slot names as tape, but LOADED into RAM: ~30 s per track, longer files truncate.",
    ),
    Bank(
        engine="softcut",
        kind="slots",
        dirs=("softcut",),
        fmt=TARGET_TAPE,
        accepts=ACCEPT_WAV,
        slots=_slots("loop"),
        max_seconds=10.9,
        target="softcut/loop_{deck}_{i}.wav",
        source="src/engine/softcut/softcut_engine.cpp:635-644; softcut_engine.h:133,149",
        blurb="Loops you record on the device: Alt+Seq/Alt+Rev save to the slot picked with Alt+PITCH, "
              "Play loads it. Same format as tape, in its own folder so the two cannot overwrite each "
              "other; ~10.9 s per loop, the size of the buffer.",
    ),
    Bank(
        engine="radio",
        kind="scanned",
        dirs=_numbered("radio", 16),
        fmt=TARGET_RADIO_RAW,
        accepts=ACCEPT_SCANNED,
        max_files=48,
        sidecars=("radio/rate.txt",),
        target="radio/{bank}/{i02}.raw",
        source="src/engine/radio/radio_engine.cpp:306; radio_engine.h:107-108",
        blurb="16 banks of up to 48 stations. Headerless .raw at 48 kHz; a .wav is also accepted and "
              "carries its own rate.",
        extras={"radio/rate.txt": "48000\n"},
    ),
    Bank(
        engine="bard",
        kind="scanned",
        dirs=_numbered("bard", 16),
        fmt=TARGET_SCAN_WAV,
        accepts=ACCEPT_SCANNED,
        max_files=32,
        sidecars=("bard/BARD.CFG",),
        target="bard/{bank}/BOOK{i02}.WAV",
        source="src/engine/bard/bard_engine.cpp:872; bard_engine.h:131-132",
        blurb="16 shelves of up to 32 books. 16-bit MONO; 24 kHz is the right rate for speech (half the "
              "bytes per hour). Each BOOK.WAV may have a BOOK.TXT of bookmarks beside it, and BOOKS.TXT "
              "maps 8.3 names back to real titles.",
        extras={"bard/BARD.CFG": "resume=on\n"},
    ),
    Bank(
        engine="pstretch",
        kind="scanned",
        dirs=("pstretch",),
        fmt=TARGET_SCAN_WAV,
        accepts=ACCEPT_SCANNED,
        max_files=32,
        target="pstretch/CLIP{i02}.WAV",
        source="src/engine/pstretch/pstretch_engine.h:176-178",
        blurb="Source clips for the SD stretch source (Mode switch). 16-bit mono, any rate - off-rate "
              "clips are pitch-corrected. Long clips are ideal: at 50x a 3-minute file plays for ~2.5 h.",
    ),
    Bank(
        engine="csound",
        kind="slots",
        dirs=("csound",),
        fmt=TARGET_TEXT,
        accepts=ACCEPT_TEXT,
        slots=tuple(f"{i}.csd" for i in range(8)),
        source="src/engine/csound/csound_patch.h:34",
        blurb="Up to 8 .csd orchestras, selected with Alt+PITCH. The patch defines the sound, not the firmware.",
    ),
    Bank(
        engine="chuck",
        kind="slots",
        dirs=("chuck",),
        fmt=TARGET_TEXT,
        accepts=ACCEPT_TEXT,
        slots=tuple(f"{i}.ck" for i in range(8)),
        source="src/engine/chuck/chuck_patch.h:49",
        blurb="Up to 8 .ck programs, selected with Alt+PITCH. Compiled at runtime by the ChucK VM.",
    ),
    Bank(
        engine="platform",
        kind="config",
        dirs=("SK",),
        fmt=TARGET_TEXT,
        accepts=ACCEPT_TEXT,
        sidecars=("SK/config.txt",),
        source="src/memory/storage.h:22-23",
        blurb="Platform config and saved state. config.txt sets MIDI channels and boot preload; MEM is "
              "written by the device.",
    ),
)

BANKS = {b.engine: b for b in LAYOUT}

# --- rules the checkers share -------------------------------------------------------------------
#
# These live here rather than in sk_card.py because they are part of "what the card is", and both
# front-ends (the CLI and the web app) need them. Anything a checker branches on belongs in the JSON
# export, or the JS copy starts drifting from the Python one.

SOURCE_EXTENSIONS = (".mp3", ".flac", ".m4a", ".aac", ".ogg", ".opus", ".aiff", ".aif", ".wma", ".alac")
"""Extensions a user is likely to drop on the card untouched. None is readable by the firmware;
recognising them lets the diagnostic say "convert this" rather than "unexpected file"."""

CONFIG_PROPERTIES = {
    "mid_ch_a": (1, 16),
    "mid_ch_b": (1, 16),
    "mid_ps_a": (0, 1),
    "mid_ps_b": (0, 1),
    "pre_load": (0, 1),
}
"""SK/config.txt properties and their legal ranges (docs/manual.md; src/memory/storage.h)."""

SIDECAR_NAMES = ("README.TXT", "BOOKS.TXT", "RATE.TXT", "BARD.CFG", "CONFIG.TXT", "MEM")
"""Upper-cased names that are metadata, not audio, and so are never format-checked."""

SKIP_DIRS = ("System Volume Information", ".Spotlight-V100", ".Trashes", ".fseventsd")
"""Filesystem bookkeeping directories to walk past without comment."""

# The default config.txt, matching the table in docs/manual.md. One property name per line followed by
# its value on the NEXT line - not `key=value`, which is the easy thing to get wrong by hand.
DEFAULT_CONFIG = """mid_ch_a
1
mid_ch_b
2
mid_ps_a
0
mid_ps_b
0
pre_load
1
"""


def all_dirs() -> tuple[str, ...]:
    """Every directory a complete card contains, parents included, in creation order."""
    out: list[str] = []
    for bank in LAYOUT:
        for d in bank.dirs:
            parts = d.split("/")
            for i in range(1, len(parts) + 1):
                p = "/".join(parts[:i])
                if p not in out:
                    out.append(p)
    return tuple(out)


def bank_for_path(rel: str) -> Bank | None:
    """Which bank owns this card-relative path? Longest directory match wins, so `SK/B` resolves to
    granular rather than the `SK` platform entry."""
    best: Bank | None = None
    best_len = -1
    for bank in LAYOUT:
        for d in bank.dirs:
            if (rel == d or rel.startswith(d + "/")) and len(d) > best_len:
                best, best_len = bank, len(d)
    return best


def scan_name_ok(name: str) -> bool:
    """Would StreamDeck::scan_bank index a file with this name? Mirrors stream_deck.cpp:161-174."""
    if not name or name.startswith("."):
        return False
    if len(name) > SCAN_MAX_NAME:
        return False
    if "." not in name:
        return False
    return name.rsplit(".", 1)[1].lower() in SCAN_EXTENSIONS


def readme_for(bank: Bank, path: str) -> str:
    """The README.TXT dropped into each folder, so the rules arrive where the user is standing.

    Deliberately plain ASCII and short: this is read on a card reader in a file manager, possibly on
    Windows Notepad, by someone who has not read the repo docs.
    """
    lines = [
        f"{bank.engine.upper()} - what goes in this folder",
        "=" * 46,
        "",
        bank.blurb,
        "",
        f"ACCEPTS: {bank.accepts.describe()}",
    ]
    if bank.accepts.note:
        lines += ["", bank.accepts.note]
    if bank.fmt.container != TEXT:
        lines += ["", f"BEST: {bank.fmt.describe()}",
                  "That is what this engine works in natively, so it loads with no conversion.",
                  "`sk_card.py convert` writes exactly this."]
    if bank.fmt.note:
        lines += ["", bank.fmt.note]
    if bank.kind == "slots" and bank.slots:
        shown = ", ".join(bank.slots[:4])
        more = f", ... ({len(bank.slots)} total)" if len(bank.slots) > 4 else ""
        lines += ["", f"FILENAMES: exactly {shown}{more}",
                  "Any other name is never opened - the slot just reads as empty."]
    if bank.scanned:
        lines += [
            "",
            "THIS FOLDER IS SCANNED, so these rules also apply - break one and the file is",
            "INVISIBLE to the device, with no error shown:",
            f"  - name must be {SCAN_MAX_NAME} characters or fewer, including the extension",
            f"  - extension must be .raw or .wav",
            f"  - file must be at least {SCAN_MIN_BYTES // 1024} KB",
            "  - names starting with a dot are skipped (macOS ._* and .DS_Store)",
        ]
        if bank.max_files:
            lines += [f"  - at most {bank.max_files} files are indexed per folder"]
    if bank.max_seconds:
        lines += ["", f"LENGTH: about {bank.max_seconds:.0f} seconds max - longer files are truncated on load."]
    lines += [
        "",
        "Check a card you built with:   python3 scripts/sk_card.py verify /path/to/card",
        f"Firmware reference: {bank.source}",
        "",
    ]
    return "\r\n".join(lines)


def root_readme(demo: bool) -> str:
    """The card-root README.TXT: the one-screen orientation for someone holding a card reader."""
    lines = [
        "SPOTYKACH SD CARD",
        "=" * 46,
        "",
        "Format the card as FAT32 (up to 32 GB). Each folder here belongs to one engine;",
        "open its README.TXT for the exact audio format that folder needs.",
        "",
        "The firmware converts sample depth and channel count as it loads, but never the SAMPLE",
        "RATE - and a headerless .raw states no format at all, so a wrong one plays as noise.",
        "Check a card with:",
        "",
        "    python3 scripts/sk_card.py verify /path/to/card",
        "",
        "Add your own audio with:",
        "",
        "    python3 scripts/sk_card.py convert --engine tape /path/to/card mysound.mp3",
        "",
        "Folders:",
    ]
    for bank in LAYOUT:
        lines.append(f"  {bank.dirs[0]:<12} {bank.engine:<10} {bank.accepts.describe()}")
    if demo:
        lines += ["", "The audio on this card is synthesized placeholder content so that every engine",
                  "makes a sound out of the box. Replace it with your own."]
    lines.append("")
    return "\r\n".join(lines)


# --- JSON export ---------------------------------------------------------------------------------
#
# The web front-end (`web/`) consumes this instead of re-declaring the layout in JavaScript. Note what
# is exported: not just the table, but every piece of GENERATED TEXT too - the per-folder READMEs, the
# root README, the default config. Those are the parts a JS port would most plausibly reimplement and
# then let rot, and they are pure functions of the table, so shipping them as data means the browser
# builds a card that is byte-identical to `sk_card.py init` without owning a single line of the wording.
#
# What the web app must still implement itself is only the WAV header writer/parser and the verify
# walk - code, not content - and those are pinned to the Python by the fixtures under `web/test/`.

SCHEMA_VERSION = 1


def _fmt_dict(fmt: Fmt) -> dict:
    return {
        "container": fmt.container,
        "encodings": list(fmt.encodings),
        "channels": fmt.channels,
        "rate": fmt.rate,
        "note": fmt.note,
        "describe": fmt.describe(),
    }


def _accepts_dict(acc: Accepts) -> dict:
    return {
        "containers": list(acc.containers),
        "encodings": list(acc.encodings),
        "max_channels": acc.max_channels,
        "rate": acc.rate,
        "note": acc.note,
        "describe": acc.describe(),
    }


def _bank_dict(bank: Bank) -> dict:
    return {
        "engine": bank.engine,
        "kind": bank.kind,
        "scanned": bank.scanned,
        "dirs": list(bank.dirs),
        "fmt": _fmt_dict(bank.fmt),
        "accepts": _accepts_dict(bank.accepts),
        "slots": list(bank.slots),
        "max_files": bank.max_files,
        "max_seconds": bank.max_seconds,
        "sidecars": list(bank.sidecars),
        "source": bank.source,
        "blurb": bank.blurb,
        "extras": dict(bank.extras),
        "target": bank.target,
        "readers": list(bank.readers),
    }


def to_dict() -> dict:
    """The whole layout as a JSON-able dict. Stable key order; no timestamps, so the export is
    byte-reproducible and can be committed and diffed."""
    return {
        "schema": SCHEMA_VERSION,
        "generated_by": "scripts/card_layout.py --json",
        "scan": {
            "max_name": SCAN_MAX_NAME,
            "min_bytes": SCAN_MIN_BYTES,
            "extensions": list(SCAN_EXTENSIONS),
            "skip_dot": SCAN_SKIP_DOT,
        },
        # Every encoding the firmware can DECODE, so the browser can label what it found in a file.
        # `convert` still only ever writes f32 or int16 (see the TARGET_* formats).
        "encodings": {
            "u8": {"bits": 8, "wav_format": 1, "label": _ENCODING_LABEL[U8]},
            "int16": {"bits": 16, "wav_format": 1, "label": _ENCODING_LABEL[INT16]},
            "int24": {"bits": 24, "wav_format": 1, "label": _ENCODING_LABEL[INT24]},
            "int32": {"bits": 32, "wav_format": 1, "label": _ENCODING_LABEL[INT32]},
            "f32": {"bits": 32, "wav_format": 3, "label": _ENCODING_LABEL[F32]},
        },
        # What the on-device resampler will take. Outside this the firmware refuses the file.
        "rate_bounds": {"min": MIN_RATE, "max": MAX_RATE},
        "banks": [_bank_dict(b) for b in LAYOUT],
        "all_dirs": list(all_dirs()),
        "granular_tapes": list(GRANULAR_TAPES),
        "default_config": DEFAULT_CONFIG,
        "config_properties": {k: list(v) for k, v in CONFIG_PROPERTIES.items()},
        "source_extensions": list(SOURCE_EXTENSIONS),
        "sidecar_names": list(SIDECAR_NAMES),
        "skip_dirs": list(SKIP_DIRS),
        "readmes": {d: readme_for(b, d) for b in LAYOUT for d in b.dirs},
        "root_readme": {"demo": root_readme(True), "bare": root_readme(False)},
    }


def to_json() -> str:
    import json
    return json.dumps(to_dict(), indent=2, sort_keys=False, ensure_ascii=True) + "\n"


def main(argv: list[str] | None = None) -> int:
    import argparse
    import sys
    from pathlib import Path

    p = argparse.ArgumentParser(prog="card_layout.py", description=__doc__.split("\n")[0])
    p.add_argument("--json", action="store_true", help="dump the layout as JSON (for web/)")
    p.add_argument("-o", "--out", help="write to this file instead of stdout")
    args = p.parse_args(argv)
    if not args.json:
        p.error("nothing to do - pass --json (or use sk_card.py layout for the human-readable table)")
    text = to_json()
    if args.out:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text, encoding="ascii")
        print(f"{out} ({len(text)} bytes, {len(LAYOUT)} banks)", file=sys.stderr)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
