# Thin Make frontend over the CMake build (WIP).
#
# Preserves the muscle-memory commands while CMake does the real work:
#   make -f Makefile.cmake                  # configure + build the default (granular) engine
#   make -f Makefile.cmake ENGINE=passthrough
#   make -f Makefile.cmake program-dfu      # build (if stale) + flash over DFU
#   make -f Makefile.cmake engine-edrums    # build + flash a variant in one shot
#   make -f Makefile.cmake clean
#   make -f Makefile.cmake DEBUG=1          # config toggles pass straight through
#
# Both build systems coexist: the root `Makefile` stays the canonical, hardware-proven build (output in
# `build/`); this CMake path is an opt-in alternative (output in `build-cmake/<engine>/`). Renaming this
# file to `Makefile` to make CMake the default is a *deferred* option, not done - it would only be worth
# it once the CMake build has been flashed and trusted as much as the Make one. The recursive calls below
# use $(THIS) so they keep working under either name if that rename ever happens.
#
# Each engine gets its own cached build dir (build-cmake/<engine>), so switching engines never forces
# a reconfigure-in-place or a clean -- this is what retires the old `.engine-stamp` clean/rebuild hack.

THIS   := $(lastword $(MAKEFILE_LIST))
ENGINE ?= granular
JOBS   ?= 8
BUILD  := build-cmake/$(ENGINE)

# Config toggles -> CMake -D flags (mirror the old Makefile's DEBUG / LOFI_INT16).
CMAKE_FLAGS :=
ifeq ($(DEBUG),1)
CMAKE_FLAGS += -DDEBUG=1
endif
ifeq ($(LOFI_INT16),1)
CMAKE_FLAGS += -DLOFI_INT16=1
endif

.PHONY: all build configure clean check-boundary program-dfu program-boot \
        engine-granular engine-passthrough engine-delay engine-qdelay engine-edrums engine-reso engine-mosc engine-graincloud engine-tape \
        engine-reverb engine-shuttle engine-softcut engine-radio engine-bard engine-glitch engine-pstretch engine-gigaverb engine-csound engine-chuck \
        engine-chorus engine-filter engine-voice \
        faust-gen gen-engines test-scripts test-scripts-deps

all: build

# Configure once per engine dir. `cmake --build` below auto-re-runs cmake if CMakeLists.txt changes,
# so this rule only fires for a fresh dir.
$(BUILD)/CMakeCache.txt:
	cmake -S . -B $(BUILD) -G "Unix Makefiles" -DENGINE=$(ENGINE) \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON $(CMAKE_FLAGS)

configure: $(BUILD)/CMakeCache.txt

build: check-boundary configure
	cmake --build $(BUILD) -j$(JOBS)

# Boundary guard carried over verbatim from the old Makefile (grep-enforced platform/engine split).
# The compiler-enforced version (per-target include roots) is a follow-on, not done in this spike.
PLATFORM_DIRS := src/hw src/ui src/memory src/transport
check-boundary:
	@if grep -rn '#include "engine/granular/\|#include "\.\./engine/granular/' $(PLATFORM_DIRS) ; then \
		echo "*** BOUNDARY VIOLATION: a platform TU (hw/ui/memory) includes granular DSP (above)."; \
		exit 1; \
	fi
	@echo "boundary OK: hw/ui/memory include no engine/granular/ headers"

program-dfu: configure
	cmake --build $(BUILD) --target program-dfu

program-boot: configure
	cmake --build $(BUILD) --target program-boot

clean:
	rm -rf build-cmake

# One-shot variant flash (parity with the old engine-* targets). No `clean` needed: each engine has its
# own cached dir, so there is no stale-engine contamination to wipe.
engine-granular:
	$(MAKE) -f $(THIS) ENGINE=granular build program-dfu
engine-passthrough:
	$(MAKE) -f $(THIS) ENGINE=passthrough build program-dfu
engine-delay:
	$(MAKE) -f $(THIS) ENGINE=delay build program-dfu
engine-qdelay:
	$(MAKE) -f $(THIS) ENGINE=qdelay build program-dfu
