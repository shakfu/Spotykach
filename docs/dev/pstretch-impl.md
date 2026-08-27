# Dev notes — real-time PaulStretch (`pstretch` engine)

Implementation, design, and roadmap for `ENGINE=pstretch` — a clean-room, real-time PaulStretch ambient time-smear. This is the developer doc (architecture, the source-mode plan, the file map, risks); a user-facing control reference will live in `docs/engines/pstretch.md` once the source modes settle.

## Status — pick up here (2026-07-01)

**Update 2026-07-01:** the earlier "no SD playback" bug is **fixed and confirmed on hardware** — SD-file mode now reads and streams clips from `/pstretch` on the device. What remains is the by-ear character pass (below), not basic SD function.

**Update 2026-07-01 (b):** the **8192 window is now the default** (~171 ms, lusher wash; 4096 is the `WINDOW=4096` opt-in), flashed and clean by ear incl. the SD+8192 path. A full **modulation / CV / gate control layer** was added — a per-deck LFO (Cycle/Glow) targeting diffusion/stretch/tone via the Size/Pos mod switch, sine/triangle/follower via Mod Type, **Alt+Cycle clock-sync** to tempo; **CV inputs** (V/Oct, Size/Pos, Mix, Crossfade, additive); **gate in** (re-grab / freeze); and **CV/gate out** (the LFO as CV + a per-cycle clock). All reuse the platform's existing mod/cv/gate hooks — no new capabilities. See the Modulation section below; host tests B14–B18. Open item is still the by-ear tuning of the mod ranges + SD character pass.

