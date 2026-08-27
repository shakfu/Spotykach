# Dev resume notes — SD-streaming `tape` engine (dual-deck record decks)

> Resume point for the streaming **tape** engine - the in-SDRAM length cap is removed and the engine is > hardware-verified as a **dual-deck** machine: two independent mono decks, routing switch + mix fader + > per-deck pan/volume, and four ENV loop modes. (`reso` shipped separately - see `docs/engines/reso.md` > and `CHANGELOG.md` `[0.2.2]`.)

The user-facing reference (controls, SD-card layout, file formats, build commands) is [`docs/engines/tape.md`](../engines/tape.md); this file holds the internals, the file map, the risks, and the bug writeups.

## TL;DR — where we are

A `tape` engine that streams audio to/from the SD card so playback and recording are no longer capped by the ~42 s (float) / 84 s (int16) in-SDRAM loop buffer (`kSourceMaxSeconds`). It is **linear** (no random seek - can't meet the 2 ms deadline). The original single-stream gate cleared on hardware; it then grew into a **dual-deck** instrument:

- **Two independent mono decks** (A/B), each play-XOR-record, its own ring + 8 slot files under `/tapes/`. Deck A records input A and plays to L, deck B records input B and plays to R.

- **Routing switch** (`ConfigId::Route`): LEFT = per-deck Alt+POS pan, CENTRE = both centered, RIGHT = random pan. **Mix fader** (`Crossfade`) = A/B blend. **MIX** knob = per-deck volume. **PITCH** = per-deck varispeed. **Alt+POS** (`AltPos`) = pan; **Alt+PITCH** (`Aux`) = tape-slot select; **bare POS** reserved.

- **ENV knob** = per-deck loop mode (4 quadrants): none / plain / faded (seam fade) / Frippertronics (per-pass decay, auto-stops).

- **Tape slots:** 8 per deck under `/tapes/`, selected by Alt+PITCH (non-destructive multi-take).

- **Tape FX:** per-deck wow/flutter + Jiles-Atherton hysteresis/saturation + resonant low-pass (Faust kernel via cyfaust). POS = drive, SIZE = character, MOD_AMT/MODFREQ = wow/flutter depth/rate, grit+PITCH = filter cutoff, grit+MIX = resonance.

Dual-deck + loops are hardware-verified ("working so far"). The latest control-map additions (Alt+POS pan, Alt+PITCH slots, `/tapes/` files, and the post-FX resonant low-pass on the grit-modifier pad) build clean + host-green, pending re-flash. SRAM_EXEC ~86.9% (incl. tape FX).

## Status at a glance

- **Hardware-verified (core):** two decks record and play independently (A->L, B->R), varispeed, routing/pan/mix, and the loop modes all work on the device. The original in-SDRAM length cap is gone.

- Streaming core (rings + WAV codec + looping) **host-tested**: `make -C host test` includes `test-stream` (ISR -> ring -> WAV file -> ring -> ISR, byte-exact, plus the loop-rewind path) - all green.

- Firmware links and fits: `make -j8 ENGINE=tape` -> ~86.9% SRAM_EXEC (incl. tape FX), no overflow. Other engines build unchanged.

- SD throughput held in testing; mono-per-deck keeps two simultaneous streams at roughly the bandwidth of one old stereo stream. Sustained both-decks-at-2x is the remaining throughput unknown (if it underruns, int16 streaming jumps up the list).

- The most recent additions - **pan moved to Alt+POS**, **tape-slot select on Alt+PITCH**, per-deck **`/tapes/` slot files**, and a **post-FX resonant low-pass on the grit-modifier pad** (grit+PITCH = cutoff, grit+MIX = resonance) - build clean and pass the host tests, but have not yet been re-flashed.

## Plan + status

1. **Ring + stream core (host-tested)** — [done]. Lock-free SPSC ring + `PlayStream`/`RecordStream` (+ looping: `PlayStream::set_loop` rewinds the source at EOF).

2. **Streaming WAV codec (host-tested)** — [done]. Placeholder header -> patch-on-finalize; mono-capable (`wav_header(size, channels)`); `WavStreamReader::rewind` for loops.

3. **Device bring-up (single stream)** — [done, hardware-verified]. Cleared after the SD-unmount fix (see "Resolved during hardware bring-up").

4. **Dual-deck + routing + loops** — [done, hardware-verified]. Per-deck `StreamDeck`, per-deck `IStreamDeck`, mono I/O (input A->deck A, input B->deck B), routing switch / pan / mix-fader / MIX volume, ENV loop modes.

5. **Polish backlog** — [ ] see "What's left".

## Architecture

Producer/consumer split, **per deck**, mirroring the existing `Storage` service:

- **Audio ISR** (`TapeEngine::process()`) only ever touches the bounded/lock-free per-deck SDRAM rings (`play_consume(deck)` / `record_produce(deck)`); it never blocks and never calls FatFs.

- **Main loop** (`AppImpl::Loop` → `StreamDeck::process()`) does the slow FatFs chunked I/O for both decks sequentially - filling each playing deck's ring ahead of the ISR and draining each recording deck's ring to disk.

- The engine reaches the service through a per-deck `IStreamDeck*` injected via `EngineContext`.

`StreamDeck` holds **two independent `Deck` units** (`std::atomic<Mode> mode` + ring + `PlayStream`/`RecordStream` + `WavStreamReader`/`WavStreamWriter` + `FatFile`/file handle). `process()` pumps both. The `Mem` struct carries one ring per deck (`ring_a`/`ring_b`) + a shared scratch. The `IStreamDeck` contract is per-deck (`play_consume(deck, ...)`, `start_play(deck, path)`, `set_loop(deck, ...)`, ...); it encodes the threading split: the `*_consume`/`*_produce` calls are ISR-only and never block; `start_*`/`stop`/`set_loop` are main-loop-only; the state reads are safe from either side.

Data flow (per deck):

- **Play:** SD file → `WavStreamReader` → `PlayStream.pump()` fills the ring → ISR `play_consume` drains it (zero-fill on underrun). Looping: at EOF `PlayStream` calls `source->rewind()` and keeps filling.

- **Record:** ISR `record_produce` fills the ring → `RecordStream.pump()` → `WavStreamWriter` (mono) → on stop, flush tail + `finalize()` patches the size fields.

- **Audio (engine):** each deck renders a mono stream (varispeed playback / record-monitor / silence), then a per-block gain matrix (MIX volume × mix-fader blend × pan, selected by the routing switch) mixes both decks to the stereo bus `out[0]`=L / `out[1]`=R. Pan/blend gains are precomputed on knob change.

- **Loop shaping (engine):** for Faded/Frippertronics, the engine reads `loop_frames(deck)`, tracks source-frame position, and applies a seam fade or per-pass decay. A faded-out Frippertronics deck is flagged from the ISR and `stop()`-ped in `prepare()` (main loop), never in the audio path.

SDRAM: **one 1 MB ring per deck** (static `DSY_SDRAM_BSS` in `buffer.sdram.cpp`, tape-only; each serves that deck's play OR record; ~5.5 s mono read-ahead; power-of-two as `SpscRing` requires) + a 32 KB chunk scratch shared by both decks (sequential pumps). No increase over the original two-ring layout. Format is mono float WAV (one channel per deck) → no sample conversion on the audio path.

## Files

New (core, host-tested, engine-agnostic):

- `src/memory/spsc_ring.h` — lock-free SPSC byte ring over an external buffer (kfifo-style, wrap-safe).

- `src/memory/audio_stream.h` — `IChunkSource` (+ `rewind`) / `IChunkSink`, `PlayStream` (+ looping), `RecordStream`.

- `src/memory/byte_file.h` — `IByteFile` seam (read / write / seek).

- `src/memory/wav_stream.h` — `WavStreamReader` (parse + `rewind`/`data_bytes`) / `WavStreamWriter` (mono) over `IByteFile`.

- `host/test_stream.cpp` — end-to-end suite incl. the loop-rewind test (`make -C host test`).

New (device):

- `src/engine/istreamdeck.h` — the per-deck `IStreamDeck` contract (+ `set_loop`/`loop_frames`).

- `src/hw/fat_file.{h,cpp}` — FatFs-backed `IByteFile` (body guarded `#if defined(SPK_ENGINE_TAPE)`).

- `src/hw/stream_deck.{h,cpp}` — the dual-deck `StreamDeck` platform service (guarded body).

- `src/engine/tape/tape_engine.{h,cpp}` — the engine (slim `IEngine` header + impl): dual decks, varispeed, routing/pan/mix/volume, loop modes, LED feedback, and the per-deck tape FX (`TapeFx`).

- `src/engine/tape/tapefx.dsp` + `faust_kernel_tapefx.h` — the tape-FX Faust source and its **generated** kernel (wow/flutter + J-A hysteresis). Regenerate with `make faust-kernels`.

- `src/engine/faust_arch.h` — the shared MIT Faust arch shim (`dsp`/`UI`/`Meta` base types), used by the tape and reverb kernels. Lifted here from `src/engine/reverb/` when a second engine started using Faust.

- `host/test_tape.cpp` — host test for the tape FX (param round-trip incl. filter cutoff/reso, saturation wired/bounded, wow/flutter modulates, low-pass attenuates a high tone + resonance boosts near the corner, playback gate), wired into `make -C host test`. Each A/B comparison runs its engine in its **own arena** (a shared `ctx` restarts the bump allocator at the same base, aliasing kernels).

Edited:

- `src/engine/engine_context.h` — adds `IStreamDeck* stream = nullptr;`.

- `src/hw/buffer.sdram.{h,cpp}` — `streamMem()` + the per-deck SDRAM rings (`ring_a`/`ring_b`, guarded).

- `src/memory/wav.h` — `wav_header(size, channels=2)` (mono headers for the per-deck files).

- `src/memory/storage.h` — under `#if SPK_ENGINE_TAPE`, keep the SD card mounted (skip `_can_unmount()`'s unmount) so the tape stream can open files all session.

- `src/hw/fat_file.cpp` — `open_write` creates the parent dir (`/tapes`) before the file.

- `src/engine/engine_params.h` — adds `ParamId::AltPos` + `CapAltPos` (the Alt+POS knob layer, capability-gated).

- `src/ui/core.ui.{h,cpp}` — routes Alt+POS → `AltPos` for `CapAltPos` engines (mirrors the Alt+PITCH->`Aux` gate); non-claiming engines keep POS->`Pos` byte-identically.

- `src/engine/granular/granular_engine.cpp` — `AltPos` no-op case (keeps the param switch `-Wswitch`-clean).

- `src/app.cpp` — construct / init / inject `StreamDeck`, pump it in `Loop` (all `#if SPK_ENGINE_TAPE`).

- `src/engine/engine_select.h`, `Makefile`, `CMakeLists.txt`, `Makefile.cmake` — register `tape`.

## Control / UX

- **Play pad** (per deck) = play toggle; **Alt+Play** = record toggle (`on_play_pad`/`on_record_pad`, main-loop-safe). The **Rev pad is inert** (reserved for reverse playback). Play XOR record per deck, 300 ms same-deck debounce.

- **PITCH** (`Speed`) = per-deck varispeed (`exp2((v-0.5)*2)`, 0.5×…2×, ±1 octave). **Alt+PITCH** (`Aux`, `CapAux`) = tape-slot select (visual selector while held). **Alt+POS** (`AltPos`, `CapAltPos`) = per-deck equal-power pan (LEFT routing); **bare POS is reserved** for a future loop-start (ignored now). **MIX** (`Mix`) = per-deck volume. **ENV** (`Env`) = loop mode (4 quadrants). **Mix fader** (`Crossfade`) = A/B blend. **Routing switch** (`Route`) = pan topology. The tape-FX knobs: **POS** (`Pos`) = drive, **SIZE** (`Size`) = character, **MOD_AMT** (`ModAmp`) = wow/flutter depth, **MODFREQ** (`ModSpeed`) = wow/flutter rate, **grit+PITCH** (`GritIntensity`) = filter cutoff, **grit+MIX** (`GritMix`) = filter resonance.

- I/O: two MONO inputs (A normalled to B); deck A records input A, deck B records input B; stereo bus → headphone + the individual jacks.

- Display: per deck, idle ring **off**, playing **green**, recording **red**, rejected start **amber** (`0xff6000`) ~1.2 s; status also on the **Play-pad LED** (the pad you press). Routing position on the **mode L/C/R** indicator. While Alt held, the ring shows the **8-slot selector** (dots, selected bright). The live input is **monitored** to the deck's channel while recording.

- `capabilities() = CapOwnDisplay | CapDualDeck | CapAux | CapAltPos`. NOT `CapTapeStorage` (the tape engine owns its own SD streaming, so the platform `Storage` service stays out of the way).

**Tape slots:** 8 per deck, `/tapes/tape_a_<n>.wav` / `tape_b_<n>.wav`. Alt+PITCH (`Aux`) picks the active slot; the next Play/record targets it (no interrupt of a playing deck). The selector shows **recorded vs empty** slots (selected bright / recorded mid / empty dim), probed via `IStreamDeck::exists` (`f_stat`) in `prepare()` on selector-open, and marks a slot used the moment its record starts. `FatFile::open_write` creates `/tapes` on first record. Single-digit slots keep the names 8.3-safe.

**Alt-layer remaps are capability-gated, so they never degrade other engines.** Alt+POS→`AltPos` fires only for `CapAltPos` engines; everyone else hits the exact original `Pos` path (`process(val,!fx,…)` + `set_param(Pos,…)`) byte-for-byte - including pickup and the value display. Same model as `CapAux` for Alt+PITCH. (An earlier cut gated `Pos` globally and silently froze granular's Alt+POS; the capability gate fixes that - non-tape firmware is unaffected.)

Loop modes (ENV): None / Plain / Faded / Fripp. Looping is in the stream (`set_loop` + `rewind`); the fade/decay is engine-side via `loop_frames` + source-frame position tracking. Tunable constants: `kFadeFrames` (~50 ms seam fade), `kFrippDecay` (0.6/pass), `kFrippFloor` (0.02 → auto-stop).

## Tape FX — implementation (wow/flutter + hysteresis + low-pass)

Each deck has its own analog-tape effect chain, applied to the **playback** signal only (not the record monitor, whose wow/flutter delay would add monitoring latency). The DSP is a **Faust kernel generated by cyfaust** (`src/engine/tape/tapefx.dsp` -> `faust_kernel_tapefx.h`, namespace `tfx_tapefx`), the same toolchain the reverb engine uses; one mono kernel per deck is placement-new'd into the SDRAM arena at `init()`.

Chain: **wow/flutter -> Jiles-Atherton hysteresis/saturation -> resonant low-pass.** The summed two-deck output bus is then **soft-limited** (`daisysp::SoftLimit`, the cubic soft-clip the edrums/granular buses use) so the filter's resonant peak (Q up to ~10) and two simultaneous decks can't clip the codec - ~transparent below unity, gently taming peaks above it.

- **Wow & flutter** - a modulated fractional delay (`de.fdelay`) wobbles playback pitch. Periodic, not random (reel rotation): a slow wow LFO plus a flutter LFO built from f + 2f + 3f harmonics (after ChowTape's `FlutterProcess`). The LFOs use the table-free recursive oscillator `os.oscrs` - `os.osc` would emit a 64K-entry static sine table (256 KB) that lands in SRAM and overflows the region. MOD_AMT sets depth, MODFREQ sets rate.

- **Hysteresis / saturation** - `hy.ja_processor` from Faust's `hysteresis.lib`: the **Jiles-Atherton** magnetic model, the same one in Chowdhury's ChowTapeModel but self-contained (fixed 4 substeps + cubic-Hermite interpolation = built-in antialiasing, no extra oversampling). In this model the saturation *is* the hysteresis: POS = drive (amount), SIZE = character (the reversibility `c`, 0.25 open/dynamic .. 0.9 compressed). The processor gain-compensates drive and DC-blocks internally, and clamps magnetization to [-1,1] so it stays bounded when driven hard.

- **Resonant low-pass** - a 2-pole `fi.resonlp` after the saturation: a dub/tape tone control on the played-back signal. Cutoff is exponential (`40 * 500^cutoff` -> ~40 Hz .. 20 kHz, even sweep across the knob) and resonance is squared (`Q = 0.7 + reso^2 * 9.3` -> flat Butterworth .. ~10, gentle for the low half of the knob, a strong peak at the top). **Cutoff and resonance ride the grit-modifier pad, not dedicated knobs:** hold **grit + PITCH** = cutoff, hold **grit + MIX** = resonance, reusing the platform's existing grit knob-edit gesture (the same `GritIntensity`/`GritMix` params the granular engine's grit uses - the tape engine just reinterprets them, so no platform/CoreUI change). Cutoff **boots open (1.0)** so the filter is inert until swept down; its pickup is seeded open, so you turn grit+PITCH *down* to engage it.

**Licensing.** `hysteresis.lib` is LGPLv2.1 **with the Faust library exception** ("you may ... distribute the compiled code ... under your own copyright and license"), so the cyfaust-generated C++ ships under the project's MIT - the same basis as the reverb kernels. (C) 2025 Thomas Mandolini. **ChowTapeModel itself (`thirdparty/AnalogTapeModel`) is GPL-3 and is used only as an algorithm/parameter reference - never linked.**

**Voicing.** The J-A is intentionally subtle (the lib's -50 dB calibration voices it for musical tape coloration, not fuzz); louder loops and higher drive push it further. The voicing levers, to dial by ear on hardware: the drive range (`drive * 54` dB in the `.dsp`), the `Ms/a/alpha/k` ferromagnetic params, and the wow/flutter rate/depth scaling.

**CPU.** Not yet measured on hardware (`Meter::cpu`). The J-A runs 4 substeps/sample (~1 `tanh` + ~4 divisions each) x 2 decks; estimated ~10-25% of the 480 MHz budget. If too hot: swap `ma.tanh` for a polynomial Langevin approx, or fall back to an ADAA-tanh saturator. Measure before relying on it.

## Resolved during hardware bring-up

**1. SD card unmounted out from under the stream (the single-stream blocker).** Record flashed the ring briefly then went dark and **no file was created** - `f_open` was failing, not the audio path. `Storage::process()` unmounts via `_can_unmount()` once settings are read and both decks are idle (`f_mount(NULL) + _fsi.DeInit()`); the tape `FatFile` then opened on a dead volume → `FR_NOT_READY` → `start_record` false → no file. Fix: under `#if SPK_ENGINE_TAPE`, skip that unmount so the volume stays mounted all session (`src/memory/storage.h`).

**2. LED feedback unreadable / failures silent.** Idle is fully **off**, active full-bright (was dim-vs-bright same-hue, indistinguishable on the LEDs); a rejected `start_*` flashes **amber** ~1.2 s (via the injected `ITimeSource`). The dual-deck rework also **moved control onto the Play pads** (which have LEDs), retiring the earlier Seq-pad wart - the pad you press is now the pad that lights.

**3. Input was being summed to mono (dual-deck fix).** The first dual-deck cut recorded `0.5*(in[0]+in[1])`. Per the hardware manual the inputs are two INDEPENDENT mono jacks - deck A records input A, deck B records input B. Corrected.

## Risks / watch-items (steady-state verified; sustained both-decks-at-2x unproven)

- **Dropouts / underruns.** Mono-per-deck keeps two simultaneous streams near one old stereo stream's bandwidth (~192 KB/s each, vs ~1.5 MB/s on the `MEDIUM_SLOW` 1-bit SDMMC; ~5.5 s mono ring). If they occur: bump SD speed / bus width (`card.cpp` `recognize()`) and/or deepen the rings.

- **SD mount lifecycle** — RESOLVED (above): the tape build keeps the card mounted. The per-deck files are at the volume root. `FatFile` and `Storage`'s `Card` share one FatFs volume but never do I/O concurrently (Storage stays idle in the tape build; the tape engine owns all streaming I/O).

- **Format** = float WAV (2× int16 bandwidth). int16 streaming is the main future throughput lever.

## What's left for the implementation

### Hardening (known gaps)

- **SD-full / partial `f_write`** — `RecordStream::pump()` pulls bytes out of the ring before `sink->write()`; a short write (disk full) loses them. Don't advance the ring until written, or stop cleanly on SD-full.

- **Error feedback** — done (coarse): `start_play` / `start_record` failures flash the ring amber ~1.2 s. Still one colour for all causes (missing file / not mounted / disk full); per-reason feedback would need `StreamDeck` to report a reason code.

- **Mount-readiness** — partly: the card stays mounted for the session, so steady-state works. A press in the first ~1 s after boot (before the mount completes) still fails, but now flashes amber rather than dying silently.

- **Format validation** — `WavStreamReader` parses any WAV header, but the engine reinterprets the body as native mono float 48k; a differently-formatted file plays as garbage. Guard before allowing arbitrary-file playback. (Self-recorded files are internally consistent.)

- **Concurrency review** — play-finished + finalize + loop-rewind observed working on device; the remaining edge cases of the ISR <-> main-loop handshakes (stop during the `_finalizing` flush window, the Frippertronics ISR->prepare stop flag, underrun at a loop seam) are reasoned-through but not yet stress-tested.

### Feature backlog

- **More slots / a browser** — 8 slots per deck exist now (`/tapes/tape_<d>_<n>.wav`, Alt+PITCH select, with a recorded-vs-empty selector); more banks or a proper file browser are the next step.

- **Seek / scrub** within the long file (`f_lseek` + ring flush).

- **Transport sync** + tempo-aligned loop points; **reverse** playback (the Rev pad is reserved for it).

- **int16 streaming** — halves SD bandwidth, the main throughput lever.

- **Loop-mode tuning** — the seam-fade length and Frippertronics decay/floor are first-cut constants; tune by ear. A wider / quantized varispeed range; progress/level meters on the rings.

### Docs / housekeeping

- `CHANGELOG.md` — [done] `[Unreleased]` tape entry (kept current with the dual-deck work).

- `README.md` / `docs/engines/README.md` — [done] `tape` in the engine lists, build options, flash targets.

- `docs/engines/tape.md` — [done] rewritten to the dual-deck state, slimmed to user-facing.

- Commit the changeset (user does).

## Build-system notes

- All streaming code is `#if defined(SPK_ENGINE_TAPE)` so non-tape engines are byte-identical (granular only +8 B from the shared `EngineContext` field). The `storage.h` unmount guard and the `wav.h` channels param default (2) likewise leave non-tape builds unchanged.

- The guarded TUs (`stream_deck.o`, `fat_file.o`, `buffer.sdram.o`) depend on `build/.engine-stamp` (like `app.o`) so `make ENGINE=tape` over a stale build doesn't relink empty objects. The granular-engine switch-without-clean contamination is a separate, pre-existing limitation (the `engine-*` targets `make clean` anyway).

## Broader session context (already committed / done)

- `reso` engine shipped (Mutable Instruments Rings voice; renamed from `karp`); pitch bug root-caused (`cv_voct` clobbering the knob) + fixed; defaults engine-seeded; Alt+PITCH model selector; vendored Rings/stmlib trimmed under `src/engine/reso/thirdparty/`. See `docs/engines/reso.md`, `CHANGELOG.md` `[0.2.2]`.

- Opt-in CMake build added (additive; `make -f Makefile.cmake`); the `make` build stays canonical. </content> </invoke>