engine-edrums:
	$(MAKE) -f $(THIS) ENGINE=edrums build program-dfu
engine-reso:
	$(MAKE) -f $(THIS) ENGINE=reso build program-dfu
engine-graincloud:
	$(MAKE) -f $(THIS) ENGINE=graincloud build program-dfu
engine-tape:
	$(MAKE) -f $(THIS) ENGINE=tape build program-dfu
engine-reverb:
	$(MAKE) -f $(THIS) ENGINE=reverb build program-dfu
engine-shuttle:
	$(MAKE) -f $(THIS) ENGINE=shuttle build program-dfu
engine-softcut:
	$(MAKE) -f $(THIS) ENGINE=softcut build program-dfu
engine-radio:
	$(MAKE) -f $(THIS) ENGINE=radio build program-dfu
engine-bard:
	$(MAKE) -f $(THIS) ENGINE=bard build program-dfu
engine-glitch:
	$(MAKE) -f $(THIS) ENGINE=glitch build program-dfu
engine-pstretch:
	$(MAKE) -f $(THIS) ENGINE=pstretch build program-dfu
engine-gigaverb:
	$(MAKE) -f $(THIS) ENGINE=gigaverb build program-dfu
# QSPI-execute engines: same DFU flash path (the QSPI linker script places the app at 0x90040000, which
# program-dfu already targets). mosc's Plaits DSP is vendored in-tree; csound/chuck need their static lib
# fetched first (scripts/fetch_csound.sh / fetch_chuck.sh) or the link fails on a missing libcsound/chuck.a.
engine-mosc:
	$(MAKE) -f $(THIS) ENGINE=mosc build program-dfu
engine-csound:
	$(MAKE) -f $(THIS) ENGINE=csound build program-dfu
engine-chuck:
	$(MAKE) -f $(THIS) ENGINE=chuck build program-dfu
# Generated Faust engines (header-only kernels; no extra sources in CMakeLists).
engine-chorus:
	$(MAKE) -f $(THIS) ENGINE=chorus build program-dfu
engine-filter:
	$(MAKE) -f $(THIS) ENGINE=filter build program-dfu
engine-voice:
	$(MAKE) -f $(THIS) ENGINE=voice build program-dfu

# --- Host-side targets (codegen + Python tests) ----------------------------------------------------
# These do NOT touch the firmware build system: they run host Python/shell to (re)generate engine
# sources or run the script test suites, so they are identical under either frontend and are mirrored
# here verbatim from the canonical Makefile for muscle-memory parity (`make -f Makefile.cmake faust-gen`).

