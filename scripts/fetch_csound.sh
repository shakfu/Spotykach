#!/usr/bin/env bash
#
# Fetch + cross-build Csound 7 (with its official Daisy port) into thirdparty/csound, which the
# ENGINE=csound build links (libcsound.a) and includes (csound API headers). thirdparty/csound is
# gitignored (~115 MB of source), so this reproduces it from upstream instead of vendoring it.
#
# Produces:
#   thirdparty/csound/Daisy/lib/libcsound.a        <- linked by the Makefile csound branch
#   thirdparty/csound/Daisy/include/csound/*.h      <- the -I include path
#
# Usage:
#   scripts/fetch_csound.sh            # fetch the source + wire the symlinks + cross-build libcsound.a
#   scripts/fetch_csound.sh --no-build # just fetch + link (build later)
#
# By default it downloads the GitHub release source tarball for a pinned tag (no git history, fast,
# the same archive you'd grab from the releases page). It falls back to `git clone` for a non-GitHub
# repo or when curl/tar are unavailable.
#
# Env overrides:
#   CSOUND_REPO  upstream repo            (default: https://github.com/csound/csound)
#   CSOUND_REF   tag / branch / commit    (default: 7.0.0-beta.16 - a pinned release tag known to build
#                                          + work here; bump it as newer Csound 7 betas are validated,
#                                          or set CSOUND_REF=develop to track the moving branch)
#   JOBS         parallel build jobs      (default: CPU count)
#
# Prerequisites: cmake, the arm-none-eabi GCC toolchain, and either curl+tar or git.
#   https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads  or the Daisy toolchain
#   https://daisy.audio/tutorials/cpp-dev-env/#1-install-the-toolchain
#
# See docs/dev/csound.md ("Building libcsound.a") and thirdparty/csound/Daisy/BUILD.md.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tp="$repo_root/thirdparty/csound"

CSOUND_REPO="${CSOUND_REPO:-https://github.com/csound/csound}"
CSOUND_REF="${CSOUND_REF:-7.0.0-beta.17}"
JOBS="${JOBS:-$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

do_build=1
[ "${1:-}" = "--no-build" ] && do_build=0

die() { echo "error: $*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# --- prerequisites -----------------------------------------------------------------------------
have cmake || die "cmake not found (https://cmake.org)"
if [ "$do_build" = 1 ]; then
    have arm-none-eabi-gcc || die "arm-none-eabi-gcc not found - install the ARM toolchain (see the header)"
fi
{ have curl && have tar; } || have git || die "need curl+tar or git to fetch Csound"

# The Daisy port links the repo's vendored libDaisy + DaisySP (via the symlinks below); they are git
# submodules of sk-engines, so make sure they are checked out.
for d in lib/libDaisy lib/DaisySP; do
    [ -e "$repo_root/$d/Makefile" ] || die "$repo_root/$d is empty - run: git submodule update --init --recursive"
done

# --- 1) source ---------------------------------------------------------------------------------
download_csound() {
    # GitHub source tarball first (matches the release download). Try the ref as a tag, then a branch,
    # then a raw commit - the auto-generated archive endpoint differs for each.
    if have curl && have tar; then
        local tmp p; tmp="$(mktemp -d)"
        for p in "refs/tags/$CSOUND_REF" "refs/heads/$CSOUND_REF" "$CSOUND_REF"; do
            echo "    trying $CSOUND_REPO/archive/$p.tar.gz"
            if curl -fsSL "$CSOUND_REPO/archive/$p.tar.gz" -o "$tmp/cs.tgz"; then
                tar -xzf "$tmp/cs.tgz" -C "$tmp"
                local d; d="$(find "$tmp" -maxdepth 1 -type d -name 'csound-*' | head -1)"
                if [ -n "$d" ]; then mv "$d" "$tp"; rm -rf "$tmp"; return 0; fi
            fi
        done
        rm -rf "$tmp"
    fi
    # Fallback: git clone (non-GitHub repo, or curl/tar unavailable).
    if have git; then
        echo "    tarball unavailable; cloning with git"
        git clone --depth 1 --single-branch --branch "$CSOUND_REF" "$CSOUND_REPO" "$tp" 2>/dev/null && return 0
        git clone --filter=blob:none "$CSOUND_REPO" "$tp" && git -C "$tp" checkout "$CSOUND_REF" && return 0
    fi
    return 1
}

if [ -f "$tp/CMakeLists.txt" ]; then
    echo "==> thirdparty/csound already present (skipping fetch; 'rm -rf $tp' to re-fetch)"
else
    echo "==> fetching csound $CSOUND_REF from $CSOUND_REPO"
    mkdir -p "$(dirname "$tp")"
    download_csound || die "could not fetch csound $CSOUND_REF"
fi
[ -f "$tp/CMakeLists.txt" ] || die "$tp does not look like a Csound source tree"

# --- 2) wire the Daisy port to the repo's libDaisy / DaisySP -----------------------------------
echo "==> linking Daisy/{libDaisy,DaisySP} -> lib/{libDaisy,DaisySP}"
ln -sfn ../../../lib/libDaisy "$tp/Daisy/libDaisy"
ln -sfn ../../../lib/DaisySP  "$tp/Daisy/DaisySP"

# --- 3) cross-build libcsound.a (single precision, bare metal) ---------------------------------
if [ "$do_build" = 0 ]; then
    echo "==> --no-build: source fetched + linked. Build later with: scripts/fetch_csound.sh"
    exit 0
fi

echo "==> configuring + building libcsound.a (arm cortex-m7, $JOBS jobs)"
mkdir -p "$tp/build"
( cd "$tp/build"
  cmake .. -DCMAKE_INSTALL_PREFIX=../Daisy \
           -DCUSTOM_CMAKE=../Daisy/Custom.cmake \
           -DCMAKE_TOOLCHAIN_FILE=../Daisy/crosscompile.cmake
  make -j"$JOBS"
  make install )

# --- 4) verify ---------------------------------------------------------------------------------
lib="$tp/Daisy/lib/libcsound.a"
hdr="$tp/Daisy/include/csound/csound.h"
[ -f "$lib" ] || die "build finished but $lib is missing"
[ -f "$hdr" ] || die "build finished but $hdr is missing"
echo "==> done: $lib ($(du -h "$lib" | cut -f1)), headers in $(dirname "$hdr")"
echo "    source pinned at CSOUND_REF=$CSOUND_REF; now build the engine: make engine-csound"
