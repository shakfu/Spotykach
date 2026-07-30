#!/usr/bin/env python3
"""Build distributable engine firmware binaries for download-and-flash users.

Each "engine" is a separate firmware variant of the same device, selected at build
time via ENGINE= and flashed over DFU. This script does a clean build of each engine
in the release set, stamps the chosen version into the binary (so the artifact name
and the banner baked inside it agree), and collects the results under dist/<version>/
with SHA-256 checksums and RELEASE_NOTES.md (the CHANGELOG section for this version
followed by flashing instructions). It lets users who
do not have the ARM toolchain (and the cyfaust/gen-dsp venv the Faust/gen~ engines need)
download a ready-to-flash binary instead of building one.

The in-binary banner ("spotykach <version> engine=<engine>", see src/version.cpp) is the
provenance anchor: this script asserts every artifact contains the banner matching its
filename before trusting it, and the same banner lets anyone identify a stray binary
later (`arm-none-eabi-strings file.bin | grep '^spotykach '`).

Usage:
    python scripts/build_release.py [VERSION] [ENGINE ...] [--hex]

    VERSION   Version baked into every binary and used in artifact names and the output
              directory. Defaults to `git describe --tags --always` of the source tree.
              On a clean tagged checkout that is the bare tag (e.g. 0.3.0); pass it
              explicitly to override.
    ENGINE..  Engines to build. Defaults to $RELEASE_ENGINES (space-separated), else the
              curated mature set below.
    --hex     Also emit .hex artifacts (for ST-Link / STM32CubeProgrammer). Off by default:
              both documented flash paths (the Daisy Web Programmer and dfu-util) use .bin.

Examples:
    python scripts/build_release.py                # describe-derived version, mature set
    python scripts/build_release.py 0.3.0          # explicit version, mature set
    python scripts/build_release.py 0.3.0 reverb   # explicit version, just one engine
    python scripts/build_release.py 0.3.0 --hex    # also emit .hex alongside each .bin
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

# Curated "mature" engine set: the engines stable enough to publish prebuilt. Others stay
# buildable but unlisted. Override per-invocation with args or the RELEASE_ENGINES env var.
#
# Deliberately NOT published (all still build via `make ENGINE=<name>`):
#   - granular    : the stock/upstream spotykach firmware; releases ship engines NEW to sk-engines.
#   - passthrough : the minimal reference engine, not a playable instrument.
#   - chorus      : the simplest single-deck Faust demo (the dual-deck demos `filter`/`voice` ship instead).
#   - gigaverb    : the lone gen~ demo - left out until it is tested / optimized further (it builds fine,
#                   but sounded poor as a reverb voice and the better `reverb` engine covers reverb).
DEFAULT_ENGINES = [
    "bard",
    "chuck",
    "csound",
    "delay",
    "filter",
    "edrums",
    "graincloud",
    "mosc",
    "radio",
    "reso",
    "reverb",
    "shuttle",
    "tape",
    "voice",
]

REPO_ROOT = Path(__file__).resolve().parent.parent

# Engines that aren't a plain `ENGINE=<name>` build need extra make flags (the same ones their
# `make engine-<name>` one-shot target wraps). csound, chuck and mosc are QSPI apps: their code can't
# fit the 186 KB SRAM_EXEC budget, so they execute in place from QSPI flash (BOOT_QSPI + a QSPI linker
# script). csound/chuck link a pre-built static lib (scripts/fetch_{csound,chuck}.sh) for their ~2 MB /
# ~1.1 MB language runtimes; mosc has no prebuilt lib - it compiles the full 24-engine Plaits voice
# (~292 KB .text) from source and KEEPS the engine arena. These mirror the Makefile's CSOUND_FLAGS /
# CHUCK_FLAGS / MOSC_FLAGS exactly; chuck uses its OWN linker script (alt_qspi_chuck.lds reclaims the
# unused SRAM_EXEC region for .bss), while csound and mosc use the stock alt_qspi.lds.
ENGINE_MAKE_FLAGS = {
    "csound": ["APP_TYPE=BOOT_QSPI", "LDSCRIPT=alt_qspi.lds"],
    "chuck":  ["APP_TYPE=BOOT_QSPI", "LDSCRIPT=alt_qspi_chuck.lds"],
    "mosc":   ["APP_TYPE=BOOT_QSPI", "LDSCRIPT=alt_qspi.lds"],
}
# Files that must already exist for an engine to build (clear error vs a cryptic link failure).
ENGINE_PREREQUISITES = {
    "csound": [("thirdparty/csound/Daisy/lib/libcsound.a", "run scripts/fetch_csound.sh first")],
    "chuck":  [("thirdparty/chuck/Daisy/lib/libchuck.a",   "run scripts/fetch_chuck.sh first")],
}

# Filename prefix for the distributed artifacts (sk-<engine>-<version>.bin). Note this is the
# short product abbreviation and is intentionally distinct from the in-binary banner, which uses
# the full "spotykach" name (see src/version.cpp / banner_bytes).
ARTIFACT_PREFIX = "sk"

# DFU flash address / PID for the app, mirrored from lib/libDaisy/core/Makefile (BOOT_SRAM app
# type): the app is written to QSPI via the df11 DFU PID. Installing the bootloader itself is a
# separate, device-level procedure deliberately not documented here.
APP_ADDRESS = "0x90040000"
DFU_PID = "df11"

# Electrosmith's browser-based DFU flasher (WebUSB; Chrome/Edge only). The friendliest path for
# end users: no toolchain, just pick the .bin and click Flash. It handles the app address itself.
WEB_PROGRAMMER_URL = "https://flash.daisy.audio/"


def git_output(*args: str) -> str:
    """Return stripped stdout of a git command, or '' if git/command fails."""
    try:
        out = subprocess.run(
            ["git", *args], cwd=REPO_ROOT, capture_output=True, text=True, check=True
        )
        return out.stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return ""


def default_version() -> str:
    return git_output("describe", "--tags", "--always") or "dev"


def is_dirty() -> bool:
    """True if the working tree has uncommitted changes (so the build is not reproducible)."""
    return subprocess.run(["git", "diff", "--quiet"], cwd=REPO_ROOT).returncode != 0


def banner_bytes(version: str, engine: str) -> bytes:
    """The exact NUL-terminated banner literal src/version.cpp bakes into the binary."""
    return f"spotykach {version} engine={engine}".encode() + b"\x00"


def run_make(*args: str) -> None:
    """Run make in the repo root, surfacing output only on failure."""
    proc = subprocess.run(
        ["make", *args], cwd=REPO_ROOT, capture_output=True, text=True
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        raise SystemExit(f"make {' '.join(args)} failed (exit {proc.returncode})")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def build_engine(engine: str, version: str, jobs: int, out_dir: Path, emit_hex: bool = False) -> int:
    """Clean-build one engine, copy + verify its artifacts into out_dir, return .bin size.

    The .bin is always produced (both documented flash paths use it); the .hex is copied only
    when emit_hex is set, for users flashing via ST-Link / STM32CubeProgrammer.
    """
    print(f"==> building {engine}")
    for rel, fix in ENGINE_PREREQUISITES.get(engine, []):
        if not (REPO_ROOT / rel).exists():
            raise SystemExit(f"ERROR: {engine} needs {rel} - {fix}")
    run_make("clean")
    # SPK_VERSION makes the in-binary banner match the artifact name exactly. Some engines (csound)
    # add build flags via ENGINE_MAKE_FLAGS - the same ones `make engine-<name>` wraps.
    run_make(f"-j{jobs}", f"ENGINE={engine}", f"SPK_VERSION={version}", *ENGINE_MAKE_FLAGS.get(engine, []))

    base = f"{ARTIFACT_PREFIX}-{engine}-{version}"
    bin_path = out_dir / f"{base}.bin"
    shutil.copyfile(REPO_ROOT / "build" / "spotykach.bin", bin_path)
    if emit_hex:
        shutil.copyfile(REPO_ROOT / "build" / "spotykach.hex", out_dir / f"{base}.hex")

    # Sanity-check the baked-in banner before we trust the artifact.
    if banner_bytes(version, engine) not in bin_path.read_bytes():
        raise SystemExit(f"ERROR: {bin_path.name} is missing the expected version banner")

    return bin_path.stat().st_size


def write_manifest(path: Path, version: str, dirty: str, git_sha: str, sizes: dict[str, int]) -> None:
    lines = [
        "spotykach firmware release",
        f"version:    {version}{dirty}",
        f"git commit: {git_sha}",
        "note:       these apps require the spotykach bootloader already installed (see RELEASE_NOTES.md)",
        "",
        f"{'engine':<14} {'bytes':>12}  binary",
    ]
    for engine, size in sizes.items():
        lines.append(f"{engine:<14} {size:>12}  {ARTIFACT_PREFIX}-{engine}-{version}.bin")
    path.write_text("\n".join(lines) + "\n")


def write_checksums(out_dir: Path) -> None:
    """Write SHA256SUMS over every artifact, in `shasum -a 256 -c`-compatible format."""
    files = sorted(p for p in out_dir.iterdir() if p.suffix in (".bin", ".hex"))
    lines = [f"{sha256(p)}  {p.name}" for p in files]
    (out_dir / "SHA256SUMS").write_text("\n".join(lines) + "\n")


def changelog_section(version: str, changelog: Path | None = None) -> str | None:
    """Return the CHANGELOG.md body under `## [<version>]`, trimmed of blank edges.

    Falls back to `## [Unreleased]` when the version-named heading is absent or empty (e.g. a
    describe-derived version with no matching heading). Returns None when neither is found.
    (Ported from the former scripts/release_notes.py; `==` heading compare, so no regex pitfalls.)
    """
    changelog = changelog or (REPO_ROOT / "CHANGELOG.md")
    if not changelog.exists():
        return None
    text = changelog.read_text()

    def extract(name: str) -> str | None:
        body: list[str] = []
        in_section = False
        for line in text.splitlines():
            if line == f"## [{name}]":
                in_section = True
                continue
            if in_section and line.startswith("## ["):
                break
            if in_section:
                body.append(line)
        return "\n".join(body) if in_section else None

    section = extract(version)
    if not (section and section.strip()):
        section = extract("Unreleased")
    if not (section and section.strip()):
        return None
    lines = section.splitlines()
    while lines and not lines[0].strip():
        lines.pop(0)
    while lines and not lines[-1].strip():
        lines.pop()
    return "\n".join(lines)


def flashing_section(version: str, engines: list[str]) -> str:
    """The `## Flashing ...` section of the release notes (markdown)."""
    # csound, chuck and mosc are QSPI apps in the set (they execute from flash rather than being copied
    # to SRAM), which adds two flashing wrinkles the generic steps don't cover.
    qspi_engines = [e for e in ("csound", "chuck", "mosc") if e in engines]
    qspi_note = ("""
