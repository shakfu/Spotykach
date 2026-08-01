#!/usr/bin/env python3
"""The SD card layout: one machine-readable source of truth for every engine that reads the card.

Ten engines read the SD card and between them use EIGHT distinct directory layouts and FOUR
incompatible audio formats. The firmware does NO conversion on the audio path - it reads file body
bytes straight into frames - so a file in the wrong format is not rejected, it is reinterpreted as
garbage. This module encodes what each engine actually requires, so `sk_card.py` can build a correct
card (`init`), diagnose an existing one (`verify`), and place converted audio (`convert`) without
three separate copies of the rules drifting apart.

Every constant here is mirrored from firmware source, cited per entry. When the firmware changes,
change it here and `scripts/test_sk_card.py` will tell you what else moved.

Stdlib only, deliberately: `verify` must run for a user whose problem IS a broken toolchain, and
`init` feeds `make dist`, which is stdlib-only so plain python3 suffices with no venv.
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

WAV = "wav"
RAW = "raw"
TEXT = "text"

F32 = "f32"
INT16 = "int16"

ANY_RATE = None

_ENCODING_LABEL = {
    F32: "32-bit IEEE float (WAV AudioFormat 3)",
    INT16: "signed 16-bit PCM (WAV AudioFormat 1)",
}

_ENCODING_LABEL_BARE = {
    F32: "32-bit IEEE float",
    INT16: "signed 16-bit PCM, little-endian",
}


@dataclass(frozen=True)
class Fmt:
    """An audio format the firmware will actually accept for a given bank."""

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


# The four audio formats in play. Naming them makes the duplication visible: tape and shuttle share
# one, radio/bard/pstretch share another modulo rate, and granular is the lone stereo/permissive one.
FMT_TAPE = Fmt(WAV, (F32,), channels=1, rate=48000,
               note="32-bit INTEGER PCM is the classic mistake here - it is not float, and plays as noise.")
FMT_GRANULAR = Fmt(WAV, (F32, INT16), channels=2, rate=48000,
                   note="The one permissive bank: either depth is accepted and converted on the fly "
                        "(hw/card.cpp:61). Still must be STEREO at exactly 48 kHz.")
FMT_RADIO = Fmt(RAW, (INT16,), channels=1, rate=48000,
                note="Headerless: nothing in the file states its format, so the rate is fixed by "
                     "convention (or radio/rate.txt). A .wav here is also accepted by the scan and "
                     "carries its own rate - see FMT_SCAN_WAV.")
FMT_SCAN_WAV = Fmt(WAV, (INT16,), channels=1, rate=ANY_RATE,
                   note="The scan validates 16-bit MONO PCM and reads the rate from the header "
                        "(raw_stream.h:64), so an off-rate file plays at correct pitch.")
FMT_TEXT = Fmt(TEXT, ())


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
    fmt: Fmt
    slots: tuple[str, ...] = ()
    max_files: int | None = None
    max_seconds: float | None = None
    sidecars: tuple[str, ...] = ()
    source: str = ""
    blurb: str = ""
    extras: dict[str, str] = field(default_factory=dict)

    @property
    def scanned(self) -> bool:
        return self.kind == "scanned"


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
        fmt=FMT_GRANULAR,
        slots=tuple(f"{i}.WAV" for i in range(1, 7)),
        source="src/hw/card.cpp:61-66,131",
        blurb="Six colour-coded tapes (B G P R T Y), six slots each. Save with Alt+Play, load with Play.",
    ),
    Bank(
        engine="tape",
        kind="slots",
        dirs=("tapes",),
        fmt=FMT_TAPE,
        slots=_slots("tape"),
        source="src/engine/tape/tape_engine.cpp:397",
        blurb="8 slots per deck, selected with Alt+PITCH. Streams from the card, so files can be any length.",
    ),
    Bank(
        engine="shuttle",
        kind="slots",
        dirs=("shuttle",),
        fmt=FMT_TAPE,
        slots=_slots("tape"),
        max_seconds=30.0,
        source="src/engine/shuttle/shuttle_engine.cpp:520",
        blurb="Same format and slot names as tape, but LOADED into RAM: ~30 s per track, longer files truncate.",
    ),
    Bank(
        engine="radio",
        kind="scanned",
        dirs=_numbered("radio", 16),
        fmt=FMT_RADIO,
        max_files=48,
        sidecars=("radio/rate.txt",),
        source="src/engine/radio/radio_engine.cpp:306; radio_engine.h:107-108",
        blurb="16 banks of up to 48 stations. Headerless .raw at 48 kHz; a .wav is also accepted and "
              "carries its own rate.",
        extras={"radio/rate.txt": "48000\n"},
    ),
    Bank(
        engine="bard",
        kind="scanned",
        dirs=_numbered("bard", 16),
        fmt=FMT_SCAN_WAV,
        max_files=32,
        sidecars=("bard/BARD.CFG",),
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
        fmt=FMT_SCAN_WAV,
        max_files=32,
        source="src/engine/pstretch/pstretch_engine.h:176-178",
        blurb="Source clips for the SD stretch source (Mode switch). 16-bit mono, any rate - off-rate "
              "clips are pitch-corrected. Long clips are ideal: at 50x a 3-minute file plays for ~2.5 h.",
    ),
    Bank(
        engine="csound",
        kind="slots",
        dirs=("csound",),
        fmt=FMT_TEXT,
        slots=tuple(f"{i}.csd" for i in range(8)),
        source="src/engine/csound/csound_patch.h:34",
        blurb="Up to 8 .csd orchestras, selected with Alt+PITCH. The patch defines the sound, not the firmware.",
    ),
    Bank(
        engine="chuck",
        kind="slots",
        dirs=("chuck",),
        fmt=FMT_TEXT,
        slots=tuple(f"{i}.ck" for i in range(8)),
        source="src/engine/chuck/chuck_patch.h:49",
        blurb="Up to 8 .ck programs, selected with Alt+PITCH. Compiled at runtime by the ChucK VM.",
    ),
    Bank(
        engine="platform",
        kind="config",
        dirs=("SK",),
        fmt=FMT_TEXT,
        sidecars=("SK/config.txt",),
        source="src/memory/storage.h:22-23",
        blurb="Platform config and saved state. config.txt sets MIDI channels and boot preload; MEM is "
              "written by the device.",
    ),
)

BANKS = {b.engine: b for b in LAYOUT}

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
        f"FORMAT: {bank.fmt.describe()}",
    ]
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
