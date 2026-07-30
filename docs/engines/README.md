# Engines

Spotykach is a fixed hardware/UI **platform** hosting a swappable DSP **engine** (one chosen at build time via `ENGINE=`). This folder documents each shipped engine in detail; this page covers the model they all share. There are three **ways to build** an engine (native C++, Faust/cyfaust, gen~/gen-dsp) — see [`../engine-types/`](../engine-types/README.md). Companion docs: `docs/architecture.md` (platform/engine design), `docs/engine-layout.md` (which files belong to which engine).

**Doc split convention.** Each engine's documentation is split by audience:

- `docs/engines/<name>.md` — **user-facing**: what the engine is and its signature behavior, the control map (knobs/pads/CV/switches), routing/display, SD-card prep + file formats, and the build/flash commands.

- `docs/dev/<name>-impl.md` — **developer-facing**: architecture and internal data flow, the file-by-file map, implementation constants and risks/watch-items, build-system internals, bring-up status, bug writeups, and the roadmap / "what's left". Each engine doc links to its impl notes near the top. (`passthrough` is a minimal reference example and is not split.)

## The contract an engine implements

Every engine implements `IEngine` (`src/engine/iengine.h`). The audio lifecycle is required:

- `init(const EngineContext&)` — receive sample rate, block size, the SDRAM `arena`, the `ITimeSource` clock, and the platform `transport` (read-only `ITransport`). Allocate + subscribe here.

- `prepare()` — non-real-time main-loop housekeeping.

- `process(in, out, size)` — the 48 kHz / 96-frame audio block.

Everything else (params, MIDI, pads, CV/gate, storage, LED queries, `render`) has a no-op default; an engine overrides only what it supports and advertises optional regions via `capabilities()` (`CapOwnDisplay`, `CapDualDeck`, `CapTransport`, `CapTapeStorage`, ...).

## The transport (shared clock)

The clock/transport is a **platform service** (`src/transport/`, see `docs/engine-layout.md`), not part of any engine. The platform owns it and drives it; an engine receives a read-only `ITransport` (`src/engine/itransport.h`) in `init()` and can:

- **read tempo/state**: `tempo()`, `source()`, `is_external_sync()`, `key_interval()`, `is_key_sub_quarter()`.

- **subscribe to ticks**: `set_on_tick(cb)` — the platform calls `cb(const TransportTick&)` every clock tick (from the audio-block context).

`TransportTick` carries `{index, tick, key, quarter, tempo, reset}`:

| field | meaning |
|---|---|
| `index` | monotonic counter of common (divided) ticks since boot — use for step placement |
| `tick` | a common/divided tick fired this step (1/16 at the default divider resolution) |
| `key` | a key (bar) boundary, per the key-interval |
| `quarter` | a quarter-note boundary |
| `tempo` | current BPM |
| `reset` | the grid was realigned (clock reset / external resync) — realign your own sequencer |

The delay engine *reads* tempo; the edrums engine *subscribes* and steps a sequencer. A non-synced engine ignores the transport entirely.

### Clock control (player-facing)

- **TAP** button — tap tempo (internal clock).

- **Tap-hold TAP + MODFREQ_A** — set tempo continuously.

- **Alt + TAP** — cycle clock source: internal → TS4 clock-in (jack 23) → MIDI.

- The clock-source LED (green = internal, pink = external) and the speed blink under jack 23 render for every engine.

## Knobs: how a physical control reaches an engine

The platform reads the 7 analog knobs per deck plus the crossfade, applies its pickup/modifier grammar, and calls `engine.set_param(ParamId, deck, value)` (or `set_mod_speed`). The **same physical knob maps to different `ParamId`s depending on the held modifier** (the Flux/Grit/Alt pads, the tap-hold). For a non-granular engine (default `DeckLayout::single`) the per-deck routing is:

| Physical knob | default | Flux-pad held | Grit-pad held | Alt held | Tap-hold |
|---|---|---|---|---|---|
| POS   | `Pos`   | `FluxFb` | — | — | — |
| ENV   | `Env`   | — | — | — | — |
| SIZE  | `Size`  | — | — | — | — |
| PITCH | `Speed` | `FluxIntensity` | `GritIntensity` | `Aux` (CapAux engines) / else pitch-quantize | — |
| MODFREQ | `set_mod_speed` | — | — | (sync flag) | **Tempo** (A) / PanSpeed (B) → platform |
| MOD_AMT | `ModAmp` | — | — | — | ClickMix (A) / PanRange (B) → platform |
| SOS   | `Mix`   | `FluxMix` | `GritMix` | `Feedback` | **KeyInterval** (A) → platform |