### Note for the {names} engine{plural}

{bins} {is_are} **QSPI app{plural}**: unlike the other engines {they} execute in place from QSPI flash
(their language runtimes are too big for SRAM), so {they} are also larger downloads and take longer to
flash. Same address and steps as above, with two caveats:

- {They_cap} need a bootloader that **runs QSPI apps** (the spotykach bootloader does); a SRAM-only
  bootloader cannot run {them}.
- With `dfu-util`, the `:leave` step may print a harmless `Error 74` / "get_status" message at the
  end - the write has already succeeded. Ignore it (the Web Programmer does not show this).
""".format(
        names=" and ".join(f"`{e}`" for e in qspi_engines),
        plural="s" if len(qspi_engines) > 1 else "",
        bins=" and ".join(f"`{ARTIFACT_PREFIX}-{e}-*.bin`" for e in qspi_engines),
        is_are="are" if len(qspi_engines) > 1 else "is",
        they="they" if len(qspi_engines) > 1 else "it",
        them="them" if len(qspi_engines) > 1 else "it",
        They_cap="They" if len(qspi_engines) > 1 else "It",
    ) if qspi_engines else "")

    return f"""## Flashing an sk-engines firmware ({version})

Each `.bin` here is a complete firmware for one engine. Flash exactly one at a time.

