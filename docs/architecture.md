# Spotykach 'engines' Architecture

This document describes the big-picture architecture of the Spotykach firmware: the hardware platform, the platform/engine decoupling, the execution model, how the subsystems fit together, and how audio and control data flow through the system. For a file-by-file map of the source tree see [source-guide.md](source-guide.md). The platform/engine refactor those sections describe is **complete**; its running notes (`dev/refactor-status.md`, `dev/item3-plan.md`) and the original code review (`dev/review-260529.md`) have been retired into [`CHANGELOG.md`](../CHANGELOG.md). For open work see [`TODO.md`](../TODO.md), and for the current review see [`REVIEW.md`](../REVIEW.md).

> **Refactor status note.** The firmware is mid-transformation from a monolith into a > platform + swappable engine. Sections 3-4 describe the decoupled design and mark, where it > matters, what is implemented today versus what the remaining refactor items finish. The > hardware (section 2), DSP graph (section 7), and memory model (section 8) are accurate as-is.

## 1. What the instrument is

Spotykach is a screenless, dual-deck looping/sampling instrument. Each of the two decks (A and B) records audio into its own loop buffer and plays it back through a granular engine that can operate in three modes:

- **Reel** - tape emulation; playback speed and pitch are linked (monophonic).

- **Slice** - digital sampler/looper with independent pitch and speed; up to 3-voice polyphony.

- **Drift** - granular texture generator that spreads many overlapping grains from a short recording.

On top of the two decks sit shared facilities: a clock/transport with internal, TRS (4 PPQN) and MIDI (24 PPQN) sync; per-deck modulation sources (LFO or envelope follower) with CV outputs; two FX families per deck (Grit = saturation/bitcrush, Flux = tape delay); an A/B crossfade; routing/panning (mono, stereo, generative stereo); a per-deck step sequencer; and SD-card sample storage organised as 6 "tapes" of 6 slots.

This granular looper is the **reference engine** for the platform/engine design (section 3): all of the above except the granular DSP itself is platform behaviour reusable by other engines.

The feature set documented in the user manual is the granular engine's; [`manual.md`](manual.md) is the firmware-tracked copy, kept in sync with the code in the same commit as a behaviour change.

## 2. Hardware platform

The firmware targets an Electro-Smith **Daisy Seed** (STM32H7, Cortex-M7) and is built against a pinned `bleeptools` fork of libDaisy plus DaisySP. Audio runs at **48 kHz** with a **96-sample** block. The application is linked to run from SRAM and is started by a custom Daisy bootloader (`APP_TYPE = BOOT_SRAM`, linker script `alt_sram.lds`, `bootloader-spotykach-v2.bin`), which is why firmware is flashed over USB DFU from the rear USB-C port rather than the Seed's own port.

Board I/O, all owned and initialised by the `Hardware` class (`src/hw/hardware.h`):

| Function          | Device / bus                       | Notes                                  |
|-------------------|------------------------------------|----------------------------------------|
| Audio in/out      | Daisy SAI codec                    | 48 kHz, 96-sample block                |
| Mod CV out (x2)   | Daisy DAC (DMA)                    | per-deck LFO/mod, 0..+5V               |
| Pots/sliders (15) | `AnalogControl` over analog muxes  | 7 per deck + crossfade                 |
| CV inputs (7)     | `AnalogControl`, bipolar           | size/pos, mix, V/oct per deck + xfade  |
| Pads (12)         | MPR121 capacitive touch (I2C)      | play/rev/grit/flux/seq per deck, spot, alt |
| LEDs (~98)        | WS2812 chain (PWM+DMA)             | two 32-LED rings + indicators          |
| Switches          | two 74HC165 shift registers (SPI-like) | mode, routing, CV-target, tap, trims |
| Clock/gate        | GPIO                               | clock in, gate in A/B, gate out A/B    |
| MIDI              | UART                               | 24 PPQN clock + notes, in and out      |
| Storage           | SD card over SDMMC + FatFS         | FAT32, `SK/` directory                 |

## 3. Platform and engine: the decoupled design

The firmware is a fixed hardware/UI **platform** that hosts one swappable DSP **engine** per firmware build. The intent: one board, one interaction language (multi-function knobs with `MValue` pickup, ring feedback, pad gestures, transport, SD tape storage, CV/gate, MIDI), and many possible instruments. A "firmware variant" is a different `IEngine` implementation behind the same platform. The granular looper is the first/reference engine; the payoff is that engine #2 is small because it inherits the whole platform.

