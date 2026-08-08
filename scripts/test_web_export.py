"""Tests for the web front-end's generated data (`scripts/web_export.py`).

The web app treats the SD card rules as data rather than re-declaring them in JavaScript, which only
works if the exported JSON is complete and current. Two failure modes matter, and there is a test for
each:

1. **Incompleteness** - the export omits something the JS side needs (a bank field, a rendered README,
   the config property ranges), so the JS quietly falls back to a default or a hardcoded copy. Caught
   by asserting the export covers every declared bank, folder and rule.

2. **Drift** - `card_layout.py` changes and the committed `web/card_layout.json` is not regenerated, so
   the page checks cards against last release's rules while the CLI checks them against this one. This
   is the exact failure the JSON export exists to prevent, and it would be silent. Caught by
   regenerating into a temp directory and diffing against what is committed.

The JS side of the contract is asserted separately, by `make test-web`.
"""

import json
from pathlib import Path

import pytest

import card_layout as cl
import web_export

REPO = Path(__file__).resolve().parent.parent
WEB = REPO / "web"

GENERATED = [
    "card_layout.json",
    "patches.json",
    "test/fixtures/manifest.json",
    "test/fixtures/verify_cases.json",
    "test/fixtures/clean_card.json",
    "test/fixtures/build_manifest.json",
]


@pytest.fixture(scope="module")
def exported():
    return cl.to_dict()


# --- completeness --------------------------------------------------------------------------------


def test_export_declares_the_schema_the_app_checks_for(exported):
    # web/js/layout.js refuses a schema it does not understand rather than half-working against it.
    assert exported["schema"] == cl.SCHEMA_VERSION == 1


def test_export_round_trips_through_json(exported):
    assert json.loads(json.dumps(exported)) == exported


def test_every_bank_is_exported_with_the_fields_the_app_reads(exported):
    assert len(exported["banks"]) == len(cl.LAYOUT)
    needed = {"engine", "kind", "scanned", "dirs", "fmt", "slots", "max_files", "max_seconds",
              "sidecars", "source", "blurb", "extras", "target"}
    for bank in exported["banks"]:
        assert needed <= set(bank), f"{bank['engine']} is missing {needed - set(bank)}"
        assert {"container", "encodings", "channels", "rate", "note", "describe"} <= set(bank["fmt"])


def test_every_folder_gets_a_rendered_readme(exported):
    # The READMEs are exported as TEXT so the browser writes files byte-identical to `sk_card.py init`
    # without owning a line of the wording. A folder missing one would be built without its rules.
    for bank in cl.LAYOUT:
        for d in bank.dirs:
            assert exported["readmes"][d] == cl.readme_for(bank, d)
    assert len(exported["readmes"]) == sum(len(b.dirs) for b in cl.LAYOUT)


def test_the_scan_rules_and_config_ranges_travel_with_the_layout(exported):
    assert exported["scan"] == {
        "max_name": cl.SCAN_MAX_NAME,
        "min_bytes": cl.SCAN_MIN_BYTES,
        "extensions": list(cl.SCAN_EXTENSIONS),
        "skip_dot": cl.SCAN_SKIP_DOT,
    }
    assert exported["config_properties"]["mid_ch_a"] == [1, 16]
    assert set(exported["config_properties"]) == set(cl.CONFIG_PROPERTIES)
    assert ".mp3" in exported["source_extensions"]
    assert "System Volume Information" in exported["skip_dirs"]


def test_only_the_audio_banks_carry_a_target_template(exported):
    audio = {b["engine"] for b in exported["banks"] if b["target"]}
    assert audio == {"granular", "tape", "shuttle", "softcut", "radio", "bard", "pstretch"}
    for bank in exported["banks"]:
        if not bank["target"]:
            assert bank["fmt"]["container"] == cl.TEXT, bank["engine"]


def test_target_templates_expand_to_paths_inside_their_own_bank():
    # A template with a typo would place converted audio in a folder the engine never reads - silently,
    # since the file would be perfectly valid where it landed.
    for bank in cl.LAYOUT:
        if not bank.target:
            continue
        path = cl.format_target(bank.target, 1, deck="a", bank=0, tape=cl.GRANULAR_TAPES[0])
        assert any(path.startswith(d + "/") for d in bank.dirs), f"{bank.engine} -> {path}"


def test_scanned_bank_targets_produce_names_the_scan_would_actually_index():
    # The one that bites: a converter that writes a name over 12 characters puts a correct file on the
    # card that the device cannot see.
    for bank in cl.LAYOUT:
        if not (bank.scanned and bank.target):
            continue
        for i in (0, 9, 47):
            path = cl.format_target(bank.target, i, bank=15)
            assert cl.scan_name_ok(path.rsplit("/", 1)[-1]), f"{bank.engine} -> {path}"


def test_the_default_config_is_exported_and_parses_as_the_checker_expects(exported):
    import sk_card
    assert exported["default_config"] == cl.DEFAULT_CONFIG
    lines = [ln for ln in cl.DEFAULT_CONFIG.splitlines() if ln.strip()]
    assert len(lines) % 2 == 0, "every property needs a value on the following line"
    for i in range(0, len(lines), 2):
        lo, hi = cl.CONFIG_PROPERTIES[lines[i]]
        assert lo <= int(lines[i + 1]) <= hi
    assert sk_card.verify_card  # the checker this config must satisfy


