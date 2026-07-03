# Desktop host harness for the Spotykach DSP core.
#
# Builds the real src/engine/granular/ for the host (no libDaisy: the <daisy.h> shim under host/shim
# supplies the few symbols common.h needs). DaisySP is portable C++ and is compiled from
# source. Run from the repo root:  make -C host   then   ./host/build/host_spotykach

CXX      ?= clang++
ROOT      = ..
# reso's vendored Rings/stmlib DSP, colocated under the engine and trimmed to the used closure.
RESO_TP   = $(ROOT)/src/engine/reso/thirdparty
# stmlib (the Mutable Instruments support lib) is vendored ONCE under common/ and shared by reso + mosc
# (relocated there in the mosc-engine commit); mirror the firmware Makefile's $(STMLIB_TP).
STMLIB_TP = $(ROOT)/src/engine/common/thirdparty/stmlib
# graincloud's vendored, de-STL'd GrainflowLib (header-only) - the grain-cloud kernel.
GRAINCLOUD_TP = $(ROOT)/src/engine/graincloud/thirdparty
BUILD     = build
TARGET    = $(BUILD)/host_spotykach

DAISYSP   = $(ROOT)/lib/DaisySP/Source

# DaisySP .cpp files include sibling headers (e.g. "dsp.h") by bare name, so every Source subdirectory
# must be on the include path. src/transport/ headers (transport.h) likewise pull siblings
# (synclock.h/tempo.h) by bare name, so that dir is on the path too for TUs that include the transport.
DAISYSP_INC = $(addprefix -I,$(wildcard $(DAISYSP)/*/))
INCLUDES  = -Ishim -I$(ROOT)/src -I$(ROOT)/src/transport -I$(RESO_TP) -I$(ROOT)/src/engine/common/thirdparty -I$(GRAINCLOUD_TP) -I$(ROOT)/src/engine/softcut/vendor/include -I. -I$(DAISYSP) $(DAISYSP_INC)
# -DTEST selects stmlib's portable C paths (the ARM ssat/usat asm is gated behind #ifndef TEST), so
# the Rings DSP (under $(RESO_TP)) compiles on the desktop. Unused elsewhere in this repo.
CXXFLAGS  = -std=c++17 -O2 -DTEST -Wall -Wno-unused-variable -Wno-unused-but-set-variable $(INCLUDES)

CORE_SRC    = $(wildcard $(ROOT)/src/engine/granular/*.cpp)
# Engine-agnostic shared primitives (biquad/follower/adenv/cpattern + header-only siblings) live in
# src/dsp/ and are consumed by the granular engine via "dsp/..." includes, so compile them here too.
DSP_SRC     = $(wildcard $(ROOT)/src/dsp/*.cpp)
# The platform transport service (the clock the engine observes via the injected ITransport). The host
# plays the platform's role: it constructs a real Transport and drives transport.tick() per block.
TRANSPORT_SRC = $(wildcard $(ROOT)/src/transport/*.cpp)
# src/engine/*.cpp now includes the hardware-free LEDRing + Color impls (moved out of src/ui/ in
# Phase 5 R2), which the DisplayModel needs; the rest of src/ui/ stays out (it needs the HAL).
ENGINE_SRC  = $(wildcard $(ROOT)/src/engine/*.cpp)
DAISYSP_SRC = $(wildcard $(DAISYSP)/*/*.cpp)
LIB_SRC     = $(CORE_SRC) $(DSP_SRC) $(TRANSPORT_SRC) $(ENGINE_SRC) $(DAISYSP_SRC)
# graincloud is a separate copy of the granular tree; its test links graincloud/*.cpp INSTEAD of the
# granular CORE_SRC (same class names live in both, so never link both into one binary).
GC_CORE_SRC = $(wildcard $(ROOT)/src/engine/graincloud/*.cpp)
GC_LIB_SRC  = $(GC_CORE_SRC) $(DSP_SRC) $(TRANSPORT_SRC) $(ENGINE_SRC) $(DAISYSP_SRC)
# The edrums/reso/reverb engines live under src/engine/<name>/ (outside the wildcards above); add for tests.
DELAY_SRC   = $(ROOT)/src/engine/delay/delay_engine.cpp
# qdelay flavor: its feedback diffuser is the header-only src/dsp/diffuser.h, so just the engine .cpp.
QDELAY_SRC  = $(ROOT)/src/engine/qdelay/qdelay_engine.cpp
EDRUMS_SRC  = $(ROOT)/src/engine/edrums/edrums_engine.cpp
RESO_SRC    = $(ROOT)/src/engine/reso/reso_engine.cpp
# graincloud is the granular engine built with SPK_GRAIN_GF (gf_cloud.cpp swaps the grain DSP); there is
# no separate engine .cpp. Its GrainflowLib DSP is exercised on the host by test_graincloud_kernel.
# The reverb engine's adapter; its Faust kernels are checked-in headers (engine/reverb/faust_kernel_*.h,
# portable C++) pulled by the .cpp, so no extra sources are needed.
REVERB_SRC  = $(ROOT)/src/engine/reverb/reverb_engine.cpp
# The tape engine + its Faust tape-FX kernel (engine/tape/faust_kernel_tapefx.h, header pulled by the .cpp).
TAPE_SRC    = $(ROOT)/src/engine/tape/tape_engine.cpp
# The buffer-based shuttle engine (4 in-RAM tape tracks; bipolar/reverse varispeed). Self-contained.
SHUTTLE_SRC = $(ROOT)/src/engine/shuttle/shuttle_engine.cpp
# The dual-deck softcut overdub looper: engine wrapper + the vendored monome softcut core (5 .cpp).
SOFTCUT_SRC = $(ROOT)/src/engine/softcut/softcut_engine.cpp $(wildcard $(ROOT)/src/engine/softcut/vendor/src/*.cpp)
RADIO_SRC   = $(ROOT)/src/engine/radio/radio_engine.cpp
# The dual-deck glitch/lo-fi noise engine (12 Noisferatu algorithms, header-only Voice). Self-contained.
GLITCH_SRC  = $(ROOT)/src/engine/glitch/glitch_engine.cpp
# The real-time PaulStretch ambient engine (vendored radix-2 FFT + Stretcher, header-only). Self-contained.
PSTRETCH_SRC = $(ROOT)/src/engine/pstretch/pstretch_engine.cpp
# reso's DSP is the Mutable Instruments Rings engine + its stmlib support library (under $(RESO_TP)).
RINGS_SRC   = $(RESO_TP)/rings/dsp/part.cc $(RESO_TP)/rings/dsp/string.cc \
              $(RESO_TP)/rings/dsp/resonator.cc $(RESO_TP)/rings/dsp/fm_voice.cc \
              $(RESO_TP)/rings/resources.cc \
              $(STMLIB_TP)/dsp/units.cc $(STMLIB_TP)/utils/random.cc \
              $(STMLIB_TP)/dsp/atan.cc

SRC          = main_host.cpp $(LIB_SRC)
TEST         = $(BUILD)/test_engine_params
TEST_EDRUMS  = $(BUILD)/test_edrums_slots
TEST_RESO    = $(BUILD)/test_reso
TEST_REVERB  = $(BUILD)/test_reverb
TEST_TAPE    = $(BUILD)/test_tape
TEST_STREAM  = $(BUILD)/test_stream
TEST_WAV_CUES = $(BUILD)/test_wav_cues
TEST_SHUTTLE = $(BUILD)/test_shuttle
TEST_SOFTCUT = $(BUILD)/test_softcut
TEST_RADIO   = $(BUILD)/test_radio
TEST_GLITCH  = $(BUILD)/test_glitch
TEST_PSTRETCH = $(BUILD)/test_pstretch
TEST_CHORUS  = $(BUILD)/test_chorus
TEST_FILTER = $(BUILD)/test_filter
TEST_VOICE   = $(BUILD)/test_voice
TEST_DELAY   = $(BUILD)/test_delay
TEST_QDELAY  = $(BUILD)/test_qdelay
TEST_DIFFUSER = $(BUILD)/test_diffuser
TEST_GRAINCLOUD_KERNEL = $(BUILD)/test_graincloud_kernel
TEST_CSOUND_ALLOC = $(BUILD)/test_csound_alloc
TEST_CSOUND_PATCH = $(BUILD)/test_csound_patch
TEST_CSOUND_MIDI  = $(BUILD)/test_csound_midi
TEST_CSOUND_RELOAD = $(BUILD)/test_csound_reload
TEST_CHUCK_PATCH = $(BUILD)/test_chuck_patch
BENCH_REVERB     = $(BUILD)/bench_reverb
BENCH_GRAINCLOUD = $(BUILD)/bench_graincloud

.PHONY: all clean run test test-granular test-edrums test-reso test-graincloud-kernel test-reverb test-delay test-qdelay test-diffuser test-tape test-stream test-wav-cues test-shuttle test-radio test-glitch test-pstretch test-chorus test-filter test-voice test-csound-alloc test-csound-patch test-csound-midi test-csound-reload test-chuck-patch bench-reverb bench-graincloud
all: $(TARGET)

$(TARGET): $(SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(SRC) -o $@

$(TEST): test_engine_params.cpp $(LIB_SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) test_engine_params.cpp $(LIB_SRC) -o $@

$(TEST_EDRUMS): test_edrums_slots.cpp $(LIB_SRC) $(EDRUMS_SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) test_edrums_slots.cpp $(LIB_SRC) $(EDRUMS_SRC) -o $@

$(TEST_RESO): test_reso.cpp $(LIB_SRC) $(RESO_SRC) $(RINGS_SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) test_reso.cpp $(LIB_SRC) $(RESO_SRC) $(RINGS_SRC) -o $@

# Csound SDRAM allocator (CsoundPool). Header-only, no libcsound/Daisy deps, so it compiles alone.
$(TEST_CSOUND_ALLOC): test_csound_alloc.cpp $(ROOT)/src/engine/csound/csound_pool.h | $(BUILD)
	$(CXX) $(CXXFLAGS) test_csound_alloc.cpp -o $@
test-csound-alloc: $(TEST_CSOUND_ALLOC)
	./$(TEST_CSOUND_ALLOC)

# Csound orchestra selector (SD patch / built-in fallback). Header-only logic + a fake IStreamDeck,
# no libcsound/Daisy deps, so it compiles alone.
$(TEST_CSOUND_PATCH): test_csound_patch.cpp $(ROOT)/src/engine/csound/csound_patch.h \
                      $(ROOT)/src/engine/istreamdeck.h | $(BUILD)
	$(CXX) $(CXXFLAGS) test_csound_patch.cpp -o $@
test-csound-patch: $(TEST_CSOUND_PATCH)
	./$(TEST_CSOUND_PATCH)

# Csound MIDI plumbing (note->Hz, p-field builder, lock-free note ring). Header-only; -pthread for the
# SPSC cross-thread test. No libcsound/Daisy deps.
$(TEST_CSOUND_MIDI): test_csound_midi.cpp $(ROOT)/src/engine/csound/csound_midi.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -pthread test_csound_midi.cpp -o $@
test-csound-midi: $(TEST_CSOUND_MIDI)
	./$(TEST_CSOUND_MIDI)

# Csound live-patch-switch handoff (ReloadGate). Lock-free ISR/main-loop sequencing; -pthread for the
# concurrent use-after-free detector. No libcsound/Daisy deps.
$(TEST_CSOUND_RELOAD): test_csound_reload.cpp $(ROOT)/src/engine/csound/csound_reload.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -pthread test_csound_reload.cpp -o $@
test-csound-reload: $(TEST_CSOUND_RELOAD)
	./$(TEST_CSOUND_RELOAD)

# ChucK program selector (SD patch / built-in fallback). Header-only logic + a fake IStreamDeck, no
# libchuck/Daisy deps, so it compiles alone.
$(TEST_CHUCK_PATCH): test_chuck_patch.cpp $(ROOT)/src/engine/chuck/chuck_patch.h \
                     $(ROOT)/src/engine/istreamdeck.h | $(BUILD)
	$(CXX) $(CXXFLAGS) test_chuck_patch.cpp -o $@
test-chuck-patch: $(TEST_CHUCK_PATCH)
	./$(TEST_CHUCK_PATCH)

# graincloud kernel smoke test (the de-STL'd GrainflowLib only; no engine/LIB deps).
$(TEST_GRAINCLOUD_KERNEL): test_graincloud_kernel.cpp $(wildcard $(GRAINCLOUD_TP)/grainflow/*.h) | $(BUILD)
	$(CXX) $(CXXFLAGS) test_graincloud_kernel.cpp -o $@

# graincloud full-engine test (links the graincloud tree, NOT granular).
TEST_GRAINCLOUD = $(BUILD)/test_graincloud
$(TEST_GRAINCLOUD): test_graincloud.cpp $(GC_LIB_SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) test_graincloud.cpp $(GC_LIB_SRC) -o $@
test-graincloud: $(TEST_GRAINCLOUD)
	./$(TEST_GRAINCLOUD)

# graincloud scattered-read benchmark (self-contained; the #11 feasibility gate).
$(BENCH_GRAINCLOUD): bench_graincloud.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) bench_graincloud.cpp -o $@

# The generated Faust kernels are headers (not in $(*_SRC)), so list them explicitly as prerequisites
# - else `make faust-gen` followed by `make test-*` would relink against a stale binary.
$(TEST_REVERB): test_reverb.cpp $(LIB_SRC) $(REVERB_SRC) $(wildcard $(ROOT)/src/engine/reverb/faust_kernel_*.h) | $(BUILD)
	$(CXX) $(CXXFLAGS) test_reverb.cpp $(LIB_SRC) $(REVERB_SRC) -o $@

$(TEST_TAPE): test_tape.cpp $(LIB_SRC) $(TAPE_SRC) $(ROOT)/src/engine/tape/faust_kernel_tapefx.h | $(BUILD)
	$(CXX) $(CXXFLAGS) test_tape.cpp $(LIB_SRC) $(TAPE_SRC) -o $@

$(TEST_DELAY): test_delay.cpp $(LIB_SRC) $(DELAY_SRC) $(ROOT)/src/engine/delay/delay_engine.h | $(BUILD)
	$(CXX) $(CXXFLAGS) test_delay.cpp $(LIB_SRC) $(DELAY_SRC) -o $@

$(TEST_QDELAY): test_qdelay.cpp $(LIB_SRC) $(QDELAY_SRC) $(ROOT)/src/engine/qdelay/qdelay_engine.h $(ROOT)/src/dsp/diffuser.h | $(BUILD)
	$(CXX) $(CXXFLAGS) test_qdelay.cpp $(LIB_SRC) $(QDELAY_SRC) -o $@

# The diffuser is header-only (src/dsp/diffuser.h, no JUCE, no heap in the audio path), so its test
# needs no other sources - just the include path.
$(TEST_DIFFUSER): test_diffuser.cpp $(ROOT)/src/dsp/diffuser.h | $(BUILD)
	$(CXX) $(CXXFLAGS) test_diffuser.cpp -o $@

$(TEST_SHUTTLE): test_shuttle.cpp $(LIB_SRC) $(SHUTTLE_SRC) $(ROOT)/src/engine/shuttle/shuttle_engine.h | $(BUILD)
	$(CXX) $(CXXFLAGS) test_shuttle.cpp $(LIB_SRC) $(SHUTTLE_SRC) -o $@

# softcut overdub looper. M_PI/M_PI_2 for the vendored core (macOS clang exposes them, but be explicit
# for portability), as the firmware Makefile does.
$(TEST_SOFTCUT): test_softcut.cpp $(LIB_SRC) $(SOFTCUT_SRC) $(ROOT)/src/engine/softcut/softcut_engine.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -DM_PI=3.14159265358979323846 -DM_PI_2=1.57079632679489661923 test_softcut.cpp $(LIB_SRC) $(SOFTCUT_SRC) -o $@

# Radio engine + the headerless raw-stream codec (raw_stream.h, header pulled by the test).
$(TEST_RADIO): test_radio.cpp $(LIB_SRC) $(RADIO_SRC) $(ROOT)/src/engine/radio/radio_engine.h \
               $(ROOT)/src/memory/raw_stream.h | $(BUILD)
	$(CXX) $(CXXFLAGS) test_radio.cpp $(LIB_SRC) $(RADIO_SRC) -o $@

# Glitch engine + its header-only Voice (no extra deps).
$(TEST_GLITCH): test_glitch.cpp $(LIB_SRC) $(GLITCH_SRC) $(ROOT)/src/engine/glitch/glitch_engine.h \
                $(ROOT)/src/engine/glitch/glitch_voice.h | $(BUILD)
	$(CXX) $(CXXFLAGS) test_glitch.cpp $(LIB_SRC) $(GLITCH_SRC) -o $@

# PaulStretch ambient engine + its header-only FFT/Stretcher (no extra deps).
$(TEST_PSTRETCH): test_pstretch.cpp $(LIB_SRC) $(PSTRETCH_SRC) $(ROOT)/src/engine/pstretch/pstretch_engine.h \
                  $(ROOT)/src/engine/pstretch/pstretch_voice.h $(ROOT)/src/engine/pstretch/fft.h | $(BUILD)
	$(CXX) $(CXXFLAGS) test_pstretch.cpp $(LIB_SRC) $(PSTRETCH_SRC) -o $@

# Generated Faust `chorus` engine - header-only (the cyfaust kernel + the shared FaustEngine wrapper),
# so it links against LIB_SRC with no extra engine .cpp.
$(TEST_CHORUS): test_chorus.cpp $(LIB_SRC) $(ROOT)/src/engine/chorus/chorus_engine.h \
                $(ROOT)/src/engine/chorus/faust_kernel_chorus.h | $(BUILD)
	$(CXX) $(CXXFLAGS) test_chorus.cpp $(LIB_SRC) -o $@

# Generated Faust `filter` (parallel DoubleMono dual-deck) and `voice` (series chain dual-deck) engines -
# header-only like chorus (shared FaustEngine / FaustChainEngine wrapper + cyfaust kernels), no extra .cpp.
$(TEST_FILTER): test_filter.cpp $(LIB_SRC) $(ROOT)/src/engine/filter/filter_engine.h \
                 $(ROOT)/src/engine/filter/faust_kernel_filter.h | $(BUILD)
	$(CXX) $(CXXFLAGS) test_filter.cpp $(LIB_SRC) -o $@

$(TEST_VOICE): test_voice.cpp $(LIB_SRC) $(ROOT)/src/engine/voice/voice_engine.h \
               $(wildcard $(ROOT)/src/engine/voice/faust_kernel_*.h) | $(BUILD)
	$(CXX) $(CXXFLAGS) test_voice.cpp $(LIB_SRC) -o $@

# Reverb process() cost benchmark. One binary times both topologies (Stereo vs DoubleMono) by switching
# the Route at runtime. The ratio is the portable signal; absolute us is host time, not device load.
REVERB_KERNELS = $(wildcard $(ROOT)/src/engine/reverb/faust_kernel_*.h)
$(BENCH_REVERB): bench_reverb.cpp $(LIB_SRC) $(REVERB_SRC) $(REVERB_KERNELS) | $(BUILD)
	$(CXX) $(CXXFLAGS) bench_reverb.cpp $(LIB_SRC) $(REVERB_SRC) -o $@

bench-reverb: $(BENCH_REVERB)
	./$(BENCH_REVERB)

# The SD-streaming core (SpscRing + Play/RecordStream) is self-contained header-only logic with no
# engine/Daisy/FatFs deps, so its test compiles on its own.
$(TEST_STREAM): test_stream.cpp $(ROOT)/src/memory/spsc_ring.h $(ROOT)/src/memory/audio_stream.h \
                $(ROOT)/src/memory/byte_file.h $(ROOT)/src/memory/wav_stream.h $(ROOT)/src/memory/wav.h \
                $(ROOT)/src/engine/istreamdeck.h | $(BUILD)
	$(CXX) $(CXXFLAGS) test_stream.cpp -o $@

$(TEST_WAV_CUES): test_wav_cues.cpp $(ROOT)/src/memory/wav.h $(ROOT)/src/engine/wav_cues.h | $(BUILD)
	$(CXX) $(CXXFLAGS) test_wav_cues.cpp -o $@

$(BUILD):
	mkdir -p $(BUILD)

run: $(TARGET)
	./$(TARGET)

# `test` runs both engine harnesses. Both inject the transport via EngineContext (granular drives the
# real platform Transport; edrums uses a small stub ITransport) - the post-transport-decoupling way.
test: test-granular test-edrums test-reso test-graincloud-kernel test-reverb test-delay test-qdelay test-diffuser test-tape test-stream test-wav-cues test-shuttle test-softcut test-radio test-glitch test-pstretch test-chorus test-filter test-voice test-csound-alloc test-csound-patch test-csound-midi test-csound-reload test-chuck-patch

test-granular: $(TEST)
	./$(TEST)

test-edrums: $(TEST_EDRUMS)
	./$(TEST_EDRUMS)

test-reso: $(TEST_RESO)
	./$(TEST_RESO)

test-graincloud-kernel: $(TEST_GRAINCLOUD_KERNEL)
	./$(TEST_GRAINCLOUD_KERNEL)

bench-graincloud: $(BENCH_GRAINCLOUD)
	./$(BENCH_GRAINCLOUD)

test-reverb: $(TEST_REVERB)
	./$(TEST_REVERB)

test-delay: $(TEST_DELAY)
	./$(TEST_DELAY)

test-qdelay: $(TEST_QDELAY)
	./$(TEST_QDELAY)

test-diffuser: $(TEST_DIFFUSER)
	./$(TEST_DIFFUSER)

test-tape: $(TEST_TAPE)
	./$(TEST_TAPE)

test-stream: $(TEST_STREAM)
	./$(TEST_STREAM)

test-wav-cues: $(TEST_WAV_CUES)
	./$(TEST_WAV_CUES)

test-shuttle: $(TEST_SHUTTLE)
	./$(TEST_SHUTTLE)

test-softcut: $(TEST_SOFTCUT)
	./$(TEST_SOFTCUT)

test-radio: $(TEST_RADIO)
	./$(TEST_RADIO)

test-glitch: $(TEST_GLITCH)
	./$(TEST_GLITCH)

test-pstretch: $(TEST_PSTRETCH)
	./$(TEST_PSTRETCH)

test-chorus: $(TEST_CHORUS)
	./$(TEST_CHORUS)

test-filter: $(TEST_FILTER)
	./$(TEST_FILTER)

test-voice: $(TEST_VOICE)
	./$(TEST_VOICE)

clean:
	rm -rf $(BUILD)