The `ParamId` names are granular-flavored (the platform's vocabulary), but a non-granular engine just interprets whichever ones it cares about — e.g. the delay reads `Size/Pos/Speed/Mix` as time/feedback/pitch/mix; edrums reads them as Euclidean + voice controls. The **default column** gives each engine 7 per-deck controls with no modifier needed.

`Tempo` and `KeyInterval` are not engine params — the platform writes them straight to the transport.

## Building and flashing a variant

```text
make -j8 ENGINE=granular      # default; also: delay | edrums | reso | graincloud | tape | shuttle | reverb | gigaverb | radio | bard | glitch | pstretch | chorus | filter | voice | passthrough
make ENGINE=edrums program-dfu
make engine-edrums            # one-shot: clean + build + flash (device in DFU mode)
make check-boundary           # platform (hw/ui/memory/transport) must not include engine/granular/
```

## The engines

The **Built via** column links to the development method ([`../engine-types/`](../engine-types/README.md)).

| Engine | `ENGINE=` | What it is | Built via | Doc |
|---|---|---|---|---|
| Granular looper | `granular` (default) | the full instrument: dual granular looper/sampler | [native C++](../engine-types/cpp.md) | [granular.md](granular.md) |
| Stereo delay | `delay` | tempo-synced delay; Clean/Tape/Shimmer characters (mode switch) + Stereo/DoubleMono/Ping-pong topologies (route switch); tone, modulation LFO, Play-pad freeze | [native C++](../engine-types/cpp.md) | [delay.md](delay.md) |
| Edrums | `edrums` | dual Euclidean drum machine (synthesized) | [native C++](../engine-types/cpp.md) | [edrums.md](edrums.md) |
| Reso | `reso` | dual resonator / pluck voice (Mutable Instruments Rings DSP) | [native C++](../engine-types/cpp.md) | [reso.md](reso.md) |
| Grain cloud | `graincloud` | polyphonic grain cloud (per-grain pitch/pan/position) - the **granular engine with its grain DSP swapped for a GrainflowLib cloud** (`SPK_GRAIN_GF`); inherits granular's record/storage/UI | [native C++](../engine-types/cpp.md) | [graincloud.md](graincloud.md) |
| Tape | `tape` | dual streaming SD record/playback decks (no in-memory length cap) + tape FX (wow/flutter, hysteresis) | [native C++](../engine-types/cpp.md) + [Faust](../engine-types/faust.md) | [tape.md](tape.md) |
| Shuttle | `shuttle` | buffer-based bipolar/reverse varispeed tape (four in-RAM tracks; capstan-speed PITCH, per-track loop window) | [native C++](../engine-types/cpp.md) | [shuttle.md](shuttle.md) |
| Reverb | `reverb` | route-aware stereo reverb, three all-Faust algorithms (Dattorro plate / Zita hall / Greyhole), Reel/Slice/Drift switch selects; DoubleMono = two mono plates (heavy hall/greyhole are stereo-only) | [Faust](../engine-types/faust.md) | [reverb.md](reverb.md) |
| gigaverb | `gigaverb` | stereo reverb from a Max gen~ patch | [gen~](../engine-types/gen.md) | [gigaverb.md](gigaverb.md) |
| Radio | `radio` | dual virtual RadioMusic: two SD-streaming "radios" with a free-running virtual playhead; `.raw`/`.wav` stations in numbered banks | [native C++](../engine-types/cpp.md) | [radio.md](radio.md) |
| Bard | `bard` | the **storyteller**: dual bookmark-navigated audiobook decks. Books stream from numbered SD shelves; a plain-text sidecar next to each book lists timestamps whose **line order is the recitation order**, so a scrambled list is a re-ordering of the book. Owned/resumable playhead (the inverse of `radio`'s free-running one), play/pause, jump-back, rate 0.5-2.5x with continuous **pitch-keep** (WSOLA), voice colour + room, and Read / Recite / Wander sequence modes that can be armed to the transport | [native C++](../engine-types/cpp.md) | [bard.md](bard.md) |
| Glitch | `glitch` | dual lo-fi / circuit-bent noise voice: 12 curated [Noisferatu](https://github.com/rob-scape/noisferatu) algorithms per deck (Alt+PITCH = algorithm select) - buffer/bit-mangle glitch, logic-noise, generative scale blips, rhythmic noise | [native C++](../engine-types/cpp.md) | [glitch.md](glitch.md) |
| Pstretch | `pstretch` | real-time, clean-room **PaulStretch** ambient time-smear: FFT phase-randomized wash per deck; live smear, Freeze drone, and capture-and-stretch-through (Rev pad). Self-contained vendored FFT | [native C++](../engine-types/cpp.md) | [pstretch.md](pstretch.md) |
| Chorus | `chorus` | stereo chorus - the demo of the **generated** Faust path (`.dsp` + JSON manifest, no hand-written C++) | [Faust (generated)](../engine-types/faust.md#generated-engines-no-hand-written-c) | [chorus.md](chorus.md) |
| Dual filter | `filter` | two independent resonant low-pass voices, one per channel - the **generated parallel (DoubleMono) dual-deck** demo | [Faust (generated)](../engine-types/faust.md#generated-engines-no-hand-written-c) | [filter.md](filter.md) |
| Voice | `voice` | drone oscillator (deck A) into a resonant filter (deck B) - the **generated series (chain) dual-deck** demo | [Faust (generated)](../engine-types/faust.md#generated-engines-no-hand-written-c) | [voice.md](voice.md) |
| Passthrough | `passthrough` | minimal stereo passthrough (reference engine) | [native C++](../engine-types/cpp.md) | [passthrough.md](passthrough.md) |
| Csound | `csound` | a full Csound 7 instance — the patch (`.csd`) defines the sound; SD patch bank + live switching + MIDI. A **QSPI** build (built via `make engine-csound`, not `ENGINE=`); needs `scripts/fetch_csound.sh` once | [Csound](https://csound.com) | [csound.md](csound.md) |
| ChucK | `chuck` | the ChucK language + VM — the patch (`.ck`) defines the sound; strongly-timed concurrent shreds; SD patch bank + live switching (compile-once cache, memory-stable). A **QSPI** build (built via `make engine-chuck`, not `ENGINE=`); needs `scripts/fetch_chuck.sh` once. MIDI planned | [ChucK](https://chuck.stanford.edu) | [chuck.md](chuck.md) |
| Mosc | `mosc` | dual **macro-oscillator** — a full 24-engine Mutable Instruments **Plaits** voice per deck (Alt+PITCH = engine select); per-deck Gate/Drone Mode, Stereo / DoubleMono / GenerativeStereo routing. A **QSPI** build (built via `make engine-mosc`, not `ENGINE=`) but the DSP is vendored in-tree — no fetch needed | [native C++](../engine-types/cpp.md) | [mosc.md](mosc.md) |
