#!/usr/bin/env python3
"""Generate the web front-end's data files and its cross-language test fixtures.

The web app (`web/`) is a second front-end onto the SD card rules, not a second copy of them. Three
things have to cross the language boundary for that to be true, and this script writes all three:

1. **`web/card_layout.json`** - the layout table, the scan rules, the format specs, AND every piece of
   generated text (per-folder READMEs, the root README, the default config). Straight from
   `card_layout.to_dict()`.

2. **`web/patches.json`** - the `examples/{chuck,csound}` patches that `sk_card.py init` copies off
   disk. A static page cannot read the repo, so they ship as data.

3. **`web/test/fixtures/`** - the parity harness. Two kinds:
   * *format fixtures*: files written by `card_audio.write_wav`/`write_raw` plus the sample arrays
     that produced them, so the JS writers can be asserted byte-identical to the Python ones.
   * *verify cases*: a deliberately-broken card, together with the findings `sk_card.verify_card`
     reports for it, so the JS checker can be asserted to reach the same verdicts. This is the test
     that actually keeps the two front-ends from giving a user different answers.

Everything written here is committed, so the page is a static deploy with no build step. `make web-data`
regenerates it and `scripts/test_web_export.py` fails if the committed copy has drifted.

Stdlib only, like the rest of the card tooling.
"""

from __future__ import annotations

import base64
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import card_audio as ca  # noqa: E402
import card_layout as cl  # noqa: E402
import sk_card  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
WEB = REPO / "web"
FIXTURES = WEB / "test" / "fixtures"

# How much of each verify-case file to carry in the JSON. Only headers are ever parsed, so a 1 KB
# prefix plus the real byte count reproduces every check exactly while keeping a 32 KB-floor fixture
# from bloating the repo by 44 KB of base64 apiece.
HEAD_BYTES = 1024


# --- format fixtures -----------------------------------------------------------------------------
#
# Sample values are chosen to pin the parts of the encoders that are easy to get subtly wrong rather
# than to sound like anything: the clipping bounds, values that straddle a rounding boundary, and
# out-of-range inputs. The int16 path is the one that matters most - Python's `int(s * 32767.0)`
# truncates toward zero, so a JS port using Math.round would be off by one on about half of all
# samples and nobody would hear it until a card sounded slightly wrong.

EDGE_SAMPLES = [
    0.0, 1.0, -1.0, 0.5, -0.5, 0.25, -0.25,
    0.9999, -0.9999, 1.5, -1.5,          # clipped
    1e-05, -1e-05, 0.3333333333333333,   # sub-LSB and a repeating value
    0.7071067811865476, -0.7071067811865476,
]

STEREO_SAMPLES = [0.0, 1.0, -0.5, 0.5, 0.25, -0.25, 1.5, -1.5]  # interleaved L,R

FORMAT_FIXTURES = [
    {"name": "f32_mono_48k.wav", "kind": "wav", "samples": EDGE_SAMPLES,
     "rate": 48000, "channels": 1, "encoding": cl.F32},
    {"name": "int16_mono_24k.wav", "kind": "wav", "samples": EDGE_SAMPLES,
     "rate": 24000, "channels": 1, "encoding": cl.INT16},
    {"name": "f32_stereo_48k.wav", "kind": "wav", "samples": STEREO_SAMPLES,
     "rate": 48000, "channels": 2, "encoding": cl.F32},
    {"name": "int16_stereo_48k.wav", "kind": "wav", "samples": STEREO_SAMPLES,
     "rate": 48000, "channels": 2, "encoding": cl.INT16},
    {"name": "mono_48k.raw", "kind": "raw", "samples": EDGE_SAMPLES,
     "rate": 48000, "channels": 1, "encoding": cl.INT16},
]


def _write_format_fixtures(out: Path) -> list[dict]:
    manifest = []
    for spec in FORMAT_FIXTURES:
        path = out / spec["name"]
        if spec["kind"] == "raw":
            ca.write_raw(path, spec["samples"], spec["rate"])
        else:
            ca.write_wav(path, spec["samples"], spec["rate"], spec["channels"], spec["encoding"])
        manifest.append({**spec, "bytes": path.stat().st_size})
    return manifest