### Prerequisite

These app binaries are not standalone: they run under the spotykach bootloader, which must
already be installed on the device. Installing the bootloader is a separate, device-level
procedure not covered here.

### Step 1: enter bootloader mode

Both methods below need the device in its bootloader (DFU) mode first: hold Reset ~3s until
the bottom pad LEDs breathe white.

### Step 2, option A: Daisy Web Programmer (easiest)

Needs a WebUSB browser - Chrome or Edge (Firefox and Safari will not work).

1. With the device in bootloader mode, open {WEB_PROGRAMMER_URL}
2. On the "File Upload" tab, choose your engine binary ({ARTIFACT_PREFIX}-<engine>-{version}.bin).
3. Click FLASH.

### Step 2, option B: dfu-util (command line)

    dfu-util -a 0 -s {APP_ADDRESS}:leave -D {ARTIFACT_PREFIX}-<engine>-{version}.bin -d ,0483:{DFU_PID}

### Verify

- Confirm the download is intact:  `shasum -a 256 -c SHA256SUMS`
- Confirm what a binary is:        `arm-none-eabi-strings <file>.bin | grep '^spotykach '`
  (prints e.g. `spotykach {version} engine=reverb`)
{qspi_note}"""


def write_release_notes(path: Path, version: str, engines: list[str]) -> None:
    """Write RELEASE_NOTES.md: the CHANGELOG section for this version, then the flashing guide."""
    changelog = changelog_section(version)
    if changelog is None:
        sys.stderr.write(
            f"warning: no CHANGELOG section for '{version}' or '[Unreleased]' - "
            "release notes will note the changelog is missing\n"
        )
        changelog = "_No CHANGELOG entry for this release._"
    path.write_text(
        f"## Changes since the last Release\n\n{changelog}\n\n"
        f"{flashing_section(version, engines)}\n"
    )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build distributable, version-stamped engine firmware binaries.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "version", nargs="?", default=None,
        help="release version (default: git describe --tags --always)",
    )
    parser.add_argument(
        "engines", nargs="*",
        help="engines to build (default: $RELEASE_ENGINES, else the curated mature set)",
    )
    parser.add_argument(
        "--jobs", "-j", type=int, default=int(os.environ.get("JOBS", "8")),
        help="parallel make jobs (default: 8 or $JOBS)",
    )
    parser.add_argument(
        "--hex", action="store_true",
        help="also emit .hex artifacts (for ST-Link / STM32CubeProgrammer; the web flasher and "
             "dfu-util both use .bin, so .hex is omitted by default)",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    version = args.version or default_version()
    if args.engines:
        engines = args.engines
    else:
        env = os.environ.get("RELEASE_ENGINES", "").split()
        engines = env or DEFAULT_ENGINES

    git_sha = git_output("rev-parse", "--short", "HEAD") or "unknown"
    dirty = " (dirty tree - not a clean release build)" if is_dirty() else ""
    out_dir = REPO_ROOT / "dist" / version

    print(f"Release version : {version}{dirty}")
    print(f"Git commit      : {git_sha}")
    print(f"Engines         : {' '.join(engines)}")
    print(f"Output          : {out_dir.relative_to(REPO_ROOT)}/\n")

    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    sizes = {engine: build_engine(engine, version, args.jobs, out_dir, args.hex) for engine in engines}

    write_manifest(out_dir / "MANIFEST.txt", version, dirty, git_sha, sizes)
    write_release_notes(out_dir / "RELEASE_NOTES.md", version, engines)
    write_checksums(out_dir)

    print(f"\nDone. Artifacts in {out_dir.relative_to(REPO_ROOT)}/")
    for name in sorted(p.name for p in out_dir.iterdir()):
        print(f"  {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