**Phase 1 hardware-verified clean; Phase 2 (SD-file source + full control rework) landed in code.** Phase 1 is flashed on the H7 (spotykach); boots, glitch-free at the full **4096 / 85 ms** window, CPU (via `METER=1`) **avg ~32% / max ~64%** with both decks. Phase 2 adds the SD-file source mode, the Mode-switch source selector (Live/Capture/SD), and the Aux clip selector; it **builds + links + fits** (Make: SRAM_EXEC ~80%, SRAM ~74%, SDRAM ~78% now that the StreamDeck's 2 MB+32 KB SD rings are linked; CMake SRAM_EXEC ~81%) and **passes `make -C host test-pstretch`** (B7 capture-via-Mode, B8 SD source, B9 Aux clip select, B10 off-rate pitch correction, B11 Alt+POS scrub). The full Phase-2 control plan is now implemented; what remains is **NOT yet hardware-verified** (no card with clips tested on device, no by-ear pass).

Done:

- **Live smear** + **Freeze** (Play pad) + **Capture/hold** (Rev pad — grab the recent ~5 s and loop the stretch *through* it). Dual-deck (A=L, B=R), routing, crossfade, dry/wet, ENV tone, ±1-oct pitch.

- **Real-time performance** solved (see that section below): FFT working set in on-chip SRAM, the hop pipelined across blocks, finely-chunked worker ticks. This was the hard part and it is finished.

- **Phase 2 SD-file source (code complete, host-tested):** the Mode switch is now the **source selector** (0=Live, 1=Capture, 2=SD); SD streams a clip from `/pstretch` *through* the stretch. Reuses the platform `StreamDeck` stack (`SPK_USE_STREAM` now set for pstretch). The voice gained a third "what fills the ring" path (`set_sd`/`sd_want`/`feed_sd`/`sd_rewind`) alongside live and capture; the engine pulls only the few source frames the slow read head needs each block (`sd_want()`), decodes int16->float, and feeds the ring. Self-regulating margin: freeze stalls the head, the gap holds at the margin, the feed (and the clip position) stops. `_wpos`/`_rpos` are rebased per ring wrap in SD mode, so the float-precision risk (#4 below) does **not** bite the hours-long SD drone.

- **Control rework done:** per-deck source is one `Source {Live,Capture,SD}` state driven by the Mode switch (`_set_source` exits the old / enters the new cleanly). The **Rev pad** retired as a capture toggle -> it now **re-grabs** the ring within Capture mode (Live/SD no-op). **Aux (Alt+PITCH) is the clip selector** (`CapAux`): `/pstretch` is scanned once in `prepare()`, the knob quantizes to a clip index, and selection re-opens the clip live on a streaming deck (or is honoured on the next switch to SD). The own-display `render()` shows the clip dots while Alt is held. **Alt+POS is scrub** (`CapAltPos`): POS stays diffusion, Alt+POS seeks the SD playhead to a position in the clip (debounced settle; works frozen). The shared `StreamDeck::scan_bank` now returns clips in **deterministic alphabetical order** (`bank_sort`), so Aux position N is the Nth file by name, not by FAT enumeration (also affects radio's station order).

Async card-mount: the SD card mounts a moment after boot, so `prepare()` keeps any SD deck that is not yet streaming trying to (re)scan `/pstretch` and open a clip until one is playing - self-heals a slow mount, a late-inserted card, an empty folder filled later, or Drift already selected at power-on (no fixed boot window; stops once streaming). If SD is selected but no clips are found, the deck's pad LED shows **red** (vs magenta when streaming) so a card/folder/format problem is visible. Host tests B12/B13.

NOT done / next session:

1. **By-ear pass for SD mode** — SD streaming from `/pstretch` is now confirmed on the device (2026-07-01); still to do: re-measure CPU with clips streaming, and tune the wash character (stretch curve, diffusion, default tone) to taste. The character pass is the one thing only a human can sign off.

2. ~~POS diffusion-vs-scrub contention~~ **DONE** — resolved by **scrub on Alt+POS** (`CapAltPos`): POS stays diffusion; Alt+POS seeks the SD stream playhead to a normalized position in the clip, debounced by a settle timer (mirrors radio, so a sweep only opens the spot you land on) and ignoring sub-1% nudges. Re-seek is `_open_clip(i, start_frame)` from `prepare()` (`_apply_scrub`, main-loop/FatFs); works while frozen (audition frozen spots). Host test B11. Residual: each settled seek re-primes the ring (~170 ms soft swell) — intentional/benign for an ambient scrub.

3. ~~Source-rate ratio for off-48k clips~~ **DONE** — a clip's own rate (from the `.wav` header) is folded into the SD grain read stride (`Stretcher::set_sd_rate`, applied in `_open_clip`), so a 44.1 kHz file plays at native pitch, not ~7% sharp. Raw files assume the engine rate. Host test B10 (zero-crossing) confirms it. (Residual: ~170 ms of soft/near-silent output at SD entry while the ring primes - benign.)

4. ~~**8192 window option**~~ **DONE** — 8192 is now the default (`WINDOW=4096` for the lighter build); ola/fifo move to the SDRAM arena at >= 8192, `kWorkBudget` scales 14->16, flashed clean by ear. A formal METER number at 8192 is still pending (only by-ear so far).

5. ~~**Modulation / CV / gate**~~ **DONE** (host-tested B14–B18; by-ear mod-range tuning still open) — see the Modulation section below.

6. **`float` position rebase (live/capture)** — `_wpos`/`_rpos` lose integer precision after ~6 min continuous (2^24 samples). SD mode now rebases; do the same for the live path before relying on long unbroken live sessions.

Tuning knobs (all in the engine/voice): `kWindow=8192` default (`PSTRETCH_WINDOW`/`WINDOW=4096` for the lighter build), `kHop`, `kRing=2^18` (~5.5 s ring / capture span), `kWorkBudget` (worker ticks/block, shared across voices — `16` at 8192, `14` at 4096; raise if audio underruns, lower if a block overruns), `kChunk=512` (work granularity), `kLut=1024` (smear cos/sin table). Modulation ranges: `kModDiffAmt=0.5`, `kModStrAmt=0.35`, `kModToneAmt=0.4` (full-depth swings — the first knobs to tune by ear), `kModRateMin=0.03` Hz, `kSyncMult[7]` (clock-sync divisions, cycles per beat).

Measuring CPU: build `make engine-pstretch METER=1`; the load meter prints to the **FS_EXTERNAL** USB-CDC (the spotykach USB-C — the app must be *running*, not in bootloader). Read `/dev/cu.usbmodem*` (`screen`/ `cat`). NOTE: `max` is a **sticky running peak** (won't drop on a fix) — watch `avg`. pstretch needs an audio **input** (it's an effect, not a generator) and the wet output is delayed; use Freeze/Capture or a live source to hear it. **Nothing is committed** (user commits).

## What it is

PaulStretch (Nasca Octavian Paul) turns audio into a diffuse, evolving spectral wash: FFT large overlapping windows, RANDOMIZE the phases (keep magnitudes), overlap-add back to time. Advancing the analysis read head slowly — or freezing it — stretches/smears time arbitrarily.

This engine is a **clean-room** reimplementation from the published algorithm description (NOT derived from any GPL source / essej's JUCE port), so it stays **MIT** like the rest of the platform (unlike the GPLv3 `glitch`/`qdelay`). It is **self-contained**: a vendored radix-2 FFT (`engine/pstretch/fft.h`), no CMSIS-DSP — CMSIS-DSP's sources are not compiled into this firmware's `libdaisy.a` and its `arm_rfft_fast_f32` caps at 4096, whereas a vendored FFT runs identically on the host test and the device and takes any power-of-two window.

## The central idea: the source is orthogonal to the stretch

The PaulStretch DSP only ever reads grains from a ring buffer. *What fills that ring* is independent of the stretch math, so one engine can offer several **source modes** behind a selector, all sharing the FFT, window, phase-smear, freeze, and every knob:

```text
            +- Live:    the audio input writes the ring in real time (read head chases it)
ring buffer +- Capture: the input filled the ring, then froze (read head loops a captured span)
            +- SD-file: a clip streams into the ring slowly, kept ahead of the read head
                          |
                          v
                  PaulStretch read head (windowed grain -> FFT -> phase smear -> IFFT -> overlap-add)
```

### Why SD-file streaming is feasible on the H7 (the key insight)

The analysis head advances at `input_rate / stretch`. At 50x stretch it consumes source at 1/50 real time (~1 KB/s), so an SD-file source does NOT need the whole (multi-minute) clip in RAM — it is *streamed* slowly, keeping only a few seconds windowed around the slow read head. SD bandwidth is trivial and the clip can be arbitrarily long (a 3-minute file at 50x plays for ~2.5 hours). This is what makes the classic "stretch a song into an hour-long drone" practical on the device, and it reuses the same `StreamDeck` stack (`SPK_USE_STREAM`, lock-free SDRAM ring + main-loop FatFs pump, slot scan/seek) that the `radio` engine already uses — so SD mode is *reuse*, not a new storage subsystem.

## DSP (engine/pstretch/pstretch_voice.h)

One mono `Stretcher` per deck. Per hop (hop = window/2, 50% overlap):

1. Extract a windowed grain at the read head, pitch-resampled (linear interp). Window = the PaulStretch `(1 - x^2)^1.25`, x in [-1,1] (flat-topped, smooth edges).

2. Forward FFT (vendored radix-2; real input via a full complex transform).

3. **Phase smear**: new phase = original + `diffusion * random offset`. Implemented as a complex rotation
   `X' = X * e^{i*phi}` (preserves |X| with no atan2/sqrt). diffusion=0 reconstructs the grain (clean),
   diffusion=1 gives a uniformly random phase (full wash). Conjugate symmetry kept (bins k / n-k, real DC/Nyquist) so the inverse is real.

4. Inverse FFT, window again, overlap-add into the accumulator, emit `hop` samples normalized by the 50%-overlap COLA gain `1/(w[i]^2 + w[i+hop]^2)`.

5. Advance the read head by `hop / stretch` input samples (frozen = hold -> static evolving drone).

In **live** mode the head is clamped to the written, not-yet-overwritten span of the ring; when it falls too far behind (large stretch) it holds at the trailing edge and the advancing write head drags it forward at real time — so the "stretch" is heard as an evolving smear of the recent past, NOT the literal present. **Freeze** is how you drone on the current instant. (This delayed-smear behaviour is inherent to real-time stretching; the capture/SD modes instead loop/stream a fixed source, so the stretch plays *through* the material the classic way.)

### Real-time performance (the bit that actually matters on hardware)

A naive "do the whole hop when the output runs dry" pull model is unusable on the device, even though the *average* CPU is low. Measured with `METER=1` (the load meter on the FS_EXTERNAL USB-CDC), the journey was **avg 200% / max 4297% -> avg 32% / max 64%**, via three fixes, in order of impact:

1. **FFT working set in on-chip SRAM, not the SDRAM arena** (avg 200% -> 40%). The FFT hammers `re/im` and the twiddle/bit-reversal tables with *scattered* access; on the H7 scattered SDRAM (FMC) access is ~10x slower than on-chip SRAM. The whole FFT working set (`re/im`, `ola`, window, tables, the cos/sin LUT) is engine `.bss` (SRAM); only the big per-voice input ring (~1 MB) stays in the SDRAM arena, where it is read roughly sequentially. **General lesson for H7 DSP engines: keep FFT/scattered buffers in SRAM.**

2. **Pipeline the hop across blocks** (max 4297% -> 145%). A whole 4096 hop in one audio block overruns it and underruns the DMA (a glitch) regardless of average load. The Stretcher is a worker: `write_input()` captures input, `work(budget)` advances a state machine, `drain()` pulls from an output FIFO. The engine shares a small `kWorkBudget` across both voices each block. Because a hop's output lasts ~21 blocks, there is slack to compute the *next* hop incrementally. (This also forced `re/im` to be per-voice, since the two voices' transforms now overlap in time.)

3. **Fine, uniform work ticks** (max 145% -> 64% at the full 4096 window). Coarse ticks (a whole FFT stage, or a whole extract/smear/ola pass) still spiked a block even though avg was only ~36%. So `FFT::step` slices by **butterflies** (`Job{stage, idx}`, `maxBf` per call) and extract/smear/ola process `kChunk` (512) elements per tick. Every tick is now bounded, so `kWorkBudget` (14) flattens the per-block load to ~avg. (An interim build dropped the window to 2048 for headroom; the finer ticks let 4096/85 ms come back at max 64%.)

The cos/sin **LUT** for the smear (random phase -> table quantization inaudible) was added during this but turned out not to be the bottleneck (the FFT/buffer work was). The earlier **phase A/B** (an optimized complex rotation vs an `atan2`/`sqrt` recompute, selectable on the Mode switch) was confirmed equivalent on the host test and showed no measurable load difference, so the recompute path was **retired** - only the rotation remains, and the Mode switch is now free (for the Phase-2 source select).

## Engine (engine/pstretch/pstretch_engine.{h,cpp}, class `PstretchEngine`)

Dual-deck (A = left input, B = right, like the delay), `CapOwnDisplay | CapDualDeck`. Per-deck rings +
the shared FFT/window scratch are sub-allocated from the injected SDRAM arena. Audio path: each voice produces a wet stream; the engine applies dry/wet, an ENV tone low-pass, the routing-switch stereo placement, and the crossfader A/B blend, soft-limited.

### Control map (current)

| Control | Effect |
|---|---|
| **SIZE** | Stretch 1x..64x (exponential) — read-head crawl speed |
| **POS** | Diffusion 0..1 (clean window resynthesis .. full phase-randomized wash) |
| **PITCH** | Pitch shift +/- 1 octave (grain resample) |
| **ENV** | Output tone (one-pole low-pass) |
| **MIX** | Dry/wet |
| **Mode switch** (per deck) | **SOURCE select: 0 = Live, 1 = Capture, 2 = SD-file** (stream a clip from `/pstretch`). The authoritative source selector. (Panel silkscreen Reel/Slice/Drift maps Slice=Live, Reel=Capture, Drift=SD.) |
| **Play pad** | Freeze the read head (evolving drone on the current spot) — per deck, works in any source |
| **Rev pad** | In Capture mode, **re-grab** the recent ring at the current instant (the switch grabs on entry; the pad refreshes). No-op in Live/SD — per deck |
| **Alt+PITCH (Aux)** | **Clip select** — pick which clip in `/pstretch` the SD source plays. Takes effect live if that deck is streaming, else on the next switch to SD. Held = show the selector (`CapAux`) — per deck |
| **Alt+POS** | **Scrub (SD only)** — seek the stream playhead to a position in the clip; POS itself stays diffusion. Debounced (a sweep opens where you land); works while frozen to audition spots (`CapAltPos`) — per deck |
| **Cycle / Glow** | Mod LFO **rate** / **depth** (Glow 0 = off). **Alt+Cycle** = clock-sync the rate to tempo — per deck |
| **Mod Type switch** | LFO **shape** (sine / triangle) or **Follow** (input-envelope follower) — per deck |
| **Size/Pos mod switch** | Mod **target**: Pos = diffusion, Size = stretch, both = tone (pitch is modulated via V/Oct CV) — per deck |
| **CV in** | Additive on the knobs: V/Oct = pitch, Size/Pos = stretch, Mix = dry/wet, Crossfade = A/B blend |
| **Gate in** | Re-grab in Capture, else toggle freeze — per deck |
| **CV / gate out** | **Mod CV out** = the LFO as 0..1 CV; **gate out** = a pulse per LFO cycle (tempo-synced clock). LFO free-runs regardless of depth — per deck |
| **Crossfade** | A/B blend of the two decks |
| **Routing switch** | Stereo topology (Stereo / DoubleMono / GenerativeStereo) |

### Modulation, CV, and gate

A control layer added on top of the source/stretch core (2026-07-01), reusing the platform's existing mod/cv/gate hooks — no new capability flags. All per deck, all **off by default** (the audio path is byte-identical to the un-modulated engine until engaged).

- **Mod LFO.** A per-deck LFO on the free Cycle (`ModSpeed`, rate) / Glow (`ModAmp`, depth) knobs. The **Size/Pos mod switch** (`StartModOn`/`SizeModOn`) picks the target — Pos = diffusion, Size = stretch, both = tone. Pitch is deliberately *not* a switch target: diffusion and tone have no CV jack (so the LFO is their only modulation route), whereas pitch and stretch also have CV jacks, so pitch modulation lives on the V/Oct jack. The **Mod Type switch** (`ModType`/`LfoShape`) selects sine / triangle / **Follow** (an input-envelope follower, fast-attack/slow-release). Applied once per block in `_derive_and_push()` (block rate is ample for the sub-10 Hz range); the LFO free-runs every block so it is always available as a CV source, and the depth only scales the internal effect.

- **Clock-sync (Alt+Cycle).** `set_mod_speed`'s `sync` flag (= Alt held) latches per deck; when synced the Cycle knob quantizes to `kSyncMult[]` (cycles per beat) and the rate is `tempo/60 * mult`, recomputed each block from the injected `ITransport` so it tracks tempo. `deck_leds()` reports `mod_synced`/`mod_type` so the platform lights the Cycle LED (the own-display ring is drawn by `render()`; the panel Cycle/Mode LEDs are a separate channel the platform reads for every engine).

- **CV in (additive).** `cv_voct` -> pitch (semitones/12 octaves), `cv_size_pos` -> stretch, `cv_mix` -> dry/wet, `cv_crossfade` -> A/B blend; calibrated to ~0 unpatched, summed onto the knobs and clamped.

- **Gate in.** `on_gate_trigger`: re-grab in Capture (`set_capture` off/on), else toggle freeze.

- **CV / gate out.** `process_cv` emits each deck's LFO as a 0..1 CV (the app maps it to the DAC and feeds the last value to the Cycle-LED brightness); `gate_out_triggered` returns a latch set on each LFO phase wrap, so the gate out is a per-cycle pulse — a tempo-synced clock when the LFO is clock-synced.

## Roadmap — all three source modes, staged

**Phase 1 (live + capture; no SD).** Live smear (done) + a latched **capture/hold** gesture on the Rev pad: snapshot the recent ring, stop writing live input, and loop the read head *through* the captured span so a big stretch traverses the whole captured phrase (the classic PaulStretch drone from a few seconds of input). Cheap, no new infrastructure. Ship + tune the stretch *sound* and measure CPU on hardware before adding streaming.

**Phase 2 (SD-file source + control rework).** *Landed (code complete, host-tested, builds/fits) — see Status.* An SD-clip source on the `StreamDeck` stack (`radio` is the template): `/pstretch` is scanned once in `prepare()`; entering SD opens the selected clip looping and streams it slowly into the analysis ring ahead of the read head. The streaming is **rate-matched, not pumped blindly**: each block the engine asks the voice `sd_want()` (how many source frames to top the write head up to `kSdMargin` ahead of the read head — ~1 frame/block at 50x), pulls exactly that many int16 frames from `ctx.stream->play_consume`, decodes to float, and `feed_sd`s them. Because want is derived from the read-head gap, it is self-throttling (no overrun) and self-quiescing under freeze. `play_consume`/`feed_sd` run in `process()` (ISR-safe, lock-free ring); the FatFs `scan_bank`/`start_play_*` run in `prepare()`/`set_config` (main loop). **Done in this cut:** Mode switch = source selector (Live/Capture/SD as one `Source` state); Rev pad re-grabs within Capture; **Aux (`CapAux`) is the clip-slot selector** (per-deck, re-opens live on a streaming deck, honoured on entry otherwise); the own-display renders the clip dots while Alt is held; positions/feed/freeze/rebase all working; **off-rate clips are pitch-corrected** by their `.wav` rate (`set_sd_rate` folded into the read stride); **Alt+POS scrubs** the SD playhead (`CapAltPos`, debounced re-seek). The full Phase-2 control plan is implemented. **Still to do:** the on-hardware + by-ear pass.

**Deferred.** Per-deck independent clips, longer windows (the vendored FFT supports 8192 for a smoother wash; 4096/85 ms today), an in-RAM record-to-slot, and a bigger capture ring.

## Files

- `src/engine/pstretch/fft.h` — vendored radix-2 complex FFT (MIT, clean-room), arena-backed tables.

- `src/engine/pstretch/pstretch_voice.h` — the mono `Stretcher` (ring, read head, window, phase smear, OLA; live clamp + capture loop + the SD source path `set_sd`/`sd_want`/`feed_sd`/`sd_rewind` with margin-keep + position rebase).

- `src/engine/pstretch/pstretch_engine.{h,cpp}` — the `IEngine` dual-deck wrapper; holds `ctx.stream`, the per-deck `Source {Live,Capture,SD}` state, the source/clip helpers (`_set_source`/`_open_sd`/`_open_clip`/`_ensure_scan`/`_apply_scrub`/`_build_path`), the Aux clip selector and Alt+POS scrub (`CapAux`/`CapAltPos`, `set_param`/`param`/`set_aux_active`), the `prepare()` scan + scrub settle, and the per-block SD feed in `process()`.

- `host/test_pstretch.cpp` — FFT round-trip/known-bin, finite/bounded/produces-signal, dry passthrough, stretch changes output, freeze, routing, **B7 capture-via-Mode + Rev-pad re-grab, B8 SD-file source, B9 Aux clip select, B10 off-rate pitch correction (zero-crossing), B11 Alt+POS scrub (debounced re-seek)**. A `FakeStream` 3-clip stub proves the engine opens/changes/seeks clips, feeds the ring from the stream, and consumes below real time.

- `src/engine/istreamdeck.h` — `bank_sort(BankEntry*, int)`: case-insensitive insertion sort the shared `StreamDeck::scan_bank` calls so scanned clips/stations are alphabetical, not FAT-enumeration order. Unit-tested in `host/test_stream.cpp`.

- Build: `SPK_USE_STREAM` is set for pstretch in `Makefile` and `CMakeLists.txt` (pulls in the shared `stream_deck.cpp`/`fat_file.cpp` and the app's stream injection). Registered as before in `engine_select.h`, `Makefile.cmake`, `host/Makefile` (+ `engine-pstretch` one-shot targets).

## Risks / open items

- **CPU: measured, comfortable.** `METER=1` on hardware reads avg ~32% / max ~64% at 4096/48 kHz with both decks (see Real-time performance above) — glitch-free. `kWorkBudget` (14) and `kChunk` (512) are the spread/throughput knobs; raise budget if audio underruns, lower it if a block ever overruns.

- **Free-running write head as `float`** — `_wpos`/`_rpos` are floats, so positions lose integer precision after ~6 min of continuous runtime (2^24 samples). Rebase them (keep bounded) before relying on long unbroken sessions; not yet done.

- **Window is 8192 (~171 ms) by default** — a lush, smooth wash; `WINDOW=4096` (~85 ms) is the lighter, meter-verified opt-in (the vendored FFT supports any 2^k). 8192 is flashed and clean by ear (incl. SD+8192); a formal METER number at 8192 is still pending.

- **Delayed-smear UX** in live mode (above) — intentional; capture/SD modes are the "play through the material" answer.

- **Build status:** with Phase-2 SD streaming linked it is now a `SPK_USE_STREAM` engine; links + fits (`SRAM_EXEC` ~80% Make / ~81% CMake, SRAM ~74%, SDRAM ~78% incl. the StreamDeck SD rings), host test green (incl. B7/B8/B9/B10/B11). Phase 1 is **hardware-verified clean**; **Phase-2 SD mode is not yet hardware-tested** (no card-with-clips run on device).