# --- parser fixtures -----------------------------------------------------------------------------
#
# Files the writers cannot produce but the PARSER must handle, because external encoders produce them
# and the firmware accepts them. Both are lifted from the cases in scripts/test_sk_card.py.


def _list_chunk_wav() -> bytes:
    """A `LIST` chunk before `data`, which pushes the body past the canonical offset 44."""
    import struct
    body = struct.pack("<4f", 0.0, 0.1, 0.2, 0.3)
    fmt = b"fmt " + struct.pack("<IHHIIHH", 16, 3, 1, 48000, 48000 * 4, 4, 32)
    meta = b"LIST" + struct.pack("<I", 10) + b"INFOhello\x00"
    data = b"data" + struct.pack("<I", len(body)) + body
    payload = b"WAVE" + fmt + meta + data
    return b"RIFF" + struct.pack("<I", len(payload)) + payload


def _extensible_wav() -> bytes:
    """WAVE_FORMAT_EXTENSIBLE (0xFFFE) with the real tag inside the GUID, as ffmpeg writes."""
    import struct
    body = struct.pack("<4h", 0, 100, -100, 0)
    ext = struct.pack("<HHI", 16, 1, 0) + b"\x01\x00" + b"\x00" * 14
    fmt = b"fmt " + struct.pack("<IHHIIHH", 40, 0xFFFE, 1, 48000, 96000, 2, 16) + ext
    data = b"data" + struct.pack("<I", len(body)) + body
    payload = b"WAVE" + fmt + data
    return b"RIFF" + struct.pack("<I", len(payload)) + payload


PARSER_FIXTURES = [
    {"name": "list_chunk.wav", "make": _list_chunk_wav,
     "expect": {"encoding": cl.F32, "channels": 1, "rate": 48000, "frames": 4, "past44": True}},
    # The extensible header is a 40-byte `fmt ` chunk, so its body also starts well past offset 44 -
    # a parser that assumed the canonical 44 would miss both of these files.
    {"name": "extensible.wav", "make": _extensible_wav,
     "expect": {"encoding": cl.INT16, "channels": 1, "rate": 48000, "frames": 4, "past44": True}},
]

BAD_FIXTURES = [
    {"name": "empty.bin", "bytes": b"", "why": "too short"},
    {"name": "not_riff.bin", "bytes": b"NOTARIFF" + b"\x00" * 40, "why": "not RIFF"},
    {"name": "no_data.bin", "bytes": b"RIFF" + b"\x00\x00\x00\x00" + b"WAVE", "why": "no data chunk"},
]


def _write_parser_fixtures(out: Path) -> tuple[list[dict], list[dict]]:
    good = []
    for spec in PARSER_FIXTURES:
        blob = spec["make"]()
        (out / spec["name"]).write_bytes(blob)
        info = ca.parse_wav(out / spec["name"])
        exp = dict(spec["expect"])
        # Assert the Python agrees with what the fixture claims, so a drifting parser is caught here
        # rather than showing up as a mysterious JS failure.
        assert info.encoding == exp["encoding"] and info.frames == exp["frames"], spec["name"]
        assert (info.data_offset > 44) == exp["past44"], spec["name"]
        good.append({"name": spec["name"], **exp, "data_offset": info.data_offset})
    bad = []
    for spec in BAD_FIXTURES:
        (out / spec["name"]).write_bytes(spec["bytes"])
        try:
            ca.parse_wav(out / spec["name"])
        except ca.WavError as e:
            bad.append({"name": spec["name"], "why": spec["why"], "message": str(e)})
        else:
            raise AssertionError(f"{spec['name']} should not parse")
    return good, bad


# --- verify cases --------------------------------------------------------------------------------
#
# A deliberately-broken card, built once and then run through the real `verify_card`. The findings it
# produces are shipped verbatim as the expected answer, so the JS checker is asserted against the
# Python's actual behaviour rather than against a second reading of the spec.


