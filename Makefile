# Config Options
# DEBUG=1
# LOFI_INT16=1   # store the loop buffer as 16-bit PCM (doubles record time to 84s)

ifeq ($(DEBUG), 1)
C_DEFS += -DINFS_LOG=1
endif

ifeq ($(LOFI_INT16), 1)
C_DEFS += -DLOFI_INT16=1
endif

# On-target terminal test channel (docs/dev/terminal-*.md). `make ... TERMINAL=1` enables a
# bidirectional text/command channel over the USB-C CDC port for scripted engine testing + runtime
# control. Zero cost when off (every terminal body is under #if SPK_TERMINAL; app.cpp/core.ui.cpp only
# reference it under the flag). NOTE: SPK_TERMINAL adds virtuals to IEngine, so toggling it changes the
# engine vtable - build clean when switching TERMINAL (the engine-* one-shot targets already `make
# clean`; pass TERMINAL=1 to them, e.g. `make engine-delay TERMINAL=1`).
#
# Footprint: enabling the channel costs ~28 KB of SRAM_EXEC (mostly the USB-device CDC stack, which a
# normal build never links). Since SRAM_EXEC was rebalanced 186K -> 300K (see linker/alt_sram.lds) EVERY engine
# hosts it with margin - worst case granular at 69.9%. The old advice that granular/reso needed a
# QSPI-execute build no longer applies. QSPI-execute engines (mosc/csound/chuck) additionally need
# USB_MIDI=0, since MidiUsbHandler claims the same OTG_HS core.
ifeq ($(TERMINAL), 1)
C_DEFS += -DSPK_TERMINAL=1
endif

# OSC codec for the terminal channel (docs/dev/terminal-osc.md). `make ... TERMINAL=1 OSC=1` swaps
# layer [2] from line-ASCII to OSC-over-SLIP, so the device becomes a node in a Max/Pd/TouchOSC rig
# where a fader binds to an address once and then just sends floats. Layers [1] and [3] - transport,
# ring, TX FIFO, verb table, IEngine binding, `mode test` - are shared byte for byte with the line
# build; this replaces ONLY the codec.
#
# Line-ASCII stays the default and the floor: it is testable, works with a dumb terminal, and every OSC
# address has a line equivalent. Pick OSC when a control surface is the client, not when a script is.
#
# Cost over the line build: ~4 KB flash, and ~6.5 KB SRAM (a 512 B SLIP packet buffer, the TX FIFO
# 2 KB -> 8 KB because the `describe` bundle must fit whole, and an 8 KB static bundle scratch shared
# with nothing). The engines already at the SRAM_EXEC edge simply do not get OSC.
ifeq ($(OSC), 1)
ifneq ($(TERMINAL), 1)
$(error OSC=1 requires TERMINAL=1 - the OSC codec is layer [2] of the terminal channel. \
Build with `make ENGINE=<e> TERMINAL=1 OSC=1`.)
endif
C_DEFS += -DSPK_TERMINAL_OSC=1
# Logger coexistence is the sharpest constraint OSC adds, and it has no analogue in the line build:
# the transport shares one CDC device with the Logger, and a `[tag]` log line interleaving with replies
# is harmless for line-ASCII but FATAL for SLIP, where it lands inside a packet and corrupts the frame.
# Phase 1 takes the documented shortcut (a) - force INFS_LOG off - rather than (b), wrapping log output
# as /sk/log frames. Erroring out beats silently dropping DEBUG, which would look like a broken build.
ifeq ($(DEBUG), 1)
$(error OSC=1 and DEBUG=1 conflict: Logger output would land inside a SLIP frame and corrupt it. \
Drop DEBUG=1, or implement the /sk/log framing described in docs/dev/terminal-osc.md.)
endif
endif

# USB-C bring-up diagnostic (docs/dev/terminal-impl.md). `make ENGINE=<e> TERMINAL=1 USBDIAG=1` blinks
# the OTG_FS bring-up verdict on the Daisy onboard LED (six groups, 1 blink = bad / 2 = good; see
# AppImpl::usb_diag_tick). For use when the port does not enumerate and there is therefore no channel to
# report over; the onboard LED is readable on a Pod / open unit, not on a cased Spotykach.
# The build is otherwise a NORMAL, fully runnable app: the blink is non-blocking, driven from Loop(),
# and touches only the onboard LED (which the app never otherwise uses). In particular the boot-button
# DFU escape hatch stays live - do NOT make this park in Init(), which would take that with it.
# Note: a command-line `C_DEFS+=...` does not work here (it clobbers the in-Makefile C_DEFS, including
# -DSTM32H750xx), which is why this is a switch rather than a define you pass through.
ifeq ($(USBDIAG), 1)
C_DEFS += -DTERM_USBDIAG=1
endif

# Which USB port the terminal lives on. DEFAULT = external: OTG_HS-as-FS on PB14/PB15 (Seed pins
# D29/D30), which is where the Spotykach's panel USB-C is wired - verified on hardware 2026-07-31.
# `TERMPORT=int` selects OTG_FS (PA11/PA12), the Daisy Seed's own USB connector, for a bare Seed or Pod.
# The symptom of having this wrong is distinctive: the USBDIAG readout shows a completely healthy core
# (clocks, supply, transceiver, pullup asserted, pads in the right alternate function) while the host
# never sends a frame - i.e. the app is driving a connector that is not connected to anything.
ifeq ($(TERMPORT), ext)
C_DEFS += -DSPK_TERMINAL_PORT_EXTERNAL=1
endif
ifeq ($(TERMPORT), int)
C_DEFS += -DSPK_TERMINAL_PORT_EXTERNAL=0
endif

# Swappable DSP engine selected at build time (item 3b). Default = the granular looper.
# `make ENGINE=passthrough` builds the minimal passthrough variant. The define drives
# src/engine/engine_select.h (-> ActiveEngine); ENGINE_SOURCES compiles only the chosen engine.

# Shared stmlib (Mutable Instruments support library), trimmed to the union of the reso (Rings) and
# mosc (Plaits) closures and vendored ONCE here instead of per engine. Both engines add $(STMLIB_INC)
# to their include scope and pull the three .cc from $(STMLIB_TP). The files are byte-identical to the
# old per-engine copies (same upstream, unchanged for years), so this is a dedup, not a version bump.
STMLIB_TP  = src/engine/common/thirdparty/stmlib
STMLIB_INC = -Isrc/engine/common/thirdparty