def test_bundled_patches_are_all_real_slot_filenames():
    # examples/ also holds README.md and midi_in.ck, which no engine opens. Bundling them would make
    # the web builder produce a card its own checker warns about.
    patches = json.loads((WEB / "patches.json").read_text())
    assert patches, "no example patches were bundled"
    for path in patches:
        engine, name = path.split("/")
        assert name in cl.BANKS[engine].slots, path


# --- the committed copy is current ---------------------------------------------------------------


def test_the_committed_web_data_matches_a_fresh_export(tmp_path):
    """The drift guard. If this fails, run `make web-data`.

    Regenerating rather than comparing a stored hash means the failure message can say WHICH file
    moved, and the fixtures (which pin the JS to the Python) are checked by the same run.
    """
    web_export.generate(tmp_path, tmp_path / "test" / "fixtures")
    stale = []
    for rel in GENERATED:
        committed = (WEB / rel).read_bytes()
        fresh = (tmp_path / rel).read_bytes()
        if committed != fresh:
            stale.append(rel)
    assert stale == [], (
        f"web/ data is out of date with scripts/: {', '.join(stale)}. Run `make web-data`.")


def test_binary_fixtures_are_committed_and_current(tmp_path):
    web_export.generate(tmp_path, tmp_path / "test" / "fixtures")
    fixtures = WEB / "test" / "fixtures"
    stale = []
    for fresh in sorted((tmp_path / "test" / "fixtures").glob("*")):
        if fresh.suffix == ".json":
            continue
        committed = fixtures / fresh.name
        if not committed.exists() or committed.read_bytes() != fresh.read_bytes():
            stale.append(fresh.name)
    assert stale == [], f"web/test/fixtures is out of date: {', '.join(stale)}. Run `make web-data`."


# --- the fixtures are worth having ---------------------------------------------------------------


def test_the_broken_card_fixture_exercises_every_finding_shape():
    """A parity fixture that only covered easy cases would pass while the checkers disagreed on the
    hard ones. Assert the deliberately-broken card actually reaches each distinct diagnosis."""
    cases = json.loads((WEB / "test/fixtures/verify_cases.json").read_text())
    problems = " | ".join(f["problem"] for f in cases["findings"])
    for expected in [
        "characters; the scan skips",      # name too long for the scan
        "KB; the scan skips",              # under the 32 KB floor
        "starts with a dot",               # AppleDouble stub
        "compressed/unsupported source",   # an mp3 dropped on the card
        "WAV parser would reject",         # bytes that are not a WAV at all
        "wrong format",                    # right container, wrong rate/channels/depth
        "odd byte count",                  # truncated headerless raw
        "slot filenames",                  # a name the engine never opens
        "is outside 1..16",                # config value out of range
        "unknown property",                # config key that parses as nothing
        "odd number of lines",             # config property with no value
        "belongs to no engine",            # stray file in the card root
        "no files for the",                # an engine folder left empty
    ]:
        assert expected in problems, f"the fixture card never triggers {expected!r}"


def test_the_broken_card_fixture_also_contains_files_that_are_correct():
    # Parity on failures is only half of it: a checker that flagged everything would pass a
    # findings-only comparison. The fixture has to include files that must produce NO finding.
    cases = json.loads((WEB / "test/fixtures/verify_cases.json").read_text())
    flagged = {f["path"] for f in cases["findings"]}
    for good in ("tapes/tape_a_1.wav", "SK/B/1.WAV", "pstretch/CLIP01.WAV", "radio/0/01.raw"):
        assert good in {f["path"] for f in cases["files"]}, f"{good} missing from the fixture"
        assert good not in flagged, f"{good} is correct but the checker flagged it"


def test_the_clean_card_fixture_has_no_errors():
    cases = json.loads((WEB / "test/fixtures/clean_card.json").read_text())
    errors = [f for f in cases["findings"] if f["level"] == "error"]
    assert errors == [], "a freshly built card must pass its own checker"
    assert len(cases["files"]) > 50, "the clean fixture should be a full card, demo audio included"


def test_format_fixtures_cover_both_encodings_and_both_containers():
    manifest = json.loads((WEB / "test/fixtures/manifest.json").read_text())
    encodings = {f["encoding"] for f in manifest["formats"]}
    kinds = {f["kind"] for f in manifest["formats"]}
    assert encodings == {cl.F32, cl.INT16}
    assert kinds == {"wav", "raw"}
    assert {f["channels"] for f in manifest["formats"]} == {1, 2}


def test_format_fixture_samples_pin_the_clipping_and_truncation_rules():
    # These are the values a JS port gets wrong: out-of-range inputs (must clip, not wrap) and values
    # that straddle a rounding boundary (Python's int() truncates; Math.round would differ).
    manifest = json.loads((WEB / "test/fixtures/manifest.json").read_text())
    samples = manifest["formats"][0]["samples"]
    assert any(s > 1.0 for s in samples) and any(s < -1.0 for s in samples), "no clipping case"
    assert any(0.999 < s < 1.0 for s in samples), "no truncation-boundary case"


def test_parser_fixtures_cover_the_headers_external_encoders_actually_produce():
    manifest = json.loads((WEB / "test/fixtures/manifest.json").read_text())
    names = {p["name"] for p in manifest["parses"]}
    assert names == {"list_chunk.wav", "extensible.wav"}
    for p in manifest["parses"]:
        assert p["past44"], f"{p['name']} should have its body past the canonical offset 44"
    assert len(manifest["rejects"]) == 3
