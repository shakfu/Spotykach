"""Unit tests for the pure helpers in build_release.py.

These cover the parts that do not need the ARM toolchain or a real firmware build: the
banner literal, checksum/manifest/flashing rendering, and argument parsing. The build
orchestration itself (run_make/build_engine) is exercised by actually running
`make dist`, not here.
"""

import hashlib

import pytest

import build_release as m


def test_banner_bytes_is_nul_terminated_literal():
    assert m.banner_bytes("0.3.0", "reverb") == b"spotykach 0.3.0 engine=reverb\x00"
    # The trailing NUL keeps it from matching as a prefix of a longer string.
    assert m.banner_bytes("0.3.0", "rev") not in m.banner_bytes("0.3.0", "reverb")


def test_sha256_matches_hashlib(tmp_path):
    f = tmp_path / "blob.bin"
    data = b"\x00\x01\x02" * 1000
    f.write_bytes(data)
    assert m.sha256(f) == hashlib.sha256(data).hexdigest()


def test_write_checksums_format_and_scope(tmp_path):
    # Only .bin/.hex are checksummed; other files (manifest etc.) are excluded.
    (tmp_path / "a.bin").write_bytes(b"aaa")
    (tmp_path / "b.hex").write_bytes(b"bbb")
    (tmp_path / "MANIFEST.txt").write_text("ignore me")

    m.write_checksums(tmp_path)
    lines = (tmp_path / "SHA256SUMS").read_text().splitlines()

    assert len(lines) == 2  # MANIFEST.txt excluded
    names = []
    for line in lines:
        digest, name = line.split("  ", 1)  # `shasum -a 256 -c` format: two spaces
        assert len(digest) == 64
        names.append(name)
    assert names == sorted(names)  # deterministic ordering
    assert "MANIFEST.txt" not in names


def test_write_checksums_digest_is_correct(tmp_path):
    (tmp_path / "a.bin").write_bytes(b"hello")
    m.write_checksums(tmp_path)
    digest, name = (tmp_path / "SHA256SUMS").read_text().split()
    assert name == "a.bin"
    assert digest == hashlib.sha256(b"hello").hexdigest()


def test_write_manifest_contents(tmp_path):
    path = tmp_path / "MANIFEST.txt"
    m.write_manifest(path, "0.3.0", " (dirty tree - not a clean release build)", "abc1234",
                     {"reverb": 175384, "delay": 153680})
    text = path.read_text()
    assert "version:    0.3.0 (dirty tree" in text
    assert "git commit: abc1234" in text
    assert "reverb" in text and "175384" in text
    assert "sk-delay-0.3.0.bin" in text


def test_flashing_section_has_address_and_version():
    text = m.flashing_section("0.3.0", [])
    assert m.APP_ADDRESS in text          # QSPI app address
    assert f",0483:{m.DFU_PID}" in text
    assert m.WEB_PROGRAMMER_URL in text   # web flasher is the recommended path
    assert "sk-<engine>-0.3.0.bin" in text


def test_flashing_section_no_bootloader_install_commands():
    text = m.flashing_section("0.3.0", [])
    # The bootloader-install procedure was deliberately removed (unverified / bricking-adjacent).
    assert "0x08000000" not in text
    assert "bootloader-spotykach-v2.bin" not in text


def test_flashing_section_csound_note_only_when_csound_present():
    assert "Note for the `csound` engine" not in m.flashing_section("0.3.0", ["reverb"])
    note = m.flashing_section("0.3.0", ["csound", "reverb"])
    assert "Note for the `csound` engine" in note
    assert "Error 74" in note             # the benign QSPI :leave message


def test_flashing_section_chuck_note():
    # chuck is a QSPI app too: it must get the same flashing note as csound.
    note = m.flashing_section("0.3.0", ["chuck", "reverb"])
    assert "Note for the `chuck` engine" in note
    assert "Error 74" in note