def _build_broken_card(root: Path) -> None:
    for d in cl.all_dirs():
        (root / d).mkdir(parents=True, exist_ok=True)

    floor = cl.SCAN_MIN_BYTES

    # --- correct files, which must produce NO findings -------------------------------------------
    ca.write_wav(root / "tapes/tape_a_1.wav", ca.tone(0.5, 220.0, 48000), 48000, 1, ca.F32)
    ca.write_wav(root / "SK/B/1.WAV", ca.stereo(ca.tone(0.5, 220.0, 48000)), 48000, 2, ca.F32)
    ca.write_wav(root / "pstretch/CLIP01.WAV",
                 ca.pad_to_bytes(ca.tone(0.5, 220.0, 48000), floor, 2), 48000, 1, ca.INT16)
    ca.write_raw(root / "radio/0/01.raw", ca.pad_to_bytes(ca.tone(0.5, 196.0, 48000), floor, 2))

    # --- wrong format: right encoding, wrong rate / channels / depth -----------------------------
    ca.write_wav(root / "tapes/tape_a_2.wav", [0.0] * 2048, 44100, 1, ca.F32)
    ca.write_wav(root / "tapes/tape_a_3.wav", [0.0] * 4096, 48000, 2, ca.F32)
    ca.write_wav(root / "tapes/tape_a_4.wav", [0.0] * 2048, 48000, 1, ca.INT16)

    # --- scan visibility -------------------------------------------------------------------------
    ca.write_wav(root / "bard/0/The Hobbit Chapter 3.wav",
                 ca.pad_to_bytes([0.0] * 100, floor, 2), 24000, 1, ca.INT16)   # name too long
    ca.write_wav(root / "pstretch/TINY.WAV", ca.tone(0.02, 220.0, 48000), 48000, 1, ca.INT16)  # < 32 KB
    (root / "radio/0/._01.raw").write_bytes(b"\x00" * 4096)                     # AppleDouble stub
    (root / "radio/0/station.txt").write_bytes(b"x" * (floor + 1))              # unindexed extension

    # --- unreadable / unsupported source ---------------------------------------------------------
    (root / "tapes/song.mp3").write_bytes(b"ID3\x04\x00\x00" + b"\x00" * 40000)
    (root / "pstretch/SONG.MP3").write_bytes(b"ID3\x04\x00\x00" + b"\x00" * 40000)
    # A real slot name whose bytes are not a WAV: exercises the parser-rejection branch, which a
    # non-slot name would never reach (the slot check fails first and returns).
    (root / "tapes/tape_b_1.wav").write_bytes(b"NOTARIFF" + b"\x00" * 40000)
    (root / "tapes/random_name.wav").write_bytes(b"\x00" * 40000)               # not a slot name

    # --- odd-length raw --------------------------------------------------------------------------
    (root / "radio/0/odd.raw").write_bytes(b"\x00" * (floor + 1))

    # --- config ----------------------------------------------------------------------------------
    (root / "SK/config.txt").write_text("mid_ch_a\n99\nnonsense\n1\npre_load\n", encoding="ascii")

    # --- stray root file -------------------------------------------------------------------------
    (root / "notes.txt").write_text("my notes\n", encoding="ascii")


def _entry(root: Path, path: Path) -> dict:
    blob = path.read_bytes()
    return {
        "path": path.relative_to(root).as_posix(),
        "size": len(blob),
        "head": base64.b64encode(blob[:HEAD_BYTES]).decode("ascii"),
    }


def _verify_cases(tmp: Path) -> dict:
    root = tmp / "card"
    _build_broken_card(root)
    findings = sk_card.verify_card(root)
    files = sorted((p for p in root.rglob("*") if p.is_file()),
                   key=lambda p: p.relative_to(root).as_posix())
    dirs = sorted(p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_dir())
    return {
        "note": "generated by scripts/web_export.py from scripts/sk_card.py verify_card",
        "head_bytes": HEAD_BYTES,
        "files": [_entry(root, p) for p in files],
        "dirs": dirs,
        "findings": [{"level": f.level, "path": f.path, "problem": f.problem, "fix": f.fix}
                     for f in findings],
    }