**One engine per binary.** Bare-metal, single image, no RTOS, no heap on the audio path, booting from SRAM with very little headroom (hundreds of bytes free). Two engines neither fit nor make sense in one image, so engine choice is a **build-time** decision (section 4), not runtime.

### Layer map

Dependencies point downward; nothing below depends on anything above it.

```text
            app.cpp / main.cpp          boot + wiring + the 4 execution contexts (section 5)
                  |
      +-----------+------------------------------+
      |                                          |
  src/ui/  (CoreUI)  --- the PLATFORM ---     src/memory/ (Settings, Storage)
      |   pots/pads/CV/MIDI/LEDs, MValue pickup,     |   config.txt + SD sample tape
      |   gesture grammar, transport, value display  |
      |                                          |
      +------------------ IEngine ---------------+    <-- THE SEAM (src/engine/iengine.h)
                          |
                  src/engine/  (the contract + engines)
                  IEngine, GranularEngine, DelayEngine, PassthroughEngine,
                  ParamId/Capabilities, DisplayModel, engine_leds
                          |
                  src/engine/granular/  (the granular DSP graph)   <-- engine-private, platform-independent
                  Core, Deck, Generator/Vox, Buffer, Fx, Driver, Track, Panner, Modulator
                          |
                  src/hw/  (HAL: Daisy peripherals, SDRAM pool, board I/O)

      src/dsp/  (engine-agnostic primitives: lutsinosc, smooth, deline, hann)  <-- shared base; platform + engines depend on it, never the reverse
```

- **`src/ui/` (CoreUI) - the platform.** Owns the interaction grammar and talks to the engine *only* through `IEngine`. Reads hardware, runs `MValue` pickup / `Hold` timers / the `_touched` modifier bitset / gesture recognition, composites LED overlays, owns the storage/tape state machine. It does not know what the engine *is*.

- **`src/engine/` - the seam + engines.** `IEngine` is the contract; each engine implements it and owns its DSP graph privately.

- **`src/engine/granular/` - the granular engine's private DSP.** Now platform-independent (no libDaisy include; all hardware injected via `EngineContext`). It is *not* a shared layer - it belongs to `GranularEngine`. A different engine brings its own DSP, not `Core`.

- **`src/dsp/` - engine-agnostic primitives.** Small reusable pieces (sine LUT, one-pole smoother, delay line, Hann window) that any engine or the platform may use; the dependency flows toward `dsp/`, never out of it. A primitive moves here once it has a real second consumer.

- **`src/hw/`, `src/memory/` - HAL and persistence.** Platform-side.

**The boundary is build-enforced.** The platform (`src/hw/`, `src/ui/`, `src/memory/`) includes *zero* granular headers - it reaches the engine only through the contract in `src/engine/`. `make check-boundary` (a prerequisite of `all`) greps those directories and fails the build if a granular include is reintroduced. Only `app.cpp`, the composition root that instantiates the build-selected engine via `engine_select.h`, is exempt. See [engine-layout.md](engine-layout.md) for the contract map and Phase 5 history.

### The `IEngine` seam

`src/engine/iengine.h` is the entire contract between platform and engine:

- **Audio lifecycle - pure virtual, required of every engine:** `init(EngineContext&)`, `prepare()`, `process(in, out, size)`. The audio path is `app -> IEngine -> engine -> DSP`.

- **Everything else - virtual with no-op default bodies (Strategy A).** An engine overrides only what it supports. The platform-driven surface includes `set_param`/`param`, `handle_midi_*`, `set_fx`/`on_*_pad`/`on_seq_*`, `cv_*`/`on_gate_*`, the `audio_*` storage port, the `transport_*` group, `process_cv`, the LED queries (`*_leds`/`render_ring`, transitional), and `render(DisplayModel&)`.

- **Capability opt-in.** `capabilities()` returns a bitset (`CapRecording`, `CapTapeStorage`, `CapStepSequencer`, `CapLaunchQuant`, `CapTransport`, `CapDualDeck`). The platform offers recording/tape/sequencer as a *toolkit*; a non-looper returns a smaller set and does not override those methods. Granular concepts are never mandatory platform behaviour.

Because the defaults are inert, adding a method to `IEngine` does not break existing engines, and a minimal engine (the `PassthroughEngine` sketch, `capabilities() == CapTransport`) is a few dozen lines.