def test_flashing_section_both_qspi_engines_pluralized():
    note = m.flashing_section("0.3.0", ["csound", "chuck", "reverb"])
    assert "Note for the `csound` and `chuck` engines" in note   # plural heading, both named
    assert "sk-csound-*.bin` and `sk-chuck-*.bin` are **QSPI apps**" in note


def test_qspi_engines_have_boot_qspi_flags_and_prereqs():
    # The bug that crashed `make dist`: a QSPI engine in DEFAULT_ENGINES with no BOOT_QSPI make flags
    # gets built as a plain SRAM engine and overflows SRAM_EXEC. Every QSPI engine must carry the
    # right flags (its own linker script) and a prebuilt-lib prerequisite.
    expected = {
        "csound": "linker/alt_qspi.lds",
        "chuck":  "linker/alt_qspi_chuck.lds",
    }
    for engine, ldscript in expected.items():
        assert engine in m.DEFAULT_ENGINES
        flags = m.ENGINE_MAKE_FLAGS.get(engine, [])
        assert "APP_TYPE=BOOT_QSPI" in flags
        assert f"LDSCRIPT={ldscript}" in flags
        assert engine in m.ENGINE_PREREQUISITES


# --- CHANGELOG extraction (ported from the former release_notes.py) ----------------------------

CHANGELOG_SAMPLE = """\
# Changelog

## [Unreleased]

## [0.4.0]

### Added

- **Feature A.** did a thing.
- **Feature B.** did another.

## [0.3.2]

### Added

- old stuff
"""


def test_changelog_section_extracts_versioned_block(tmp_path):
    cl = tmp_path / "CHANGELOG.md"
    cl.write_text(CHANGELOG_SAMPLE)
    sec = m.changelog_section("0.4.0", cl)
    assert sec is not None
    assert "Feature A" in sec and "Feature B" in sec
    assert "old stuff" not in sec          # stops at the next `## [` heading
    assert sec.startswith("### Added")     # leading blank lines trimmed
    assert not sec.endswith("\n")          # trailing blank lines trimmed


def test_changelog_section_falls_back_to_unreleased(tmp_path):
    cl = tmp_path / "CHANGELOG.md"
    cl.write_text("# C\n\n## [Unreleased]\n\n- pending\n\n## [0.1.0]\n\n- released\n")
    # A version with no matching heading falls back to the Unreleased section.
    assert m.changelog_section("9.9.9", cl) == "- pending"


def test_changelog_section_missing_returns_none(tmp_path):
    cl = tmp_path / "CHANGELOG.md"
    cl.write_text("# C\n\n## [0.1.0]\n\n- released\n")     # no Unreleased, no 9.9.9
    assert m.changelog_section("9.9.9", cl) is None
    assert m.changelog_section("0.1.0", cl) == "- released"


def test_write_release_notes_order_changelog_then_flashing(tmp_path):
    # changelog_section reads the repo CHANGELOG, so assert structural invariants (not specific text):
    # the changelog section comes first, then the flashing section, with the csound note included.
    notes = tmp_path / "RELEASE_NOTES.md"
    m.write_release_notes(notes, "0.4.0", ["csound"])
    text = notes.read_text()
    assert "## Changes since the last Release" in text
    flashing = "## Flashing an sk-engines firmware (0.4.0)"
    assert flashing in text
    assert text.index("## Changes since the last Release") < text.index(flashing)
    assert "Note for the `csound` engine" in text


def test_parse_args_defaults():
    args = m.parse_args([])
    assert args.version is None
    assert args.engines == []
    assert args.jobs == 8
    assert args.hex is False  # .hex omitted unless explicitly requested


def test_parse_args_positional():
    args = m.parse_args(["0.3.0", "reverb", "delay"])
    assert args.version == "0.3.0"
    assert args.engines == ["reverb", "delay"]


def test_parse_args_hex_flag():
    assert m.parse_args(["0.3.0", "--hex"]).hex is True
    assert m.parse_args(["0.3.0", "reverb", "--hex"]).engines == ["reverb"]


def test_parse_args_jobs_flag():
    assert m.parse_args(["-j", "4"]).jobs == 4
    assert m.parse_args(["--jobs", "2"]).jobs == 2