def _clean_card_case(tmp: Path) -> dict:
    """A card `sk_card.py init` just built. It must produce zero errors - a base card its own checker
    rejects would be worse than shipping nothing, and that has to hold in both languages."""
    root = tmp / "clean"
    sk_card.build_card(root, demo=True, quiet=True)
    files = sorted((p for p in root.rglob("*") if p.is_file()),
                   key=lambda p: p.relative_to(root).as_posix())
    dirs = sorted(p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_dir())
    findings = sk_card.verify_card(root)
    return {
        "note": "a freshly built card, from sk_card.build_card(demo=True)",
        "head_bytes": HEAD_BYTES,
        "files": [_entry(root, p) for p in files],
        "dirs": dirs,
        "findings": [{"level": f.level, "path": f.path, "problem": f.problem, "fix": f.fix}
                     for f in findings],
    }


# --- patches -------------------------------------------------------------------------------------


def _build_manifest(tmp: Path) -> dict:
    """Every file `sk_card.py init --no-demo` writes, by SHA-256.

    Hashes rather than contents because this is a byte-equality check on ~30 files that mostly repeat
    the same README boilerplate: the JS builder in web/src/core/build.ts has to produce the identical card,
    and a digest per path says so in a fixture small enough to read in a diff. Demo audio is excluded
    because the web app deliberately does not synthesize it (see web/src/core/build.ts).
    """
    import hashlib

    root = tmp / "skeleton"
    sk_card.build_card(root, demo=False, quiet=True)
    files = sorted((p for p in root.rglob("*") if p.is_file()),
                   key=lambda p: p.relative_to(root).as_posix())
    return {
        "note": "sha256 of every file written by sk_card.build_card(demo=False)",
        "dirs": list(cl.all_dirs()),
        "files": {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in files},
    }


def _patches() -> dict:
    """The example patches `sk_card.py init` copies, bundled so the page can write them too."""
    out = {}
    for engine, src_dir, pattern in (("chuck", REPO / "examples/chuck", "*.ck"),
                                     ("csound", REPO / "examples/csound", "*.csd")):
        if not src_dir.is_dir():
            continue
        slots = set(cl.BANKS[engine].slots)
        for f in sorted(src_dir.glob(pattern)):
            if f.name in slots:
                out[f"{engine}/{f.name}"] = f.read_text(encoding="utf-8")
    return out


# --- driver --------------------------------------------------------------------------------------


def _dump(path: Path, obj) -> str:
    text = json.dumps(obj, indent=2, ensure_ascii=True) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="ascii")
    return text


def generate(out_dir: Path = WEB, fixtures_dir: Path = FIXTURES) -> dict[str, int]:
    """Write every generated web asset. Returns {relative path: byte count} for reporting."""
    import tempfile

    written: dict[str, int] = {}
    written["card_layout.json"] = len(_dump(out_dir / "card_layout.json", cl.to_dict()))
    written["patches.json"] = len(_dump(out_dir / "patches.json", _patches()))

    fixtures_dir.mkdir(parents=True, exist_ok=True)
    formats = _write_format_fixtures(fixtures_dir)
    parser_ok, parser_bad = _write_parser_fixtures(fixtures_dir)
    written["test/fixtures/manifest.json"] = len(_dump(fixtures_dir / "manifest.json", {
        "note": "generated by scripts/web_export.py from scripts/card_audio.py",
        "formats": formats,
        "parses": parser_ok,
        "rejects": parser_bad,
    }))

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        written["test/fixtures/verify_cases.json"] = len(
            _dump(fixtures_dir / "verify_cases.json", _verify_cases(tmp)))
        written["test/fixtures/clean_card.json"] = len(
            _dump(fixtures_dir / "clean_card.json", _clean_card_case(tmp)))
        written["test/fixtures/build_manifest.json"] = len(
            _dump(fixtures_dir / "build_manifest.json", _build_manifest(tmp)))
    return written


def main(argv: list[str] | None = None) -> int:
    import argparse

    p = argparse.ArgumentParser(prog="web_export.py", description=__doc__.split("\n")[0])
    p.add_argument("--out", default=str(WEB), help="web/ directory to write into")
    p.add_argument("--quiet", action="store_true")
    args = p.parse_args(argv)

    out = Path(args.out)
    written = generate(out, out / "test" / "fixtures")
    if not args.quiet:
        for rel, n in written.items():
            print(f"  {out.relative_to(REPO) if out.is_relative_to(REPO) else out}/{rel}  ({n} bytes)")
        print(f"{len(written)} files written. Run `make test-web` to check the JS agrees.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