**Parameters and the mode fanout.** `ParamId` (`src/engine/engine_params.h`) is the logical parameter vocabulary the platform speaks. The platform decides *which* `ParamId` a physical control drives; the engine owns what the param means, including mode-dependent dispatch (e.g. granular `Size` -> window-spread in Drift, loop-size otherwise). The platform never sees the engine's `Mode`.

### Hardware-free core via `EngineContext`

`src/engine/engine_context.h` is how the DSP graph avoids any libDaisy dependency. At `init()` the platform fills an `EngineContext` - `sample_rate`, `block_size`, an `ITimeSource` (clock abstraction; `daisy::System` on hardware, a host clock off-target), and `EngineBuffers` (the large SDRAM buffers, from `SDRAMBuffer::pool()` on hardware or `malloc` on the host) - and passes it to `engine.init(ctx)`. The core pulls nothing from hardware directly. This is also what lets the desktop host harness (`host/`) run the real engine over WAV via `IEngine`.

> **Seam impurity (known):** `EngineBuffers` is currently shaped for the granular engine > (`source`/`detect`/`delay`/`slices`/`track` per deck). A second engine with different buffer > needs would force this to generalise (e.g. an opaque arena the engine sub-allocates). > Acceptable while there is one engine; tracked for the Phase-5 build/boundary work.

### LEDs: `DisplayModel`

`src/engine/display_model.h` is a hardware-agnostic panel description (two 32-px `LEDRing` canvases + named indicators) the engine fills and the platform blits to the WS2812 chain. The engine never touches `Hardware`.

> **Status (transitional):** the granular engine currently reports LED *state* through query > methods (`*_leds`, `render_ring`, `mix`, `route`) and the platform interprets it (palette, > blink, `MValue` value-display overlays, storage/tape compositing). Item 3(a)'s > `render(DisplayModel&)` migration moves the steady-state fill into the engine while the > platform keeps its transient-overlay compositing - about 90% of the LED code is irreducible > platform interaction-grammar, so this is "engine fills the base layer, platform composites on > top," not a clean blit.

### Known residual coupling (closes during the remaining refactor items)

- ~~**Switch-config writes + deck-state readbacks** reach `Core` via `engine.core()`~~ **RESOLVED by item 3(a) (2026-06-02): `engine.core()` is deleted; `CoreUI`'s ctor takes `IEngine&`.** The former Categories 2-3 are now the `set_config`/`tempo_to_fit`/`toggle_grit_mode` config channel and the `DeckLayout`/`size_sets_tempo` knob-layout queries; seeding reads the engine's pre-seeded `param()`. The platform no longer touches `Core`.

- **`Driver`/transport** lives inside `Core` and is forwarded through `transport_*`. Conceptually platform; relocation to a platform transport service is deferred and will likely be forced by a second engine's `CapTransport`.

- **Build boundaries:** the blanket `-Isrc/` lets any layer include any other; Phase 5 splits into static libs with separate include roots to enforce the layering above.

## 4. Slotting in an engine

**"Filename or `#ifdef`?"** Neither alone, and not scattered `#if`s. Selection is a single **build-time** choice driven by one Make variable that does two things together:

1. **Build graph (filenames):** the variable selects *which engine source files compile and link*.

2. **Type binding (one localised `#if`):** the same variable emits one `-D` that a single header turns into a `using ActiveEngine = ...;` alias.

The platform is polymorphic over `IEngine` (virtual dispatch), but the *concrete type is fixed at compile time*: the engine is a static value member of `AppImpl` - no heap, no runtime factory. That is deliberate; a runtime `IEngine*` factory would buy nothing (one engine per image) and cost a heap allocation the real-time design forbids. There are **no** per-feature `#ifdef`s in the platform - `CoreUI`/`Storage`/`app` speak only `IEngine`.

### Today (item 3(a) complete; engine-select mechanism not yet built)

Engine choice is **hardcoded** in `app.cpp` (`GranularEngine _engine;`), but the coupling that blocked a clean swap is gone: `CoreUI`/`Storage` hold only `IEngine` (the `GranularEngine&` ctor and `engine.core()` were removed by item 3(a), 2026-06-02). To swap engines today you still edit `app.cpp`'s member declaration, but nothing else in the platform names the concrete engine. The remaining step to a one-line swap is the `ENGINE`-variable + `engine_select.h` mechanism below (item 3(b)). The current `Makefile` compiles `$(wildcard src/engine/*.cpp)` and relies on `--gc-sections` to strip the unselected engine from the binary.

### Target (after item 3(a) deletes `core()` and the `GranularEngine&` ctor)