# Regenerate every engine's Faust kernel via cyfaust's cpp backend. FAUST_KERNELS lists one
# "<dir>:<namespace-prefix>:<name>" spec per kernel: it compiles <dir>/<name>.dsp -> <dir>/faust_kernel_<name>.h
# with the generated `class mydsp` wrapped in namespace spotykach::<prefix><name> so multiple kernels coexist
# in one build (cyfaust's cpp backend has no class-rename flag, so every kernel is `mydsp` - the namespace
# disambiguates them, and the generated `__mydsp_H__` include guard is renamed to match). The kernel's own
# `#include`s are hoisted to global scope (a namespaced #include would pull <cmath> etc. into the namespace);
# the generated class's unqualified `dsp`/`UI`/`Meta` resolve to the shared arch shim src/engine/faust_arch.h.
# cyfaust (the Cython libfaust wrapper, full cpp backend) lives in a repo-local .venv: `python3 -m venv .venv
# && .venv/bin/pip install cyfaust`. Override the interpreter with `CYFAUST_PY=/path/to/python` to pin a
# different libfaust version. Add a kernel: drop <name>.dsp in its engine dir, add a spec here, and bind it.
CYFAUST_PY ?= .venv/bin/python
FAUST_KERNELS ?= src/engine/reverb:rv_:dattorro src/engine/reverb:rv_:zita src/engine/tape:tfx_:tapefx
faust-gen:
	@for spec in $(FAUST_KERNELS); do \
	  dir=$${spec%%:*}; rest=$${spec#*:}; pfx=$${rest%%:*}; nm=$${rest##*:}; ns=$$pfx$$nm; \
	  src=$$dir/$$nm.dsp; out=$$dir/faust_kernel_$$nm.h; \
	  echo "compiling $$src -> $$out (namespace spotykach::$$ns)"; \
	  $(CYFAUST_PY) -m cyfaust compile $$src -b cpp -o $$dir/.kernel.gen || exit 1; \
	  { \
	    echo '// SYNTHUX ACADEMY /////////////////////////////////////////'; \
	    echo '// SPOTYKACH ///////////////////////////////////////////////'; \
	    echo '#pragma once'; echo ''; \
	    echo "// GENERATED FILE - do not edit by hand. Regenerate with \`make faust-gen\` (cyfaust cpp backend)."; \
	    echo "// Source: $$src. The generated \`class mydsp\` is wrapped in namespace spotykach::$$ns; its"; \
	    echo "// dsp/UI/Meta base types resolve to the shared arch shim (see engine/faust_arch.h)."; echo ''; \
	    grep '^#include' $$dir/.kernel.gen; \
	    echo '#include "engine/faust_arch.h"'; echo ''; \
	    echo "namespace spotykach { namespace $$ns {"; \
	    grep -v '^#include' $$dir/.kernel.gen | sed "s/__mydsp_H__/__$${ns}_H__/g"; \
	    echo "} } // namespace spotykach::$$ns"; \
	  } > $$out; \
	  rm -f $$dir/.kernel.gen; \
	done
	@echo "regenerated faust kernels"

# Regenerate every gen~ engine via gen-dsp's Daisy backend (the gen~ analogue of faust-gen).
# GEN_EXPORTS lists one "<gen~-export-dir>:<name>" spec per engine: scripts/gen_engine.py runs gen-dsp
# into src/engine/gen_<name>/, keeps only the genlib-isolation bridge (drops gen-dsp's board main and
# private allocator), emits <name>_engine.h (a ParamId map you retune by hand -- preserved across
# re-runs unless --force-glue), and wires the ENGINE switch + engine_select.h in marker-delimited
# blocks. gen-dsp lives in the repo-local .venv alongside cyfaust (`.venv/bin/pip install -e <gen-dsp>`).
# Add an engine: drop its spec here (or run the script directly), then `make ENGINE=<name>`.
GEN_PY ?= .venv/bin/python
# One "<gen~-export-dir>:<name>" spec per engine. gigaverb points at its own vendored export (the copy
# gen-dsp dropped under the engine dir is itself a valid gen-dsp input), so a regen is reproducible on
# any checkout without the external gen-dsp source tree. For a new engine, point at your gen~ export dir.
GEN_EXPORTS ?= src/engine/gigaverb/gen:gigaverb
gen-engines:
	@test -n "$(GEN_EXPORTS)" || { echo "set GEN_EXPORTS='<export-dir>:<name> ...' (or run scripts/gen_engine.py directly)"; exit 1; }
	@for spec in $(GEN_EXPORTS); do \
	  export=$${spec%:*}; name=$${spec##*:}; \
	  $(GEN_PY) scripts/gen_engine.py $$export $$name || exit 1; \
	done
	@echo "regenerated gen~ engines"

# Run the Python script test suites (scripts/test_*.py). These cover host-side utilities
# like convert_tape_audio.py and need neither hardware nor a firmware build. pytest is part
# of the `dev` dependency group declared in pyproject.toml ([dependency-groups], PEP 735),
# installed into the repo-local .venv. Override the interpreter with `TEST_PY=/path/to/python`.
# `make test-scripts` installs the dev group on first use (when pytest is missing);
# `make test-scripts-deps` (re)installs it on demand. Needs pip >= 25.1 for `--group`.
TEST_PY ?= .venv/bin/python
test-scripts-deps:
	$(TEST_PY) -m pip install -q --group dev
test-scripts:
	@$(TEST_PY) -c 'import pytest' 2>/dev/null || $(TEST_PY) -m pip install -q --group dev
	$(TEST_PY) -m pytest scripts/