ENGINE ?= granular
ifeq ($(ENGINE), granular)
C_DEFS += -DSPK_ENGINE_GRANULAR
# Granular engine = its IEngine wrapper (granular_engine.cpp) + all the DSP, all under src/engine/granular/.
ENGINE_SOURCES = $(wildcard src/engine/granular/*.cpp)
else ifeq ($(ENGINE), passthrough)
C_DEFS += -DSPK_ENGINE_PASSTHROUGH
# Passthrough engine is header-only (src/engine/passthrough/); no engine .cpp to compile.
ENGINE_SOURCES =
else ifeq ($(ENGINE), delay)
C_DEFS += -DSPK_ENGINE_DELAY
ENGINE_SOURCES = src/engine/delay/delay_engine.cpp
else ifeq ($(ENGINE), qdelay)
C_DEFS += -DSPK_ENGINE_QDELAY
# QDelay flavor: the delay grammar with a Clean/Diffuse/Duck character palette. Its feedback diffuser
# is the header-only src/dsp/diffuser.h (no extra source); the engine .cpp is the only unit to compile.
ENGINE_SOURCES = src/engine/qdelay/qdelay_engine.cpp
else ifeq ($(ENGINE), edrums)
C_DEFS += -DSPK_ENGINE_EDRUMS
ENGINE_SOURCES = src/engine/edrums/edrums_engine.cpp
else ifeq ($(ENGINE), tape)
C_DEFS += -DSPK_ENGINE_TAPE
# Streaming tape engine = its IEngine wrapper (tape_engine.cpp). Its SD streaming service
# (src/hw/stream_deck.cpp + fat_file.cpp) compiles via the platform src/hw/ wildcard, with bodies
# guarded by SPK_USE_STREAM (the platform stream capability) so every non-streaming engine stays
# byte-identical. SPK_USE_STREAM is the feature flag any engine needing SD streaming opts into.
C_DEFS += -DSPK_USE_STREAM
ENGINE_SOURCES = src/engine/tape/tape_engine.cpp
else ifeq ($(ENGINE), radio)
C_DEFS += -DSPK_ENGINE_RADIO
# Dual virtual RadioMusic. Streams headerless raw 16-bit-mono ".raw" stations from SD via the shared
# streaming service (stream_deck.cpp + fat_file.cpp, guarded by SPK_USE_STREAM like tape/shuttle), so
# every non-streaming engine stays byte-identical.
C_DEFS += -DSPK_USE_STREAM
ENGINE_SOURCES = src/engine/radio/radio_engine.cpp
else ifeq ($(ENGINE), bard)
C_DEFS += -DSPK_ENGINE_BARD
# The storyteller: bookmark-navigated audiobook decks. Streams 16-bit-mono books from SD via the shared
# streaming service (stream_deck.cpp + fat_file.cpp, guarded by SPK_USE_STREAM like tape/radio/pstretch),
# and is the one engine that WRITES a small text file (its resume table) through the same service.
# Bookmark parsing / auto-marks / the resume table are header-only under src/engine/bard/.
C_DEFS += -DSPK_USE_STREAM
ENGINE_SOURCES = src/engine/bard/bard_engine.cpp
# WSOLA (the PITCH-KEEP time-scaler) + the room push SRAM_EXEC to ~94% at -O2, leaving too little headroom
# to work in. Build bard at -Os as reso/reverb already do: ~88% with room to spare, and the M7 @ 480 MHz has
# ample compute margin for this engine (its DSP is a resampler, a correlation search, biquads and combs).
OPT = -Os
else ifeq ($(ENGINE), glitch)
C_DEFS += -DSPK_ENGINE_GLITCH
# Dual-deck lo-fi/circuit-bent noise voice: 12 curated algorithms ported from Rob Scape's Noisferatu
# (de-Arduino'd, per-instance, retuned for 48 kHz). Self-contained - no SD/arena, two ~8 KB glitch
# buffers live in the engine object.
ENGINE_SOURCES = src/engine/glitch/glitch_engine.cpp
else ifeq ($(ENGINE), pstretch)
C_DEFS += -DSPK_ENGINE_PSTRETCH
# pstretch needs ~291K of SRAM DATA for its FFT working set - nearly twice the next worst engine - so
# it uses the data-favoured split (200K code / 312K data) rather than dragging the default down for
# every other engine. Measured 2026-07-31; the LDSCRIPT selection lives with the other link settings
# below - see linker/alt_sram_pstretch.lds for the measured band.
# Real-time clean-room PaulStretch ambient time-smear. Self-contained DSP: a vendored radix-2 FFT
# (engine/pstretch/fft.h), no CMSIS-DSP. Per-voice input rings + FFT scratch live in the SDRAM arena.
# The Phase-2 SD-file source streams clips from the card via the shared streaming service (stream_deck.cpp
# + fat_file.cpp, guarded by SPK_USE_STREAM like tape/radio), so non-streaming engines stay byte-identical.
C_DEFS += -DSPK_USE_STREAM
ENGINE_SOURCES = src/engine/pstretch/pstretch_engine.cpp
else ifeq ($(ENGINE), reverb)
C_DEFS += -DSPK_ENGINE_REVERB
# Stereo reverb (Dattorro plate / Zita hall) whose DSP is Faust-generated. The cyfaust-generated kernels
# (faust_kernel_<name>.h) are produced from the .dsp sources by `make faust-kernels`; the arch shim
# (faust_arch.h) is hand-written + MIT. The kernels' delay-line state is placement-new'd into the SDRAM
# arena, so SRAM stays flat; the link's -Wl,--print-memory-usage shows SRAM_EXEC (the binding region).
ENGINE_SOURCES = src/engine/reverb/reverb_engine.cpp
# Three all-Faust voices selected by the Reel/Slice/Drift mode switch: Dattorro plate (blue), Zita hall
# (violet), and Greyhole (teal). No gen~ runtime - every voice is a cyfaust kernel registered in
# FAUST_KERNELS below.
# Greyhole's modulated diffusion network + pitch-shifter overflow SRAM_EXEC at -O2 (~106%), so build the
# reverb at -Os (as reso does); it fits at ~97% with headroom. The M7 @ 480 MHz has ample compute margin.
OPT = -Os
# The reverb is route-aware: ONE stereo voice in Stereo/GenerativeStereo, two independent MONO reverbs
# (one per deck) in DoubleMono - chosen at runtime by the Route switch, no build flag. Pair with METER=1
# to read the two-voice (DoubleMono) CPU load on device.
else ifeq ($(ENGINE), shuttle)
C_DEFS += -DSPK_ENGINE_SHUTTLE
# Buffer-based bipolar/reverse "varispeed shuttle" tape: 4 in-SDRAM mono tape buffers (2 decks x 2
# tracks). RECORD needs only the arena; LOAD (load a tape slot from SD into RAM) opts into the shared
# platform streaming service via SPK_USE_STREAM (same flag the tape engine sets), so the platform
# constructs/pumps/injects the StreamDeck and ctx.stream is live on target.
C_DEFS += -DSPK_USE_STREAM
ENGINE_SOURCES = src/engine/shuttle/shuttle_engine.cpp
else ifeq ($(ENGINE), softcut)
C_DEFS += -DSPK_ENGINE_SOFTCUT
# Optional extra defines for diagnostics (e.g. SOFTCUT_EXTRA=-DSOFTCUT_DIAG_PASSTHROUGH). Empty by default.
C_DEFS += $(SOFTCUT_EXTRA)
# softcut's ReadWriteHead uses M_PI/M_PI_2, which strict -std=c++17 does not expose from <cmath> on
# arm-none-eabi (the same idiom reso/mosc use for M_PI). Define them for this build.
C_DEFS += -DM_PI=3.14159265358979323846 -DM_PI_2=1.57079632679489661923
# Dual-deck overdub looper loads loops from SD (Alt+PITCH slot select + boot preload), so it opts into
# the platform streaming service (stream_deck.cpp + fat_file.cpp), the same flag tape/shuttle/radio set.
C_DEFS += -DSPK_USE_STREAM
# Vendored monome softcut-lib core (5 .cpp: Voice/ReadWriteHead/SubHead/Svf/FadeCurves), under
# src/engine/softcut/vendor. SOFTCUT_INC scopes its public headers to this build. The DSP is tiny
# (~10 KB SRAM_EXEC) so this is a normal -O2 SRAM build, not QSPI. Feasibility scaffold: pair with
# METER=1 and flip the Mode switch (Slice/Reel/Drift -> 2/4/6 voices) to read on-device load.
SOFTCUT_TP  = src/engine/softcut/vendor
SOFTCUT_INC = -I$(SOFTCUT_TP)/include
ENGINE_SOURCES = src/engine/softcut/softcut_engine.cpp \
	$(wildcard $(SOFTCUT_TP)/src/*.cpp)
else ifeq ($(ENGINE), reso)
C_DEFS += -DSPK_ENGINE_RESO
# stmlib's filters use M_PI, which strict -std=c++17 does not expose from <cmath> on arm-none-eabi.
C_DEFS += -DM_PI=3.14159265358979323846
# The Rings DSP (~30K of code+tables) overflows the 186K execution SRAM at -O2. Build reso at -Os to
# fit; the M7 at 480 MHz has ample headroom (Rings shipped on a 168 MHz F4). Scoped to this engine.
OPT = -Os
# reso's DSP is the Mutable Instruments Rings engine, vendored under src/engine/reso/thirdparty/; its
# stmlib support library now comes from the shared $(STMLIB_TP) (see top). RESO_INC scopes both includes
# to the reso build (empty for other engines). The .cc files compile on-target WITHOUT -DTEST, so stmlib
# uses its Cortex-M ssat/usat fast paths.
RESO_TP  = src/engine/reso/thirdparty
RESO_INC = -I$(RESO_TP) $(STMLIB_INC)
ENGINE_SOURCES = src/engine/reso/reso_engine.cpp \
	$(RESO_TP)/rings/dsp/part.cc $(RESO_TP)/rings/dsp/string.cc \
	$(RESO_TP)/rings/dsp/resonator.cc $(RESO_TP)/rings/dsp/fm_voice.cc \
	$(RESO_TP)/rings/resources.cc \
	$(STMLIB_TP)/dsp/units.cc $(STMLIB_TP)/utils/random.cc $(STMLIB_TP)/dsp/atan.cc
else ifeq ($(ENGINE), mosc)
C_DEFS += -DSPK_ENGINE_MOSC
# stmlib's filters use M_PI, which strict -std=c++17 does not expose from <cmath> on arm-none-eabi.
C_DEFS += -DM_PI=3.14159265358979323846
# Plaits' user_data.h normally pulls the original STM32F37x flash header for on-device patch storage;
# this firmware has none, so stub it (UserData::ptr() -> NULL, Voice uses the built-in fm_patches_table).
C_DEFS += -DPLAITS_USER_DATA_STUB
# The Plaits DSP (full 24-engine voice + ~370 KB of LUTs) overflows the 186K execution SRAM at -O2;
# build at -Os to fit (as reso does). The M7 @ 480 MHz has ample headroom (Plaits shipped on an F37x).
OPT = -Os
# The full 24-engine voice is ~292 KB of .text - it overflows the 186 KB SRAM_EXEC, so mosc is a
# QSPI-EXECUTE target (like csound): build BOOT_QSPI with the QSPI linker script:
#   make ENGINE=mosc APP_TYPE=BOOT_QSPI LDSCRIPT=linker/alt_qspi.lds   (or just: make engine-mosc)
# UNLIKE csound/chuck, mosc still synthesises from the platform engine arena (it placement-news its two
# plaits::Voice + scratch into ctx.arena), so it does NOT set SPK_NO_ENGINE_ARENA - the 48 MB arena stays.
# It reuses the csound engine's VTOR inject (engine-agnostic BOOT_QSPI vector-table fix).
# mosc's DSP is the Mutable Instruments Plaits voice, vendored under src/engine/mosc/thirdparty/; its
# stmlib support library now comes from the shared $(STMLIB_TP) (see top). MOSC_INC scopes both includes
# to the mosc build (the plaits root holds `plaits/`, the shared root holds `stmlib/`). The .cc files
# compile on-target WITHOUT -DTEST, so stmlib uses its Cortex-M ssat/usat fast paths.
MOSC_TP  = src/engine/mosc/thirdparty
MOSC_INC = -I$(MOSC_TP) $(STMLIB_INC)
ENGINE_SOURCES = src/engine/mosc/mosc_engine.cpp src/engine/csound/spotykach_qspi_vtor.cpp \
	$(MOSC_TP)/plaits/dsp/voice.cc \
	$(wildcard $(MOSC_TP)/plaits/dsp/engine/*.cc) \
	$(wildcard $(MOSC_TP)/plaits/dsp/engine2/*.cc) \
	$(wildcard $(MOSC_TP)/plaits/dsp/physical_modelling/*.cc) \
	$(wildcard $(MOSC_TP)/plaits/dsp/speech/*.cc) \
	$(MOSC_TP)/plaits/dsp/chords/chord_bank.cc \
	$(MOSC_TP)/plaits/dsp/fm/algorithms.cc $(MOSC_TP)/plaits/dsp/fm/dx_units.cc \
	$(MOSC_TP)/plaits/resources.cc \
	$(STMLIB_TP)/dsp/units.cc $(STMLIB_TP)/utils/random.cc $(STMLIB_TP)/dsp/atan.cc
else ifeq ($(ENGINE), graincloud)
# graincloud is THE GRANULAR ENGINE TREE compiled with -DSPK_GRAIN_GF, not a copy of it. Under that
# flag Generator's per-sample Vox array is replaced by a per-block GrainflowLib cloud (gf_cloud.*) and
# Deck gates it from the Play pad; every other file - Core, Buffer, Track, Vox, the FX, the modulators
# - is the same source, compiled twice into two different images. Its vendored GrainflowLib lives in
# src/engine/graincloud/thirdparty/grainflow/.
#
# It used to be a byte-for-byte fork: 35 of 42 files identical, ~3,400 duplicated lines that a fix to
# granular/ silently did not reach (and graincloud is the PUBLISHED one of the pair). The three files
# that genuinely differ now carry `#if SPK_GRAIN_GF` blocks in the granular tree - generator.h,
# generator.cpp and two lines of deck.cpp - so granular/ stays exactly where upstream put it and stays
# diffable against it, which a shared "graincore" directory would have cost.
#
# Include order matters: src/engine/graincloud comes FIRST so granular/generator.cpp's guarded
# `#include "gf_cloud.h"` resolves (it is not in granular/, so the quoted lookup falls through to -I).
C_DEFS += -DSPK_ENGINE_GRAINCLOUD -DSPK_GRAIN_GF=1
# gfSyn pulls M_PI via <cmath>, which strict -std=c++17 does not expose on arm-none-eabi.
C_DEFS += -DM_PI=3.14159265358979323846
# Granular + the GrainflowLib templates overflow the execution SRAM at -O2; build at -Os to fit
# (as reso/reverb do). The M7 @ 480 MHz has ample compute headroom.
OPT = -Os
GRAINCLOUD_TP  = src/engine/graincloud/thirdparty
GRAINCLOUD_INC = -Isrc/engine/graincloud -I$(GRAINCLOUD_TP) -Isrc/engine/granular
# The granular tree MINUS its IEngine wrapper (graincloud_engine.cpp replaces granular_engine.cpp),
# plus graincloud's own two files. generator.cpp is shared - the SPK_GRAIN_GF branches are inside it.
ENGINE_SOURCES = $(filter-out src/engine/granular/granular_engine.cpp, $(wildcard src/engine/granular/*.cpp)) \
                 $(wildcard src/engine/graincloud/*.cpp)
# gen~ engines (ENGINE=gen_<name>) are appended below by scripts/gen_engine.py, one marker-delimited
# `else ifeq` block per export. They use the genlib-isolation bridge from gen-dsp + the shared
# src/engine/gen/ family (GenEngine<W> + the arena-bound genlib runtime). See `make gen-engines`.
# >>> gen:gigaverb >>> (managed by scripts/gen_engine.py)
else ifeq ($(ENGINE), gigaverb)
C_DEFS += -DSPK_ENGINE_GIGAVERB
C_DEFS += -DGENLIB_NO_JSON
C_DEFS += -DDAISY_EXT_NAME=gigaverb
C_DEFS += -DGEN_EXPORTED_NAME=gen_exported
C_DEFS += -DGEN_EXPORTED_HEADER=\"gen_exported.h\"
C_DEFS += -DGEN_EXPORTED_CPP=\"gen_exported.cpp\"
C_DEFS += -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter
GEN_DIR = src/engine/gigaverb
GEN_INC = -I$(GEN_DIR) -I$(GEN_DIR)/gen -I$(GEN_DIR)/gen/gen_dsp
ENGINE_SOURCES = $(GEN_DIR)/_ext_daisy.cpp src/engine/gen/genlib_arena.cpp
# <<< gen:gigaverb <<<
# >>> faust:chorus >>> (managed by scripts/gen_faust_engine.py)
else ifeq ($(ENGINE), chorus)
C_DEFS += -DSPK_ENGINE_CHORUS
# Faust engine generated from chorus.dsp + chorus.json - header-only (the cyfaust kernel + the
# shared FaustEngine<Traits> wrapper), so there is no engine .cpp.
ENGINE_SOURCES =
# <<< faust:chorus <<<
# >>> faust:filter >>> (managed by scripts/gen_faust_engine.py)
else ifeq ($(ENGINE), filter)
C_DEFS += -DSPK_ENGINE_FILTER
# Faust engine generated from filter.dsp + filter.json - header-only (the cyfaust kernel + the
# shared FaustEngine<Traits> wrapper), so there is no engine .cpp.
ENGINE_SOURCES =
# <<< faust:filter <<<
# >>> faust:voice >>> (managed by scripts/gen_faust_engine.py)
else ifeq ($(ENGINE), voice)
C_DEFS += -DSPK_ENGINE_VOICE
# Faust engine generated from voice.dsp + voice.json - header-only (the cyfaust kernel + the
# shared FaustEngine<Traits> wrapper), so there is no engine .cpp.
ENGINE_SOURCES =
# <<< faust:voice <<<
else ifeq ($(ENGINE), csound)
# Csound is a QSPI-ONLY target: it links libcsound.a (~2 MB code) which can't fit the 186 KB
# SRAM_EXEC budget, so it must run from QSPI. Build it BOOT_QSPI with the QSPI linker script:
#   make ENGINE=csound APP_TYPE=BOOT_QSPI LDSCRIPT=linker/alt_qspi.lds   (or just: make engine-csound)
# Prereq: libcsound.a (scripts/fetch_csound.sh). engine_select.h maps SPK_ENGINE_CSOUND ->
# CsoundEngine; see docs/dev/csound-impl.md.
C_DEFS += -DSPK_ENGINE_CSOUND
# Enable the platform SD streaming service so ctx.stream is injected (app.cpp): the engine reads
# /csound/<n>.csd patches off the card via ctx.stream->exists/read_text. Without this, ctx.stream is
# null and only the built-in orchestra is ever available.
C_DEFS += -DSPK_USE_STREAM
# Csound runs its synthesis from its own 12 MB SDRAM pool (csound_alloc.cpp) and never touches the
# platform engine arena, so opt out of it: shrinks the 48 MB arena to a token block (buffer.sdram.cpp),
# dropping SDRAM from ~62 MB to ~14 MB. A capability flag, not an engine-name check (shared with chuck).
C_DEFS += -DSPK_NO_ENGINE_ARENA
CSOUND_BASE = thirdparty/csound/Daisy
CSOUND_INC  = -I$(CSOUND_BASE)/include/csound
ENGINE_SOURCES = src/engine/csound/csound_engine.cpp src/engine/csound/csound_alloc.cpp src/engine/csound/spotykach_qspi_vtor.cpp
LIBS    += $(CSOUND_BASE)/lib/libcsound.a
LDFLAGS += -u _printf_float
# Route Csound's C-malloc family to the SDRAM bump pool (csound_alloc.cpp); the platform heap stays in SRAM.
# aligned_alloc is wrapped too: Csound (memalloc.c, beta17+) uses it, and nano-libc's aligned_alloc pulls
# in posix_memalign, which nosys does not provide (undefined-reference link error) - our wrap replaces it.
LDFLAGS += -Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc,--wrap=aligned_alloc
else ifeq ($(ENGINE), chuck)
# ChucK is a QSPI-ONLY target (like Csound): it links libchuck.a (~1.1 MB code) which can't fit the
# 186 KB SRAM_EXEC budget, so it must run from QSPI. Build it BOOT_QSPI with the QSPI linker script:
#   make ENGINE=chuck APP_TYPE=BOOT_QSPI LDSCRIPT=linker/alt_qspi_chuck.lds   (or just: make engine-chuck)
# Prereq: libchuck.a + the shim sysroot (scripts/fetch_chuck.sh). engine_select.h maps SPK_ENGINE_CHUCK
# -> ChuckEngine; see docs/dev/chuck-impl.md.
C_DEFS += -DSPK_ENGINE_CHUCK
# Enable the platform SD streaming service so ctx.stream is injected (app.cpp): the engine reads
# /chuck/<n>.ck patches off the card via ctx.stream->exists/read_text. Without this, ctx.stream is null
# and only the built-in program is ever available (the Alt+PITCH selector shows one entry, nothing to
# load).
C_DEFS += -DSPK_USE_STREAM
# ChucK synthesizes from its own 12 MB SDRAM pool (chuck_alloc.cpp) and never touches the platform engine
# arena, so opt out of it: shrinks the 48 MB arena to a token block (buffer.sdram.cpp), leaving SDRAM at
# ~22% (12 MB pool + 2 MB stream rings) instead of ~97%. A capability flag, not an engine-name check.
C_DEFS += -DSPK_NO_ENGINE_ARENA
# ChucK feature defines = the exact set fetch_chuck.sh built libchuck.a with. They MUST match so the
# ChucK class layouts the engine TU sees agree with the archive (__DISABLE_THREADS__ etc. drop members).
C_DEFS += -D__PLATFORM_LINUX__ -D__USE_CHUCK_YACC__ \
  -DCPU_IS_LITTLE_ENDIAN=1 -DCPU_IS_BIG_ENDIAN=0 -DTYPEOF_SF_COUNT_T=__INT64_TYPE__ -DSIZEOF_SF_COUNT_T=8 \
  -D__DISABLE_WATCHDOG__ -D__DISABLE_NETWORK__ -D__DISABLE_OTF_SERVER__ \
  -D__ALTER_HID__ -D__DISABLE_HID__ -D__DISABLE_SERIAL__ -D__DISABLE_ASYNCH_IO__ -D__DISABLE_THREADS__ \
  -D__DISABLE_KBHIT__ -D__DISABLE_PROMPTER__ -D__DISABLE_SHELL__ -D__OLDSCHOOL_RANDOM__
# ChucK's headers use C++ exceptions + RTTI, and libchuck.a was built with both ON, so the engine TU
# must agree (ABI). libDaisy's CPPFLAGS set -fno-exceptions/-fno-rtti; re-enable them for JUST the
# ChucK engine TU (a target-specific override below, after CPP_USER_FLAGS is appended last in CPPFLAGS).
# Scoped to the one TU so the rest of the firmware keeps -fno-exceptions and doesn't drag the libstdc++
# exception machinery (a multi-KB static .bss footprint) into the SRAM-tight platform.
build/chuck_engine.o: CPP_USER_FLAGS += -fexceptions -frtti
CHUCK_BASE = thirdparty/chuck
# chuck.h/chuck_globals.h from src/core; the shim provides the POSIX headers chuck.h transitively pulls.
CHUCK_INC  = -I$(CHUCK_BASE)/src/core -I$(CHUCK_BASE)/Daisy/shim
# Reuse the Csound engine's VTOR inject (the BOOT_QSPI vector-table fix is engine-agnostic).
ENGINE_SOURCES = src/engine/chuck/chuck_engine.cpp src/engine/chuck/chuck_alloc.cpp src/engine/csound/spotykach_qspi_vtor.cpp
LIBS    += $(CHUCK_BASE)/Daisy/lib/libchuck.a
# nano.specs omits floating-point printf; ChucK's parser stringifies float literals via std::to_string
# -> vsnprintf("%f"), which returns a negative length without it and aborts in std::__throw_length_error
# (nano libstdc++ stubs every __throw_* to a bare abort, so it is uncatchable). See docs/dev/chuck-pod-poc.md.
LDFLAGS += -u _printf_float
# Route ChucK's C-malloc family to the SDRAM pool (chuck_alloc.cpp); the platform heap stays in SRAM.
LDFLAGS += -Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc
else
$(error Unknown ENGINE '$(ENGINE)' - use 'granular', 'passthrough', 'delay', 'qdelay', 'edrums', 'reso', 'mosc', 'graincloud', 'tape', 'radio', 'bard', 'glitch', 'pstretch', 'reverb', 'shuttle', 'softcut', 'gigaverb', 'chorus', 'filter', 'voice', 'csound', or 'chuck')
endif
# The engine list in that error is HAND-MAINTAINED and had drifted (it was missing glitch, pstretch,
# softcut and gigaverb). Add a new engine's name to it, and to the matching message in CMakeLists.txt.
# Do not put anything between the `else` and the `$(error ...)` line: scripts/gen_engine.py and
# gen_faust_engine.py locate this switch by the literal string "else\n$(error Unknown ENGINE" and insert
# generated engine blocks immediately before it, so a line in that gap breaks both generators.

# Opt-in (make ... METER=1): enable the on-device CPU load meter (app.cpp's CpuLoadMeter). It writes
# Max/Avg/Min processing load % to the external USB CDC (LOGGER_EXTERNAL port) every ~250 ms using a
# direct NON-BLOCKING transmit (drops if the host isn't draining) - so the meter can never hang the main
# loop the way the daisy Logger does. No INFS_LOG/Logger dependency, so it adds almost no code. Read it
# over USB serial (keep the port open). Compiled under METER, works at the shipping -O2.
# NOTE: METER=1 and TERMINAL=1 are mutually exclusive - the meter's CDC device claims the same OTG core
# the terminal channel needs. With TERMINAL=1 you do not want this flag anyway: a terminal build drives
# the same CpuLoadMeter and reports it on request via `query cpu` / `cpumin` / `cpumax` (+ `reset cpu`),
# with no second USB device. See docs/dev/terminal-dispatch.md "CPU load".
ifeq ($(METER), 1)
C_DEFS += -DMETER
endif

# pstretch FFT/analysis window override. The default (8192 - a lusher wash) lives in pstretch_engine.h; opt in
# to the lighter, meter-verified window with `make ENGINE=pstretch WINDOW=4096` (avg ~32% / max ~64% CPU on
# hardware). 8192 ~doubles the FFT working set (ola/fifo move to the SDRAM arena there); it links + fits and
# runs clean on hardware (flashed 2026-07-01) - re-measure CPU with TERMINAL=1 + `query cpu` (or METER=1) for an exact number.
ifdef WINDOW
C_DEFS += -DPSTRETCH_WINDOW=$(WINDOW)
endif

# ChucK bring-up debugging (opt-in, ENGINE=chuck only). See docs/dev/chuck-impl.md "M1/M2 hardware".
#   make engine-chuck BRINGUP=1          - blink the Daisy onboard LED at boot checkpoints (1..4) so a
#                                          non-booting QSPI app (solid-white panel) can be localised.
#   make engine-chuck BRINGUP=1 NOCHUCK=1 - also skip ChucK's create/compile, to prove the platform +
#                                          linker script boot without the ChucK runtime (isolation).
ifeq ($(BRINGUP), 1)
C_DEFS += -DCHUCK_BRINGUP
endif
ifeq ($(NOCHUCK), 1)
C_DEFS += -DCHUCK_SKIP_RUNTIME
endif
# Bisect ChucK init: CHUCKLVL=1 (new ChucK only) / 2 (+init) / 3 (+compile, = full). The first level
# whose flash boots the panel (vs solid-white) localises which call fails.
ifdef CHUCKLVL
C_DEFS += -DCHUCK_RUNTIME_LEVEL=$(CHUCKLVL)
endif

USE_FATFS = 1

# Firmware identity baked into every binary (see src/version.h / version.cpp). SPK_VERSION is
# `git describe` of the source tree: a bare tag like "v1.2.0" on a clean release checkout, or
# "v1.2.0-5-gabc1234" mid-development; "dev" if git is unavailable. We deliberately omit --dirty
# so the value only changes per commit (one relink), not on every uncommitted edit. ENGINE is the
# variant name. Both reach the compiler as string literals; build/.version-stamp (below) forces
# the two consuming objects to recompile when the value changes, since make can't see -D changes.
SPK_VERSION ?= $(shell git -C $(CURDIR) describe --tags --always 2>/dev/null || echo dev)
C_DEFS += -DSPK_VERSION_STR='"$(SPK_VERSION)"'
C_DEFS += -DSPK_ENGINE_STR='"$(ENGINE)"'

# Project Name
TARGET = spotykach

CPP_STANDARD = -std=c++17

LIBDAISY_DIR = lib/libDaisy
DAISYSP_DIR = lib/DaisySP
CMSIS_DSP_SRC_DIR = ${LIBDAISY_DIR}/Drivers/CMSIS-DSP/Source

# Daisy Bootloader - SRAM Linkage
APP_TYPE = BOOT_SRAM
# Per-engine linker script. Everything uses linker/alt_sram.lds (300K/212K code/data) EXCEPT pstretch, which is
# the one engine large in both halves at once and does not link at that split - see the header of
# linker/alt_sram_pstretch.lds for the measured band. Selected on ENGINE rather than left to the engine-pstretch
# target, so a plain `make ENGINE=pstretch` is correct too; an explicit LDSCRIPT= on the command line
# still wins, which is what the QSPI targets (mosc/csound/chuck) rely on.
LDSCRIPT = $(if $(filter pstretch,$(ENGINE)),linker/alt_sram_pstretch.lds,linker/alt_sram.lds)
BOOT_BIN = bootloader-spotykach-v2.bin

# USB MIDI (device MIDI on the rear USB-C) pulls libDaisy's USB-device + MIDI-class code (~3 KB) into
# .text. It fits the QSPI-execute builds (csound/chuck/mosc) with room to spare, but overflows the
# 186 KB SRAM_EXEC budget of the BOOT_SRAM builds (granular already links at ~94%). So enable it
# automatically for BOOT_QSPI, and allow an explicit override (USB_MIDI=1 / USB_MIDI=0) to measure
# headroom on a specific SRAM engine. Gates the midi_usb code in hw/hardware.* and ui/core.ui.midi.cpp.
USB_MIDI ?= $(if $(filter BOOT_QSPI,$(APP_TYPE)),1,0)
ifeq ($(USB_MIDI),1)
C_DEFS += -DSPK_USB_MIDI
endif

C_INCLUDES = -Isrc/ -Ilib/ $(RESO_INC) $(MOSC_INC) $(GRAINCLOUD_INC) $(SOFTCUT_INC) $(GEN_INC) $(CSOUND_INC) $(CHUCK_INC)
# NOTE: there used to be `C_USR_FLAGS = -ffast-math -funroll-loops` here, but the core Makefile reads
# C_USER_FLAGS (with the E), so it was dead - those flags never reached the compiler and the shipping
# firmware was built without them. Removed to stop it reading as active. The device meets its CPU/SRAM
# budget without them, so do NOT just rename the var: -ffast-math changes FP semantics (implies
# -ffinite-math-only, dropping isnan/isinf guards) and -funroll-loops inflates .text (SRAM_EXEC is
# ~94% full). Enabling fast-math/FTZ is a deliberate, measured, hardware-flashed change - batch with P2.
C_DEFS += -DINFS_LOG_TARGET=daisy::LOGGER_EXTERNAL

CPP_SOURCES = \
	main.cpp \
	src/app.cpp \
	src/version.cpp \
	$(ENGINE_SOURCES) \
	src/engine/color.cpp \
	src/engine/led.ring.cpp \
	$(wildcard src/transport/*.cpp) \
	$(wildcard src/terminal/*.cpp) \
	$(wildcard src/dsp/*.cpp) \
	$(wildcard src/hw/*.cpp) \
	$(wildcard src/ui/*.cpp) \
	$(wildcard src/memory/*.cpp)

# Core location, and generic Makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# Rebuild the engine-dependent object when ENGINE changes. Objects are compiled with -DSPK_ENGINE_*,
# but make can't see flag changes, so `make ENGINE=passthrough` over a stale granular build would
# relink the wrong engine (undefined-reference to the other engine's vtable). app.cpp is the only TU
# that includes engine_select.h, so make it depend on a stamp whose content is rewritten only when
# ENGINE differs -> app.o rebuilds exactly on a switch, no manual `make clean` needed.
# version.o bakes the ENGINE name into the build banner (-DSPK_ENGINE_STR, see src/version.cpp), which
# is equally invisible to make - without the stamp, `make ENGINE=x` over a stale build of engine y links
# a binary that runs x but reports "engine=y" to `strings`/the boot log. Same stamp as app.o.
build/app.o build/version.o: build/.engine-stamp
# The SD-streaming platform TUs are also engine-flag-dependent: their bodies are guarded by
# SPK_USE_STREAM (set by the streaming engines tape/shuttle, so non-streaming engines stay
# byte-identical), so they must rebuild on an engine switch too - otherwise `make ENGINE=tape` over a
# stale non-stream build relinks empty objects (undefined StreamDeck/FatFile/streamMem). Same stamp as app.o.
build/stream_deck.o build/fat_file.o build/buffer.sdram.o: build/.engine-stamp
# gen~ engines share the wrapper object basenames _ext_daisy.o / genlib_arena.o across exports (gen-dsp
# fixes the filenames), so a gen_X -> gen_Y switch must recompile them or the link pulls the previous
# engine's wrapper (undefined-reference to the other's <name>_daisy namespace). Same stamp as app.o.
# NOTE: after `scripts/gen_engine.py --remove`, run `make clean` once - the removed engine's stale
# build/_ext_daisy.d still names its now-deleted source, which make rejects before any recipe runs.
build/_ext_daisy.o build/genlib_arena.o: build/.engine-stamp
# Same class of collision between reso and mosc: BOTH vendor a file called `resources.cc` (Rings' and
# Plaits' wavetables), which compile to the same build/resources.o. A `reso` -> `mosc` switch without
# this reuses Rings' object and the link fails with `undefined reference to plaits::lut_sine` - which
# reads like a missing source rather than a stale object, and is why `make engine-mosc` opens with a
# full `make clean`. (The clean is still there and still correct; this makes a plain
# `make ENGINE=mosc` after a reso build work too, as the README promises for every engine.)
build/resources.o: build/.engine-stamp
# Basename-colliding objects are DELETED on an engine switch, along with their .d files, rather than
# merely declared out of date. Listing them as prerequisites (above) is not enough: the stale
# build/<name>.d still names the OTHER engine's source as a prerequisite of build/<name>.o, and the
# pattern rule compiles `$<` - the first prerequisite - so make dutifully rebuilds resources.o from
# rings/resources.cc during a mosc build and the link still fails on `plaits::lut_sine`. Removing the
# .d removes the stale prerequisite, after which vpath resolves against the current engine's sources.
# This is also what the gen~ note above warns about for `--remove`; deleting the .d handles that too.
#
# The list: three of these are reso-vs-mosc, because Rings and Plaits are the same author's codebases
# and reuse file names (rings/dsp/{resonator,string}.cc + rings/resources.cc vs the Plaits equivalents).
# The other two are the gen~ wrapper objects, whose names gen-dsp fixes across exports. Regenerate with:
#
#   find src -name '*.cc' -o -name '*.cpp' | xargs -n1 basename | sort | uniq -d
#
ENGINE_COLLIDING_OBJS = resources resonator string _ext_daisy genlib_arena
build/.engine-stamp: FORCE | $(BUILD_DIR)
	@echo '$(ENGINE)' | cmp -s - $@ 2>/dev/null || { echo '$(ENGINE)' > $@; \
	  rm -f $(addprefix $(BUILD_DIR)/,$(addsuffix .o,$(ENGINE_COLLIDING_OBJS)) $(addsuffix .d,$(ENGINE_COLLIDING_OBJS))); }

# Same trick for the baked-in version string: -DSPK_VERSION_STR is invisible to make's dependency
# graph, so without this a new commit (new `git describe`) would leave a stale version in the
# binary. version.o holds the banner literal; app.o logs it at boot - both recompile when the
# stamp's content changes (i.e. when SPK_VERSION changes), and nothing else does.
build/version.o build/app.o: build/.version-stamp
build/.version-stamp: FORCE | $(BUILD_DIR)
	@echo '$(SPK_VERSION)' | cmp -s - $@ 2>/dev/null || echo '$(SPK_VERSION)' > $@

# Build-flag stamps. -DSPK_TERMINAL / -DTERM_USBDIAG / -DSPK_TERMINAL_PORT_EXTERNAL are invisible to
# make's dependency graph, so without these a `make TERMINAL=1` over a stale tree would silently keep
# objects compiled the other way.
#
# These flags change TYPE LAYOUT, not just behaviour: SPK_TERMINAL adds members to CoreUI (_input_frozen)
# and AppImpl (_terminal) and virtuals to IEngine; TERM_USBDIAG adds another CoreUI member. Mixing
# objects built with and without them puts every later member at the wrong offset - which presents as a
# frozen/garbled panel while the terminal itself still works, and is maddening to diagnose. So a change
# must invalidate EVERYTHING, not just the TUs that mention the flag.
#
# Why the objects are deleted rather than just out-dated: this project builds under GNU Make 3.81
# (what macOS ships), which compares mtimes at WHOLE-SECOND resolution. A stamp rewritten in the same
# second an object was compiled is not "newer", so the object is skipped - producing a PARTIAL rebuild
# whose stale subset depends on timing. That is exactly the layout-mismatch case above, and it bit us
# on hardware (see docs/dev/terminal-impl.md). Deleting the objects is immune to timestamp resolution.
#
# The stamps are prerequisites of every object, so all the recipes run before any compilation starts;
# the rm therefore cannot race a concurrent -j compile.
#
# -DSPK_GRAIN_GF is in the same class and is why granular <-> graincloud needs one. Those two engines
# compile THE SAME granular tree to the same object basenames (build/generator.o, build/deck.o, ...),
# and the flag adds members to Generator - so a `make ENGINE=granular` over a stale graincloud tree
# would otherwise reuse objects whose Generator is two members longer, i.e. the layout mismatch above.
# The .engine-stamp above cannot cover this: it only re-makes a named handful of objects, and here the
# whole shared tree is affected. (Before the dedup the two engines had separate directories and this
# could not arise; deleting ~3,400 duplicated lines is what makes the stamp necessary.)
#
# -DSPK_USB_MIDI needs one for a subtler reason than layout: it defaults ON for BOOT_QSPI and OFF
# otherwise (see USB_MIDI below), so switching from any SRAM engine to a QSPI one reuses platform
# objects compiled WITHOUT it. The result builds, links, boots, and silently ignores device MIDI -
# byte-for-byte the failure the CMake gap produced (see CHANGELOG), reachable from the Makefile too:
# a clean `mosc` image is ~304 KB, the same image built over a stale reso tree ~292 KB.
$(OBJECTS): build/.terminal-stamp build/.osc-stamp build/.usbdiag-stamp build/.termport-stamp \
            build/.grainflavor-stamp build/.usbmidi-stamp

build/.usbmidi-stamp: FORCE | $(BUILD_DIR)
	@echo '$(USB_MIDI)' | cmp -s - $@ 2>/dev/null || { echo '$(USB_MIDI)' > $@; rm -f $(BUILD_DIR)/*.o; }

build/.grainflavor-stamp: FORCE | $(BUILD_DIR)
	@echo '$(ENGINE)' | grep -qx graincloud && echo 1 > $(BUILD_DIR)/.gf-want || echo 0 > $(BUILD_DIR)/.gf-want
	@cmp -s $(BUILD_DIR)/.gf-want $@ 2>/dev/null || { cp $(BUILD_DIR)/.gf-want $@; rm -f $(BUILD_DIR)/*.o; }

build/.terminal-stamp: FORCE | $(BUILD_DIR)
	@echo '$(TERMINAL)' | cmp -s - $@ 2>/dev/null || { echo '$(TERMINAL)' > $@; rm -f $(BUILD_DIR)/*.o; }

# SPK_TERMINAL_OSC changes TYPE LAYOUT, not just behaviour: it makes TextSink virtual (adding a vtable
# pointer), grows TxFifo's buffer 2 KB -> 8 KB, adds a member to TermState and swaps LineAssembler for
# SlipAssembler inside Terminal. Mixing objects across the flag is exactly the silent-corruption class
# the other stamps exist to prevent.
build/.osc-stamp: FORCE | $(BUILD_DIR)
	@echo '$(OSC)' | cmp -s - $@ 2>/dev/null || { echo '$(OSC)' > $@; rm -f $(BUILD_DIR)/*.o; }

build/.usbdiag-stamp: FORCE | $(BUILD_DIR)
	@echo '$(USBDIAG)' | cmp -s - $@ 2>/dev/null || { echo '$(USBDIAG)' > $@; rm -f $(BUILD_DIR)/*.o; }

build/.termport-stamp: FORCE | $(BUILD_DIR)
	@echo '$(TERMPORT)' | cmp -s - $@ 2>/dev/null || { echo '$(TERMPORT)' > $@; rm -f $(BUILD_DIR)/*.o; }

.PHONY: FORCE
FORCE:

# Platform/engine boundary guard (Phase 5 R4b). The platform (hw/ui/memory) must reach the engine
# ONLY through the contract headers in src/engine/, never the granular DSP under src/engine/granular/.
# app.cpp is exempt: it is the composition root that instantiates the concrete ActiveEngine via
# engine_select.h. Wired as a prerequisite of `all`, so a `make` that reintroduces a granular include
# into hw/ui/memory fails - the boundary is enforced by the build, not by convention/review.
PLATFORM_DIRS = src/hw src/ui src/memory src/transport
.PHONY: check-boundary
check-boundary:
	@if grep -rn '#include "engine/granular/\|#include "\.\./engine/granular/' $(PLATFORM_DIRS) ; then \
		echo "*** BOUNDARY VIOLATION: a platform TU (hw/ui/memory) includes granular DSP (above)."; \
		echo "*** The platform must use only the contract headers in src/engine/. See docs/engine-layout.md."; \
		exit 1; \
	fi
	@echo "boundary OK: hw/ui/memory include no engine/granular/ headers"

all: check-boundary

# On-target test harness (docs/dev/terminal-tools.md): drives a flashed, TERMINAL=1 device over the
# USB-C CDC port via tools/skdev. Distinct from `test` (off-target host/ unit tests) - it needs real
# hardware, and no-ops (pytest.skip) when no device is attached, so it is safe in a hardware-free CI.
.PHONY: test-hw
# Host-side Python for the on-target harness. Prefers the project venv (this repo uses uv, which
# creates .venv), then uv itself, then a bare python3 - a plain `python3` is usually the SYSTEM
# interpreter, which has neither pyserial nor pytest. Absolute path because the recipe cd's into tools/.
# Override for anything unusual: `make test-hw PYTHON=/path/to/python`.
PYTHON ?= $(shell if [ -x "$(CURDIR)/.venv/bin/python" ]; then echo "$(CURDIR)/.venv/bin/python"; \
                  elif command -v uv >/dev/null 2>&1; then echo "uv run python"; \
                  else echo python3; fi)

# `make test-hw` drives a flashed device over the LINE codec; `make test-hw CODEC=osc` drives one
# flashed `TERMINAL=1 OSC=1` over the OSC codec. Deliberately NOT spelled `OSC=1`: that variable selects
# what to BUILD, and this selects what to TALK TO - one is a firmware flag, the other a host client, and
# conflating them would make `make test-hw OSC=1` look like it flashes something.
#
# The suites are codec-agnostic - written against the client's method surface, which both clients share
# - so running both and comparing IS the cross-codec parity check docs/dev/terminal-osc.md makes the
# acceptance criterion. Layer [3] is shared byte for byte, so anything the two runs disagree about is a
# codec bug by definition.
test-hw:
	cd tools && $(PYTHON) -m pytest -q $(if $(filter osc,$(CODEC)),--osc,)

# The DEVICE-FREE half of tools/: the describe parser and the OSC codec + semantic translator, both
# checked against byte samples the off-target C++ tests emit (host/build/describe_sample.txt and
# describe_osc_sample.bin). Needs neither hardware nor pyserial, so unlike `test-hw` it belongs in CI.
# Run `make -C host test` first to refresh the samples; both tests skip cleanly without them.
.PHONY: test-tools
test-tools:
	$(TEST_PY) -m pytest -q tools/test_descriptor.py tools/test_osc_codec.py

# One-shot variant flash: clean -> build -> flash over DFU. Put the device in DFU mode first
# (hold Reset ~3s until the bottom pad LEDs breathe white), then `make granular` / `make passthrough`.
.PHONY: engine-granular engine-passthrough engine-delay engine-qdelay engine-edrums engine-reso engine-mosc program-mosc engine-graincloud engine-tape engine-shuttle engine-softcut engine-reverb engine-radio engine-bard engine-glitch engine-pstretch engine-chorus engine-filter engine-voice engine-gigaverb engine-csound program-csound engine-chuck program-chuck
engine-granular:
	$(MAKE) clean
	$(MAKE) -j8 ENGINE=granular
	$(MAKE) ENGINE=granular program-dfu

engine-passthrough:
	$(MAKE) clean
	$(MAKE) -j8 ENGINE=passthrough
	$(MAKE) ENGINE=passthrough program-dfu

engine-delay:
	$(MAKE) clean
	$(MAKE) -j8 ENGINE=delay
	$(MAKE) ENGINE=delay program-dfu

engine-qdelay:
	$(MAKE) clean
	$(MAKE) -j8 ENGINE=qdelay
	$(MAKE) ENGINE=qdelay program-dfu

engine-edrums:
	$(MAKE) clean
	$(MAKE) -j8 ENGINE=edrums
	$(MAKE) ENGINE=edrums program-dfu

engine-reso:
	$(MAKE) clean
	$(MAKE) -j8 ENGINE=reso
	$(MAKE) ENGINE=reso program-dfu

# mosc is QSPI-execute (BOOT_QSPI + linker/alt_qspi.lds): the full 24-engine Plaits voice is ~292 KB of .text,
# too big for the 186 KB SRAM_EXEC. Same recipe as csound but it KEEPS the engine arena (no own pool).
# The leading `-` on program-dfu ignores the benign get_status error on the QSPI `:leave`.
MOSC_FLAGS = ENGINE=mosc APP_TYPE=BOOT_QSPI LDSCRIPT=linker/alt_qspi.lds
engine-mosc:
	$(MAKE) clean
	$(MAKE) -j8 $(MOSC_FLAGS)
	-$(MAKE) $(MOSC_FLAGS) program-dfu

# Re-flash the last mosc build without rebuilding (board in DFU).
program-mosc:
	-$(MAKE) $(MOSC_FLAGS) program-dfu

engine-graincloud:
	$(MAKE) clean
	$(MAKE) -j8 ENGINE=graincloud
	$(MAKE) ENGINE=graincloud program-dfu

engine-tape:
	$(MAKE) clean
	$(MAKE) -j8 ENGINE=tape
	$(MAKE) ENGINE=tape program-dfu

engine-shuttle:
	$(MAKE) clean
	$(MAKE) -j8 ENGINE=shuttle
	$(MAKE) ENGINE=shuttle program-dfu

engine-softcut:
	$(MAKE) clean
	$(MAKE) -j8 ENGINE=softcut
	$(MAKE) ENGINE=softcut program-dfu

engine-radio:
	$(MAKE) clean
	$(MAKE) -j8 ENGINE=radio
	$(MAKE) ENGINE=radio program-dfu

engine-bard:
	$(MAKE) clean
	$(MAKE) -j8 ENGINE=bard
	$(MAKE) ENGINE=bard program-dfu

engine-glitch:
	$(MAKE) clean
	$(MAKE) -j8 ENGINE=glitch
	$(MAKE) ENGINE=glitch program-dfu

engine-pstretch:
	$(MAKE) clean
	$(MAKE) -j8 ENGINE=pstretch
	$(MAKE) ENGINE=pstretch program-dfu

# Csound is QSPI-only (BOOT_QSPI + the SDRAM-pool linker script, links libcsound.a). Put the board
# in DFU before the build finishes - program-dfu flashes once (no retry loop). The leading `-`
# ignores the benign get_status error on the QSPI `:leave` (the flash itself succeeds).
CSOUND_FLAGS = ENGINE=csound APP_TYPE=BOOT_QSPI LDSCRIPT=linker/alt_qspi.lds
engine-csound:
	$(MAKE) clean
	$(MAKE) -j8 $(CSOUND_FLAGS)
	-$(MAKE) $(CSOUND_FLAGS) program-dfu

# Re-flash the last csound build without rebuilding (board in DFU).
program-csound:
	-$(MAKE) $(CSOUND_FLAGS) program-dfu

# ChucK is QSPI-only, same recipe as csound but with its own linker script (linker/alt_qspi_chuck.lds reclaims
# the unused SRAM_EXEC region for .bss - csound keeps the stock linker/alt_qspi.lds). Links libchuck.a.
CHUCK_FLAGS = ENGINE=chuck APP_TYPE=BOOT_QSPI LDSCRIPT=linker/alt_qspi_chuck.lds
engine-chuck:
	$(MAKE) clean
	$(MAKE) -j8 $(CHUCK_FLAGS)
	-$(MAKE) $(CHUCK_FLAGS) program-dfu

# Re-flash the last chuck build without rebuilding (board in DFU).
program-chuck:
	-$(MAKE) $(CHUCK_FLAGS) program-dfu

engine-chorus:
	$(MAKE) clean
	$(MAKE) -j8 ENGINE=chorus
	$(MAKE) ENGINE=chorus program-dfu

engine-filter:
	$(MAKE) clean
	$(MAKE) -j8 ENGINE=filter
	$(MAKE) ENGINE=filter program-dfu

engine-voice:
	$(MAKE) clean
	$(MAKE) -j8 ENGINE=voice
	$(MAKE) ENGINE=voice program-dfu

engine-gigaverb:
	$(MAKE) clean
	$(MAKE) -j8 ENGINE=gigaverb
	$(MAKE) ENGINE=gigaverb program-dfu

# Generate a FAUST engine from a .dsp + JSON manifest (scripts/gen_faust_engine.py): builds the cyfaust
# kernel(s), emits the FaustEngine/FaustChainEngine<Traits> wrapper, and wires the build + control diagram.
# See docs/dev/engine-gen.md.   usage:  make faust-engine MANIFEST=src/engine/<name>/<name>.json
# (`engine-gen` is the former name, kept as a deprecated alias.)
.PHONY: faust-engine engine-gen
faust-engine engine-gen:
	@test -n "$(MANIFEST)" || { echo "usage: make faust-engine MANIFEST=src/engine/<name>/<name>.json"; exit 1; }
	$(GEN_PY) scripts/gen_faust_engine.py $(MANIFEST)

# Flash the Faust-generated reverb engine (Dattorro plate / Zita hall).
engine-reverb:
	$(MAKE) clean
	$(MAKE) -j8 ENGINE=reverb
	$(MAKE) ENGINE=reverb program-dfu

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
FAUST_KERNELS ?= src/engine/reverb:rv_:dattorro src/engine/reverb:rv_:zita src/engine/reverb:rv_:greyhole src/engine/tape:tfx_:tapefx src/engine/chorus:fx_:chorus src/engine/filter:fx_:filter src/engine/voice:fx_voice_:osc src/engine/voice:fx_voice_:filter
# `faust-gen` is the former name, kept as a deprecated alias.
.PHONY: faust-kernels faust-gen
faust-kernels faust-gen:
	@for spec in $(FAUST_KERNELS); do \
	  dir=$${spec%%:*}; rest=$${spec#*:}; pfx=$${rest%%:*}; nm=$${rest##*:}; ns=$$pfx$$nm; \
	  src=$$dir/$$nm.dsp; out=$$dir/faust_kernel_$$nm.h; \
	  echo "compiling $$src -> $$out (namespace spotykach::$$ns)"; \
	  PYTHONUTF8=1 $(CYFAUST_PY) -m cyfaust compile $$src -b cpp -o $$dir/.kernel.gen || exit 1; \
	  { \
	    echo '// SYNTHUX ACADEMY /////////////////////////////////////////'; \
	    echo '// SPOTYKACH ///////////////////////////////////////////////'; \
	    echo '#pragma once'; echo ''; \
	    echo "// GENERATED FILE - do not edit by hand. Regenerate with \`make faust-kernels\` (cyfaust cpp backend)."; \
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

# Regenerate every gen~ engine via gen-dsp's Daisy backend (the gen~ analogue of faust-kernels).
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
# Generate ONE gen~ engine (the gen~ analogue of `make faust-engine`). Two forms:
#   make gen-engine MANIFEST=src/engine/<name>/<name>.json   # unified: knob map from the JSON manifest
#   make gen-engine GEN_EXPORT=<gen~-export-dir>:<name>      # bootstrap: positional default knob map
# MANIFEST runs gen-dsp on the manifest's `export` then emits index_of() from its `knobs`; add NOGEN=1 to
# regenerate the glue only (reuse the synced export, no gen-dsp), FORCE=1 to overwrite the generated header.
.PHONY: gen-engine
gen-engine:
	@if [ -n "$(MANIFEST)" ]; then \
	  $(GEN_PY) scripts/gen_engine.py --manifest $(MANIFEST) $(if $(NOGEN),--no-gen) $(if $(FORCE),--force-glue); \
	elif [ -n "$(GEN_EXPORT)" ]; then \
	  spec='$(GEN_EXPORT)'; export=$${spec%:*}; name=$${spec##*:}; \
	  $(GEN_PY) scripts/gen_engine.py $$export $$name $(if $(FORCE),--force-glue); \
	else \
	  echo "usage: make gen-engine MANIFEST=src/engine/<name>/<name>.json [NOGEN=1] [FORCE=1]   (or GEN_EXPORT=<export-dir>:<name>)"; exit 1; \
	fi

.PHONY: gen-engines
gen-engines:
	@test -n "$(GEN_EXPORTS)" || { echo "set GEN_EXPORTS='<export-dir>:<name> ...' (or run scripts/gen_engine.py directly)"; exit 1; }
	@for spec in $(GEN_EXPORTS); do \
	  export=$${spec%:*}; name=$${spec##*:}; \
	  $(GEN_PY) scripts/gen_engine.py $$export $$name || exit 1; \
	done
	@echo "regenerated gen~ engines"

# Docs diagrams. Per-engine control-surface diagrams are GENERATED from a small JSON spec
# (docs/diagrams/controls/<engine>.json) through the shared d2 template by
# scripts/gen_controls_diagram.py; then every docs/diagrams/*.d2 is rendered to SVG in docs/media/ by
# the `d2` CLI (https://d2lang.com; `brew install d2`). `make diagrams` runs the whole chain
# (spec -> .d2 -> .svg) incrementally; `make controls-diagrams` regenerates just the .d2 from specs.
D2 ?= d2
DIAGRAM_PY ?= python3
CONTROL_SPECS := $(wildcard docs/diagrams/controls/*.json)
CONTROL_D2    := $(patsubst docs/diagrams/controls/%.json,docs/diagrams/%-controls.d2,$(CONTROL_SPECS))
# hand-written d2 sources, excluding the generated *-controls.d2 so each SVG is listed once
STATIC_D2     := $(filter-out docs/diagrams/%-controls.d2,$(wildcard docs/diagrams/*.d2))
DIAGRAM_SVG   := $(sort $(patsubst docs/diagrams/%.d2,docs/media/%.svg,$(STATIC_D2) $(CONTROL_D2)))
# PDFs for the CONTROL diagrams only. They are the ones the web front-end shows and the ones worth
# printing and pinning next to the hardware; the architecture diagrams are read on screen. d2 renders
# PDF natively, so this is the same source and the same tool, not a second pipeline.
CONTROL_PDF   := $(patsubst docs/diagrams/%.d2,docs/media/%.pdf,$(CONTROL_D2))

.PHONY: diagrams controls-diagrams
diagrams: $(DIAGRAM_SVG) $(CONTROL_PDF)
	@echo "diagrams up to date in docs/media/"

controls-diagrams: $(CONTROL_D2)

# JSON control-spec + template -> generated <engine>-controls.d2
docs/diagrams/%-controls.d2: docs/diagrams/controls/%.json docs/diagrams/controls-template.d2 scripts/gen_controls_diagram.py
	$(DIAGRAM_PY) scripts/gen_controls_diagram.py $< -o $@

# SVG -> PDF, via librsvg rather than d2.
#
# d2 CAN emit PDF, but only by driving a headless Chromium through Playwright, which it downloads on
# first use - and that download is currently 404ing from its CDN. rsvg-convert renders the SVG d2
# already produced, needs no browser, and keeps the text as text: the output carries the embedded
# fonts rather than rasterising the labels, which is the whole point of shipping a PDF of a diagram
# somebody intends to print.
RSVG ?= rsvg-convert
docs/media/%.pdf: docs/media/%.svg
	@command -v $(RSVG) >/dev/null 2>&1 || { echo "$(RSVG) not found - install librsvg (brew install librsvg)"; exit 1; }
	@mkdir -p $(@D)
	$(RSVG) -f pdf -o $@ $<

# any d2 source -> SVG
docs/media/%.svg: docs/diagrams/%.d2
	@command -v $(D2) >/dev/null 2>&1 || { echo "d2 not found - install from https://d2lang.com (brew install d2)"; exit 1; }
	@mkdir -p $(@D)
	$(D2) $< $@

# Build distributable, version-stamped, checksummed engine binaries into dist/<version>/ for users
# who want to download-and-flash rather than build (no ARM toolchain / cyfaust+gen-dsp venv needed).
# scripts/build_release.py does a clean build of each engine in RELEASE_ENGINES, names the artifacts
# sk-<engine>-<version>.bin (add WITH_HEX=1 for .hex too), and adds SHA256SUMS and RELEASE_NOTES.md
# (the CHANGELOG section for the version + flashing instructions). The
# script is stdlib-only, so plain python3 (no venv) suffices; override with REL_PY if needed.
#   make dist                       # describe-derived version, curated engine set
#   make dist VERSION=0.3.0         # explicit version (use the bare tag you will create)
#   make dist RELEASE_ENGINES="reverb delay"   # subset
#   make dist WITH_HEX=1            # also emit .hex (ST-Link / STM32CubeProgrammer users)
# (the toggle is WITH_HEX, not HEX: the included core Makefile already defines HEX as its
# objcopy-to-ihex command, so HEX is always non-empty and unusable as a flag here.)
REL_PY ?= python3
RELEASE_ENGINES ?=
.PHONY: dist
dist:
	RELEASE_ENGINES="$(RELEASE_ENGINES)" $(REL_PY) scripts/build_release.py $(VERSION) $(if $(WITH_HEX),--hex,)

# The same release build, driven through the opt-in CMake path instead of this Makefile. Same script,
# so the manifest, SHA256SUMS, release notes and the in-binary banner check are identical - only the
# compiler driver differs.
#
# Output goes to dist-cmake/<version>/, never dist/. That is deliberate: `make gh-release` globs
# dist/<version>/* and uploads it, so a shared output directory would make it possible to publish
# CMake-built binaries as an official release just by running two targets in the wrong order. Keeping
# the trees apart also lets you build both and diff them, which is the reason to want this at all.
#
# These binaries are NOT byte-identical to the canonical ones and are not for shipping: the CMake link
# resolves newlib's exit/atexit family from full libc rather than nano, costing ~1.25 KB of SRAM_EXEC,
# and it emits objects in a different order so nearly every address shifts. Both are understood and
# deliberate - see docs/dev/cmake-gap.md. Use this to compare the two build systems across the whole
# engine set, not to produce artifacts for users.
#   make dist-cmake                              # curated engine set, describe-derived version
#   make dist-cmake VERSION=0.6.2                # explicit version (banner is pinned to it)
#   make dist-cmake RELEASE_ENGINES="delay reso"  # a subset, for a quick comparison
.PHONY: dist-cmake
dist-cmake:
	RELEASE_ENGINES="$(RELEASE_ENGINES)" $(REL_PY) scripts/build_release.py $(VERSION) --cmake $(if $(WITH_HEX),--hex,)

# Build the base SD card and package it into dist/<version>/sk-card-<version>.zip, so a user can
# download-and-unzip a correct card instead of hand-authoring eight folder layouts in four audio
# formats. `make gh-release` globs dist/<version>/*, so the card ships with the binaries for free.
#
# Like build_release.py this is deliberately STDLIB-ONLY (plain python3, no venv, no ffmpeg): the demo
# audio is synthesized, not sampled, which also means the card carries no third-party content and the
# zip is byte-reproducible for checksumming. Override the interpreter with REL_PY.
#   make sdcard                      # describe-derived version, with demo audio
#   make sdcard VERSION=0.6.2        # explicit version (match the tag you will create)
#   make sdcard SDCARD_DEMO=0        # skeleton + configs + READMEs only, no audio
#   make sdcard SDCARD_OUT=/media/SK # write the card straight to a mounted card instead of a zip
SDCARD_DEMO ?= 1
.PHONY: sdcard
sdcard:
ifeq ($(SDCARD_OUT),)
	$(REL_PY) scripts/sk_card.py dist $(VERSION) $(if $(filter 0,$(SDCARD_DEMO)),--no-demo,)
else
	$(REL_PY) scripts/sk_card.py init $(SDCARD_OUT) --force $(if $(filter 0,$(SDCARD_DEMO)),--no-demo,)
	$(REL_PY) scripts/sk_card.py verify $(SDCARD_OUT)
endif

# Check a card (yours or a user's) against what the firmware actually accepts. Reports wrong formats,
# names too long for the directory scan, files under the 32 KB scan floor, macOS metadata stubs, and
# malformed config.txt - each with the fix. Exits non-zero if anything will not work.
#   make check-sdcard CARD=/media/SK
.PHONY: check-sdcard
check-sdcard:
	@test -n "$(CARD)" || { echo "usage: make check-sdcard CARD=/path/to/card"; exit 1; }
	$(REL_PY) scripts/sk_card.py verify $(CARD)

# Upload an already-built dist/<version>/ as a GitHub release (requires `gh auth login`). Tag the
# release with the SAME bare version so the in-binary banner matches. Run `make dist VERSION=x` first.
.PHONY: gh-release
gh-release:
	@test -n "$(VERSION)" || { echo "usage: make gh-release VERSION=0.3.0 (after make dist VERSION=0.3.0)"; exit 1; }
	@test -d dist/$(VERSION) || { echo "dist/$(VERSION) not found - run 'make dist VERSION=$(VERSION)' first"; exit 1; }
	gh release create $(VERSION) dist/$(VERSION)/* \
	  --title "sk-engines $(VERSION)" \
	  --notes-file dist/$(VERSION)/RELEASE_NOTES.md

# Run the Python script test suites (scripts/test_*.py). These cover host-side utilities
# like convert_tape_audio.py and need neither hardware nor a firmware build. pytest is part
# of the `dev` dependency group declared in pyproject.toml ([dependency-groups], PEP 735),
# installed into the repo-local .venv. Override the interpreter with `TEST_PY=/path/to/python`.
# `make test-scripts` installs the dev group on first use (when pytest is missing);
# `make test-scripts-deps` (re)installs it on demand. Needs pip >= 25.1 for `--group`.
TEST_PY ?= .venv/bin/python
.PHONY: test-scripts test-scripts-deps
test-scripts-deps:
	$(TEST_PY) -m pip install -q --group dev
test-scripts:
	@$(TEST_PY) -c 'import pytest' 2>/dev/null || $(TEST_PY) -m pip install -q --group dev
	$(TEST_PY) -m pytest scripts/

# Regenerate the web front-end's data files and cross-language test fixtures (docs/dev/web-frontend.md).
# The browser app in web/ consumes the SD card rules AS DATA rather than re-declaring them in
# JavaScript, so card_layout.json is a build artifact of scripts/card_layout.py; a hand-ported copy
# would reintroduce exactly the drift that module exists to prevent. The output is committed so the
# page is a static deploy with no build step - run this after touching card_layout.py or card_audio.py,
# and `make test-scripts` will fail if you forget.
#   make web-data                    # regenerate web/card_layout.json, patches.json and the fixtures
.PHONY: web-data
web-data:
	$(REL_PY) scripts/web_export.py

# The web front-end is TypeScript, bundled by bun to web/dist/app.js. The bundle is COMMITTED so the
# page stays a static deploy - GitHub Pages serves web/ as-is with no CI step - which means it can also
# go stale, and `make test-web` fails if it is older than src/.
#   make web-build                   # rebuild web/dist/app.js after editing web/src/
BUN ?= $(shell command -v bun 2>/dev/null)
.PHONY: web-build test-web web-serve
web-build:
	@test -n "$(BUN)" || { echo "bun not found - install it (https://bun.sh) or set BUN=/path/to/bun"; exit 1; }
	@# The CSS step shells out to `tailwindcss`, which lives in web/node_modules. A checkout that has
	@# only ever SERVED the page (which needs no install, the bundle being committed) has no node_modules
	@# and fails here with a bare `tailwindcss: command not found`. Install on demand instead. The same
	@# guard is in test-web below, but that one only fires when node_modules is missing entirely - a
	@# partial tree (typescript present, tailwind not) got past it.
	@cd web && test -x node_modules/.bin/tailwindcss || $(BUN) install
	cd web && $(BUN) run build

# Run the web front-end's test suite: the WAV writers asserted byte-identical to card_audio.py, the
# verify checker asserted to reach the same verdicts as sk_card.py on a deliberately-broken card, the
# view-models driven by fake ports (no DOM, no device), and the views mounted against a minimal DOM
# shim. Type-checks first, in two passes - `src/` strictly, the tests with null-checking relaxed.
#
# `bun install` is needed once, for TypeScript itself; nothing else is a dependency and nothing ships
# from node_modules.
#   make test-web
test-web:
	@test -n "$(BUN)" || { echo "bun not found - install it (https://bun.sh) or set BUN=/path/to/bun"; exit 1; }
	@cd web && test -d node_modules || (cd web && $(BUN) install)
	cd web && $(BUN) run typecheck
	cd web && $(BUN) test/run.ts

# Serve web/ locally. The browser APIs this app uses (File System Access, WebSerial) are only offered
# over HTTPS or localhost, and ES modules will not load from a file:// URL at all, so opening
# web/index.html directly does not work. Still a plain static server: the TypeScript is bundled ahead
# of time, so serving needs no toolchain at all.
#   make web-serve                   # builds, then http://localhost:8000
#   make web-serve SERVE_BIND=0.0.0.0   # ...and reachable from the LAN, to test on a phone
#
# Bound to the loopback address, NOT python's 0.0.0.0 default: this target said localhost while
# actually listening on every interface, so a laptop on a cafe network was serving the tree to it.
# Nothing here is secret, but a static server that follows symlinks and has no path allowlist is not
# something to hand to a strange network by accident. Override SERVE_BIND when LAN access is the point.
SERVE_PORT ?= 8000
SERVE_BIND ?= 127.0.0.1
web-serve: web-build
	@echo "http://localhost:$(SERVE_PORT)  (Ctrl-C to stop)"
	@cd web && $(REL_PY) -m http.server $(SERVE_PORT) --bind $(SERVE_BIND)

# Vendored Daisy archives. The core Makefile's link step (-ldaisy -ldaisysp) needs these built, but a
# fresh checkout has source-only submodules, so a bare `make` used to fail at link with "cannot find
# -ldaisy". Wire each archive as a file prerequisite of the elf with its own build rule: a plain `make`
# builds a missing archive on demand, but the rules have no prerequisites so the sub-make never re-runs
# on a normal rebuild once the .a exists. `make libs` still force-rebuilds; pair with `clean-libs`.
LIBDAISY_A = $(LIBDAISY_DIR)/build/libdaisy.a
DAISYSP_A = $(DAISYSP_DIR)/build/libdaisysp.a

$(BUILD_DIR)/$(TARGET).elf: $(LIBDAISY_A) $(DAISYSP_A)

$(LIBDAISY_A):
	cd $(LIBDAISY_DIR) && $(MAKE)

$(DAISYSP_A):
	cd $(DAISYSP_DIR) && $(MAKE)

.PHONY: libs
libs:
	cd $(LIBDAISY_DIR) && $(MAKE)
	cd $(DAISYSP_DIR) && $(MAKE)

clean-libs:
	cd $(LIBDAISY_DIR) && $(MAKE) clean
	cd $(DAISYSP_DIR) && $(MAKE) clean

# ---------------------------------------------------------------------------------------------------
# Aggregate test target + help
# ---------------------------------------------------------------------------------------------------
#
# `test` MUST be .PHONY. Without it, make matches the `test/` DIRECTORY, decides it is up to date, and
# a bare `make test` silently does nothing - which is exactly what the README used to have to warn
# about. The four suites below are independent and are the whole off-target contract; the on-target
# one is `make test-hw` (needs a flashed TERMINAL=1 device, and skips cleanly without one).
.PHONY: test
test:
	$(MAKE) check-boundary
	$(MAKE) -C host test
	$(MAKE) -C test test
	$(MAKE) test-scripts
	$(MAKE) test-web

# `make help` - the discoverable index. This Makefile has ~50 targets and a dozen toggles; before this
# they were findable only by reading the file or the README end to end.
.PHONY: help
help:
	@echo 'sk-engines - a Spotykach platform/engine firmware.  https://github.com/shakfu/sk-engines'
	@echo ''
	@echo 'Build (engine chosen at build time; switching ENGINE needs no clean):'
	@echo '  make -j8 libs                 build the vendored libDaisy + DaisySP archives (once)'
	@echo '  make -j8                      build the default engine (ENGINE=granular) -> build/spotykach.bin'
	@echo '  make -j8 ENGINE=<name>        build another SRAM engine (see the list below)'
	@echo '  make program-dfu              flash whatever is already in build/ (device in DFU mode)'
	@echo '  make engine-<name>            clean + build + flash in one step, e.g. make engine-delay'
	@echo ''
	@echo 'SRAM engines (ENGINE=):'
	@echo '  granular graincloud tape shuttle softcut radio bard pstretch'
	@echo '  delay qdelay reverb gigaverb chorus filter voice'
	@echo '  edrums reso glitch passthrough'
	@echo 'QSPI engines (own one-shot target, too large for execution SRAM):'
	@echo '  make engine-mosc              no prerequisites; Plaits DSP is vendored in-tree'
	@echo '  make engine-csound            needs scripts/fetch_csound.sh once (libcsound.a)'
	@echo '  make engine-chuck             needs scripts/fetch_chuck.sh once (libchuck.a)'
	@echo ''
	@echo 'Build toggles (append to any build):'
	@echo '  DEBUG=1        UART logging                LOFI_INT16=1  16-bit loop buffer (2x record time)'
	@echo '  TERMINAL=1     USB-C command channel       METER=1       CPU-load meter over CDC (excludes TERMINAL)'
	@echo '  OSC=1          OSC+SLIP codec (needs TERMINAL=1; for a Max/Pd/TouchOSC rig, excludes DEBUG)'
	@echo '  USBDIAG=1      USB bring-up blink codes    WINDOW=<n>    pstretch FFT window (4096/8192)'
	@echo ''
	@echo 'Test (off-target, no hardware needed):'
	@echo '  make test                     all four suites + the boundary guard'
	@echo '  make -C host test             engine + DSP suites          make -C test test    standalone unit tests'
	@echo '  make test-scripts             the Python host tooling       make test-web        the browser front end'
	@echo '  make check-boundary           platform must not include engine DSP'
	@echo 'Test (on-target, needs a flashed TERMINAL=1 device; skips cleanly without one):'
	@echo '  make test-hw                  pytest harness over USB-C     python tools/skterm.py   hand REPL'
	@echo ''
	@echo 'SD card:'
	@echo '  make sdcard SDCARD_OUT=/media/SK    build a complete card (folders, configs, demo audio)'
	@echo '  make check-sdcard CARD=/media/SK    explain anything that will not work, with the fix'
	@echo ''
	@echo 'Web front end (same rules as the CLI, exported as data):'
	@echo '  make web-serve                build + serve on http://localhost:8000'
	@echo '  make web-data                 regenerate web/ data + fixtures after touching scripts/'
	@echo '  make web-build                rebuild web/dist/ after touching web/src/'
	@echo ''
	@echo 'Release + codegen:'
	@echo '  make dist                     build the curated engine set into dist/<version>/'
	@echo '  make faust-kernels            regenerate the Faust engine kernels'
	@echo '  make gen-engines              regenerate the gen~ engine directories'
	@echo '  make diagrams                 regenerate the control-surface SVGs'
	@echo ''
	@echo 'Docs: README.md - docs/architecture.md - docs/engines/ - docs/engine-types/ - TODO.md'