Once `CoreUI`/`Storage` hold only `IEngine&`/`IEngine*`, the concrete engine type appears in exactly one place - `app.cpp`'s member declaration - and selection becomes a one-line build flag.

`Makefile`:

```make
ENGINE ?= granular
ifeq ($(ENGINE), granular)
  C_DEFS += -DSPK_ENGINE_GRANULAR
  ENGINE_SOURCES = $(wildcard src/engine/granular/*.cpp)   # adapter + DSP, all under granular/
else ifeq ($(ENGINE), passthrough)
  C_DEFS += -DSPK_ENGINE_PASSTHROUGH
  ENGINE_SOURCES = src/engine/passthrough_engine.cpp
endif
# replaces the current `$(wildcard src/engine/*.cpp)` so only the selected engine compiles
CPP_SOURCES += $(ENGINE_SOURCES) $(wildcard src/engine/shared/*.cpp)
```

`src/engine/engine_select.h` (new, the single `#if`):

```cpp
#pragma once
#if defined(SPK_ENGINE_GRANULAR)
  #include "engine/granular/granular_engine.h"
  namespace spotykach { using ActiveEngine = GranularEngine; }
#elif defined(SPK_ENGINE_PASSTHROUGH)
  #include "engine/passthrough_engine.h"
  namespace spotykach { using ActiveEngine = PassthroughEngine; }
#else
  #error "No engine selected: set ENGINE=<name> in the Makefile"
#endif
```

`app.cpp`:

```cpp
#include "engine/engine_select.h"   // not granular_engine.h
...
ActiveEngine _engine;               // the only place the concrete type is named
```

Build a variant with `make ENGINE=passthrough`; the default stays `granular`.

### Writing a new engine - checklist

1. Subclass `IEngine`; implement the three pure-virtual lifecycle methods. Own your DSP privately (do not reuse `src/engine/granular/` - that is granular-specific).

2. Return a `capabilities()` bitset for the regions you support; override only those methods.

3. Map `ParamId`s in `set_param`/`param` (own any mode fanout internally).

4. If `CapTapeStorage`: implement the `audio_*` byte-port so the platform tape can save/load your buffer. If `CapTransport`: implement `transport_*` (or consume the platform transport service once `Driver` relocates).

5. Fill `render(DisplayModel&)` for your panel; the platform blits and composites overlays.

6. Add the engine to `engine_select.h` and the `Makefile` `ENGINE` switch.

7. Verify on hardware (no host harness covers the pot/pad/LED/MIDI paths) and watch `SRAM_EXEC`.

## 5. Execution model

There is no RTOS. Concurrency comes from interrupts layered over a single main loop. `AppImpl` (`app.cpp`) owns the top-level subsystems and wires the callbacks in `AppImpl::Init`:

```text
DaisyTimeSource _time_source; // injected clock (ITimeSource) for the engine
GranularEngine  _engine;      // the active DSP engine, driven through IEngine
CoreUI          _ui;          // platform: controls, LEDs, pads, MIDI, storage UI
Hardware        _hw;          // Daisy peripherals and board I/O
Settings        _settings;    // calibration data in QSPI flash
Storage         _storage;     // SD-card samples + text config
```

`_engine` is the concrete engine member (today `GranularEngine`; section 4 covers selection); the platform drives it only through the `IEngine` interface. Four execution contexts run concurrently, ordered here from highest to lowest timing priority:

1. **Audio callback** - `AppImpl::ProcessAudio` (48 kHz / 96 frames, ~500 us per block). Reads analog controls, ticks the UI's per-block needs (`_ui.tick`, `_ui.read_cv`), then runs `_engine.process`. This is the hard real-time path: no allocation, no blocking, no SD or flash access.

2. **DAC callback** - `DACCallback`. Calls `_engine.process_cv(cv0, cv1, n)` **once per block** to fill the two modulation/LFO CV channels (block-rate by design, so the ISR pays no per-sample virtual dispatch), converts to the DAC's 12-bit range, and feeds the UI's LFO display the block's last sample.

3. **TIM5 timer ISR** - `T5Callback` at 250 Hz. Polls the gate inputs and renders LEDs (every fourth call, so LEDs refresh at ~62 Hz).

4. **Main loop** - `AppImpl::Loop`. Runs non-real-time work: `CoreUI::process` (pads, MIDI, switches, value display, gate out), `_engine.prepare`, `Storage::process` (the cooperative SD state machine), and the "boot button held 3s -> reset to bootloader" check.

The deliberate split is that **SD-card and flash I/O live only in the main loop**, never in the audio path, and large buffers are pre-allocated up front so the audio path never allocates. The main risk in this model is that the main loop and the audio callback share data (deck/UI state) without locks; correctness relies on the audio path only reading parameter snapshots and on writes being word-sized. See the review for the race analysis.

## 6. Subsystem map

```text
                         +-------------------+
            pads/pots --> |      CoreUI       | <-- MIDI in
            switches  --> | (src/ui) PLATFORM | --> MIDI out, gate out
                         +----+----------+----+
                              |          ^
                       IEngine|          | IEngine queries (LED/transport state)
                       calls  v          | (render/DisplayModel target)
                         +----+----------+----+
                         |   IEngine (seam)   |
                         |  GranularEngine    |  -----> mod CV out (process_cv, via DAC cb)
                         +---------+----------+
                                   | owns
        audio in  ----->  +--------+----------+  -----> audio out
                          |       Core        |
                          |    (src/engine/granular)     |
                          +--+-----+------+---++
                             |     |      |   |
              +--------------+     |      |   +-------------+
              v                    v      v                 v
        +-----------+        +---------+ +--------+   +-----------+
        |  Deck A   |        | Deck B  | | Driver |   |  Panner   |
        | (looper+  |        |         | | clock/ |   |  XFade    |
        |  granular)|        |         | | sync   |   |  Click    |
        +-----+-----+        +----+----+ +----+---+   +-----------+
              |                   |            |
              v                   v            v
        +-----------+      large buffers   tempo/ticks
        | SDRAMBuffer pool (src/hw/buffer.sdram) |
        +----------------------------------------+
              ^
              |  samples (load/save), config
        +-----+-----------------+
        | Storage / Card / FatFS|  (src/memory, src/hw/card)
        +-----------------------+
```

`CoreUI` is the only subsystem that talks to both `Hardware` and the engine, but it reaches the engine *only through `IEngine`* (with the residual `core()` exception noted in section 3). It translates hardware events into musical actions on the engine, reads engine state back to drive LEDs and gate/MIDI outputs, and drives `Storage` for the SD-card UI. The engine (and the `Core` it owns) does not know about hardware; `Core` is a self-contained DSP graph fed parameters and producing samples. `Storage` holds the engine through `IEngine*` and the engine through the `audio_*` byte-port, not a `Deck*`.

## 7. Core DSP graph (the granular engine's private DSP)

`Core` (`src/engine/granular/core.cpp`) is the DSP graph owned privately by `GranularEngine`; it is not a shared platform layer. It holds the two decks plus the shared `Driver`, `XFade` (the A/B crossfade mix), `Click`, `Panner`, and a per-deck `Modulator`. The per-sample signal flow in `Core::process` is, conceptually:

```text
in[L,R] --route--> Deck A.process_out --> panner --\
                                                    >-- XFade(mix) --> +click --> SoftLimit --> out[L,R]
in[L,R] --route--> Deck B.process_out --> panner --/
```

with a cross-record feedback path: each deck's input for recording can be the external input or the *other* deck's output, selected by `Deck::Source` (external/internal), which is what enables cross-deck recording and resampling. The `Route` enum (DoubleMono, Stereo, GenerativeStereo) selects the input topology and, via `infer_panner_mode`, the panner behaviour.

### Clock / transport

`Driver` (`src/engine/granular/driver.cpp`) is the transport. `SynClock` runs a 48-PPQN internal timeline that can phase-lock to an external 4-PPQN (TRS) or 24-PPQN (MIDI) source using fractional tick accumulation and a tempo-adjustment feedback loop. `Tempo` provides tap-tempo (averaged, with outlier rejection, clamped 20-250 BPM). `Divider` turns the PPQN stream into musical triggers (default 1/16, with swing/triplet options). The driver distributes ticks to both decks and to the modulators, fires "key" and "quarter" callbacks used by the UI for LEDs and clock-out, and converts incoming TRS clock to outgoing MIDI clock.

> The platform drives transport through the engine's `transport_*` forwards rather than > reaching `Driver` directly; `Driver` still physically lives in `Core` pending the transport > relocation noted in section 3.

### Deck

Each `Deck` (`src/engine/granular/deck.cpp`) is a complete looper voice:

- `Buffer` - the loop buffer with a record/overdub state machine (idle/fadein/sustain/ fadeout, 4 ms Hann fades) and feedback-controlled overdub. Reads use linear or cubic interpolation for pitch-shifted playback.

- `Generator` + `Vox` - the granular engine. The generator owns start/size/spread/ pitch/speed, slicing (manual and automatic), and reverse, and runs up to `kVoxCount` (3) voices, each managing several overlapping windowed grains. Mode (Reel/Slice/Drift) selects linear-vs-spread grain behaviour and tape-vs-digital speed semantics.

- `Track` - a per-deck step sequencer recording `Event`s (note plus parameter locks) onto a fixed grid; patterns persist across mode changes.

- `Dispatcher` - polyphonic voice allocation between the sequencer/clock events and the granular voices, with a "hold" voice for monophonic modes.

- `Fx` - the per-deck Grit (Drive = overdrive, or Reduce = decimator/bitcrush) and Flux (echo delay) effects, each soft-switched on/off.

- `Detector` - onset detection used to auto-start recording at the -40 dB threshold.

### Modulation

Per deck, a `Modulator` wraps an `LFO` (sine/square/saw/sample-and-hold) and a `Follower` (envelope follower on the deck output). It can free-run (0.01-12 Hz) or sync to musical divisions (1/32 to 4 bars). Its output drives both an internal modulation target (loop start/size, pitch, mix) and the physical mod CV output via the DAC callback.

## 8. Memory model

All large buffers live in external SDRAM and are handed out once at init by the `SDRAMBuffer::pool()` singleton (`src/hw/buffer.sdram.cpp`):

- Per-deck source (loop) buffers sized for `kSourceMaxSeconds` (42 s) of stereo audio, plus a third source buffer.

- Per-deck/per-channel detector and delay (echo) buffers.

- Per-deck slice-point arrays and `Track` event buffers.

- A 32 KB staging buffer for SD-card chunked I/O.

The platform pulls these pointers from the pool, packs them into the `EngineBuffers` of an `EngineContext` (section 3), and passes that to `engine.init`; the engine then routes them into each deck through a `Deck::Params` struct. Decks and effects never allocate their own large storage. Smaller state lives in the objects themselves (statically allocated). Calibration data persists in QSPI flash via `Settings` and libDaisy's `PersistentStorage`; user config (MIDI channels, preload) persists as `SK/config.txt` on the SD card and is parsed by `Config`.

The application boots from SRAM; code lives in the `SRAM_EXEC` region, which is the binding memory constraint - any added abstraction surface must watch it. It was 186 KB until commit `993210f` moved 114 KB across from the data `SRAM` region (300 KB / 212 KB today) so the `TERMINAL=1` channel would fit on every engine; `pstretch`, whose FFT working set needs the data side instead, gets its own 200 KB / 312 KB split via [`linker/alt_sram_pstretch.lds`](../linker/alt_sram_pstretch.lds). Each build prints the live figure (`--print-memory-usage`).

## 9. Control and persistence flow

- **Pots/sliders**: read in the audio callback, debounced/smoothed by libDaisy `PotMonitor`, enqueued onto a `UiEventQueue`, drained in the main loop, and applied to the engine through `MValue` wrappers (pickup/catch behaviour to avoid parameter jumps) that call `engine.set_param(ParamId, deck, value)`. The platform owns the pickup mechanics; the engine owns what each `ParamId` does.

- **Pads**: MPR121 touch polled in the main loop; touch/release dispatched through callbacks into `CoreUI`, which encodes the short/long/hold and Alt-combination gestures and routes them to `engine.on_*_pad`/`set_fx`/`on_seq_*`.

- **CV**: read per audio block; the platform reads + calibrates each jack and calls `engine.cv_*`; V/oct is mapped to speed via an engine-owned `SpeedMap`.

- **Storage**: SD operations are a cooperative state machine driven from the main loop; `init_*` operations and config/preload reads are blocking but bounded and only happen outside the audio path. The most-recent sample per deck can auto-preload on boot. `Storage` saves/loads the engine's loop buffer through the `audio_*` byte-port on `IEngine`.

## 10. Conventions

- Most non-trivial classes are non-copyable via the `NOCOPY` macro (`src/nocopy.h`).

- Several services are singletons: `SDRAMBuffer::pool`, `Config::dynamic`, `Expose::values` (debug value printing), `Meter::cpu` (CPU load metering).

- `infrasonic` namespace (`common.h`) holds shared math/log helpers and the `INFS_*` macros; logging compiles out unless built with `DEBUG=1`.

- Dotted filenames denote variants of a concept: `fx.drive.cpp`, `fx.reduce.cpp`, `core.ui.midi.cpp`, `random.lfo.h`.
