# Dev notes — `softcut` engine (dual-deck crossfaded overdub looper)

Implementation/bring-up notes for `ENGINE=softcut`. The user-facing reference (control map, gestures, build/flash) is [`docs/engines/softcut.md`](../engines/softcut.md); the feasibility spike that justified the port and the on-device CPU measurements live in [`docs/dev/softcut-spike.md`](softcut-spike.md).

## TL;DR — where we are

A working, hardware-verified looper on monome's [softcut-lib](https://github.com/monome/softcut-lib): four `softcut::Voice`s (2 per deck, a Rev-swapped pair like the shuttle engine). softcut's read/write head plays AND records the same loop with subsample-accurate click-free crossfades and interpolated overdub — the sound-on-sound that shuttle/tape can't do. **Record-defines-loop** semantics (first Alt+Play records a fresh take, the second closes the loop at the played length), SD load + save, varispeed/reverse, a sweepable post filter, and a click-free Seq-pad voice realign. Normal `-O2` SRAM build (~89% SRAM_EXEC). Host-tested (`make -C host test-softcut`).

## Architecture

- `SoftcutEngine` holds `softcut::Voice _voice[2][kTracks]` (kTracks=2 → 4 voices). The voice STATE lives in SRAM (engine members, ~9.3 KB each); only the audio BUFFERS are in the SDRAM arena, via softcut's `setVoiceBuffer()` split.

- Per-voice loop buffer: `kBufFrames = 2^19 = 524288` frames = **10.9 s** = 2 MB (8 MB for 4). Power-of-2 is a hard softcut requirement (`SubHead` wraps via bitmask).

- The vendored softcut core is in `src/engine/softcut/vendor/{include,src}` (5 `.cpp`: Voice, ReadWriteHead, SubHead, Svf, FadeCurves). It is **patched** for the embedded target (see "Vendored port fixes").

- `process()` runs each voice (`processBlockMono`) into a mono scratch, then pan/sums to the stereo bus with the route-aware per-voice pan + A/B blend, soft-clipping while overdubbing. Input to every voice is a **mono sum of both input channels** (so a loop records whichever jack the source is on).

- 4 voices is the hardware-measured CPU budget (~62% avg / 79% peak worst case). A future 6-voice build = land the `std::function`-per-sample removal in `vendor/src/Voice.cpp`, then bump `kTracks` to 3.

## Control map

| Control | Function |
|---|---|
| PITCH | rate, bipolar (noon=stop, CW +2x, CCW reverse -2x); snaps to unity on engage (pickup) |
| POS / SIZE | loop window start / length (over the recorded take) |
| MIX | voice volume; ENV |
| Alt+POS | pan; Alt+PITCH |
| MOD_AMT / MODFREQ | loop crossfade time / rate-slew time |
| FLUX pad | post SVF: +PITCH cutoff, +MIX resonance |
| Play / Alt+Play | roll-stop / record-overdub (record-defines-loop on an empty voice) |
| Rev / Seq | swap focused voice / realign all voices (click-free `cutToPos`) |
| **Alt+Seq tap / hold / Alt+Rev** | save full take / **erase voice** (clean discard) / save trimmed loop (to the Alt+PITCH slot) |
| Routing switch | per-voice pan: manual (DoubleMono) / spread (Stereo) / random (GenerativeStereo) |

## Key mechanisms

- **Record-defines-loop.** An empty voice (`_len==0`): first Alt+Play clears the buffer and records a fresh open take (loop held at the full buffer so the live POS/SIZE knobs can't cap it — `set_param` skips `_apply_window` while `_defining`); the second Alt+Play closes the loop at the head position (`getSavedPosition()`), sets `_len`, snaps SIZE=full/POS=0, and reseeds the pickups. A voice with content overdubs on top instead. `_defining[2][kTracks]` tracks the open take.

- **Overdub feedback (ENV).** `ParamId::Env → setPreLevel`. 1.0 = infinite sound-on-sound, <1 = old layers decay as you add, 0 = overwrite. Only acts while recording.

- **Live monitoring.** softcut outputs the loop, not the input, so `process()` mixes the dry input mix into the output while a deck's focused voice is recording — without it you hear nothing of the take until it wraps.

- **Head start.** Setting `playFlag` alone does NOT move a softcut head; every Play/overdub engage (and SD-load completion) calls `cutToPos()` to kick it into a fade-in.

- **SD load** (mirrors shuttle): boot preload + Alt+PITCH slot select drain a float32-mono WAV into the voice buffer via the platform stream (`SPK_USE_STREAM`); RAM-capped at the 10.9 s buffer.

- **SD save** (new): `_start_save(d, full)` opens `start_record`, then `process()` (ISR) pushes the region's frames into the record ring (honoring the accepted count), the platform main loop drains them to SD, and `prepare()` calls `stop()` to finalize. `full=true` (Alt+Seq) writes the whole take `[0,_len]`; `full=false` (Alt+Rev) writes the POS/SIZE window. float32-mono WAV to the Alt+PITCH slot, so save↔load round-trip.

- **Erase / deferred save** (Option B clean-discard). The platform fires the Alt+Seq tap hook (`on_seq_toggle_arm`) on press and the hold hook (`clear_sequence`, at 1.5 s) only on a sustained hold. To let a hold mean "erase, don't save," the tap only *arms* a full-save (`_save_arm`); `prepare()` commits it after `kSaveArmMs` (1.6 s, just past the hold) if no erase cancelled it. `clear_sequence` cancels the arm and empties the focused voice (`stop()` + `memset` the buffer + `_len=0`), so the next Alt+Play records a fresh take. Alt+Rev's trimmed save has no hold collision and stays immediate.

## Vendored softcut port fixes

All marked `sk-engines PORT NOTE`; re-apply if re-vendoring from upstream:

1. **`TestBuffers.h` → empty stub.** Upstream embeds `float buf[6][131072]` (3 MB) + `<fstream>` per ReadWriteHead (18 MB for 6 voices) for a desktop Matlab dump.

2. **`Voice.h` `std::atomic<phase_t>`(double) → `std::atomic<float>`.** 64-bit atomics aren't lock-free on the M7 and emit `__atomic_*_8` libcalls newlib (nano) lacks → link error.

3. **Dropped `<iostream>` (Resampler.h) + a `std::cerr` (ReadWriteHead.cpp).** Pulled ~150 KB of locale/stream `.text` — the difference between SRAM_EXEC overflow and fitting.

4. **`FadeCurves.h` default member initializers.** `init()` calls the shape setters before assigning the ratio/min members those setters read → first calc reads UNINITIALIZED members; a garbage ratio overruns a 1001-float stack buffer (benign on-device as a zero-init static; ASan stack-buffer-overflow on host where a Voice is on the stack).

5. **`-DM_PI -DM_PI_2`** (Makefile) — strict `-std=c++17` doesn't expose them from `<cmath>` on arm-none-eabi (reso/mosc idiom).

## CPU / memory

See [`softcut-spike.md`](softcut-spike.md) for the on-device METER measurements (2 → ~32%, 4 → ~62%/79%, 6 → ~92%/108%, worst case). Code is tiny (the softcut DSP is ~11 KB SRAM_EXEC); the full firmware links at ~89% SRAM_EXEC at `-O2`. Buffers (8 MB) and the engine arena are in SDRAM.

The host bench under-predicts because softcut is SDRAM-buffer-bound like the reverb — the on-device pass was structural, not a formality.

## Build / flash / test

```text
make ENGINE=softcut            # normal SRAM build (-O2)
make ENGINE=softcut METER=1    # + on-device CPU meter (serial + ring A)
make program-dfu               # flash (enter DFU first)
make -C host test-softcut      # host correctness test
```

**Diagnostic hooks** (guarded, compiled out by default): `make ENGINE=softcut SOFTCUT_EXTRA=-DSOFTCUT_DIAG_PASSTHROUGH` bypasses `process()` to a raw input→output passthrough; `-DSOFTCUT_DIAG_TONE` emits a 440 Hz tone — for isolating codec I/O vs engine logic. NB `make` does not rebuild on `-D` changes alone: `rm build/softcut_engine.o` when switching `SOFTCUT_EXTRA`.

## Bring-up history (hardware, with the user)

Worked through on-device over several DFU cycles; the bugs (and reusable softcut-porting lessons): FadeCurves uninit (ASan); `playFlag` doesn't start the head; input-channel mismatch (fixed by the mono-sum input); no live monitoring; the **empty-buffer record bug** (a fresh voice had no sized, head-aligned loop to record into → fixed by record-defines-loop, the key insight); SIZE capping the open take mid-record. The dual-deck `set_config(Mode)` clobber was caught in the scaffold (gate on `DeckRef::A`).

## Control surface re-evaluation (2026-07-01)

The *panel* is densely mapped (both mod knobs, every pad, the Alt layers, SD save/load). But softcut uses **none** of the CV/gate I/O, ignores the per-deck Mode switch, and leaves a lot of softcut-lib's own DSP unexposed. Confirmed unused hooks: no `cv_voct`/`cv_size_pos`/`cv_mix`/`cv_crossfade`, no `on_gate_trigger`, no `process_cv`/`gate_out_triggered`, no `ConfigId::Mode`, no `ITransport` (the `set_mod_speed` `sync` flag is ignored). For a *looper* the CV/gate omission is the biggest gap.

### Gaps

**A. CV / gate I/O — entirely unmapped (the standout miss).**

| Hook | High-value mapping for a looper |
|---|---|
| **V/Oct in** (`cv_voct`) | → rate — pitched/tuned loops, playable from a keyboard/sequencer (softcut as a sampler) |
| **Size/Pos CV in** (`cv_size_pos`) | → loop position or length (scan/modulate the window) |
| **Mix CV in** (`cv_mix`) | → voice volume (VCA) |
| **Crossfade CV** (`cv_crossfade`) | → deck A/B blend (the controls JSON already claims this, but it is **not** implemented — needs the override, cf. pstretch) |
| **Gate in** (`on_gate_trigger`) | → arm overdub / **retrigger the loop** (clocked looping) / one-shot capture |
| **Gate out** (`gate_out_triggered`) | → a pulse on each **loop boundary** — the loop becomes the patch's clock/reset |
| **Mod CV out** (`process_cv`) | → the loop **phase** as a ramp/phasor locked to the loop (`Voice::getActivePosition()` already exists) |

**B. Free panel controls.**

- **Mode switch** (per-deck 3-way, currently `-`): a natural **record-mode** selector — Overdub (current) / Replace / **One-shot** (softcut has `setRecOnceFlag`). (NB: `set_config(Mode)` must stay gated per-deck — the clobber noted in the bring-up history.)

- **Grit pad** (currently `-`): *reserved for the TapeFx port* (see the deferred-features TODO). An **alternative** use, if TapeFx is dropped, is softcut's **pre-filter** (`setPreFilterFc/Rq/Lp/...`) — Grit shapes what gets *recorded* (lo-fi record chain) while FLUX shapes *playback*. Both are near-free (the pre-filter is already compiled into the vendored `Voice`); pick one.

**C. Unexposed softcut-lib DSP.** Comparing the full `Voice` public API against the setters the engine actually calls, the engine wires ~60% of the voice's control surface. The rest is **not DSP to write** — it is already in the vendored `Voice`, already contributing to the ~11 KB code and the ~62%/79% CPU we pay for, just unwired. Engine-used: `setRate`, `setLoop{Start,End,Flag}`, `setFadeTime`, `setPreLevel` (ENV), `setRecLevel`/`setRecFlag`/`setPlayFlag`, `setRateSlewTime` (MODFREQ), `setPostFilter{Fc,Rq,Lp,Dry}` (FLUX), `cutToPos`, `getSavedPosition`. What's left on the table:

- **An entire pre-filter — completely untouched (the big one).** softcut has *two* independent multimode SVFs per voice: a **pre-filter** on the *record* path (shaping what is written to the buffer) and the **post-filter** on *playback* (the FLUX pad). The pre-filter's eight setters — `setPreFilterFc`, `setPreFilterRq`, `setPreFilterLp/Hp/Bp/Br`, `setPreFilterDry`, `setPreFilterFcMod` — are all unused, so softcut records the raw input. Wiring it gives a **record-vs-playback tone pair** (lo-fi/telephone loops, rumble removal before committing, a record-side character distinct from playback) for zero new DSP. This is the concrete capability behind the "Grit → pre-filter" idea in **B**.

- **The post-filter is multimode, but only LP is used.** The FLUX pad drives `setPostFilterLp` (+ `Fc`/`Rq`/ `Dry`); the same SVF also exposes `setPostFilterHp/Bp/Br` (highpass / bandpass / notch), blendable via the per-mode level setters. Four of five shapes + morphing are unused — the FLUX pad runs a fifth of the filter it already pays for.

- **Phase quantization + phase poll (4 methods).** `setPhaseQuant`, `setPhaseOffset`, `getQuantPhase`, `updateQuantPhase` are the built-in machinery for **clock-locked loops / quantized cuts** and for reading a voice's loop phase. Their absence is *why* the Seq realign must be an immediate `cutToPos` rather than beat-quantized, and why there is no loop-phase signal to drive a CV/gate out. With the injected `ITransport` (unused today) + **Alt+Cycle** (`set_mod_speed`'s `sync` flag, currently ignored) this becomes clock-sync. Overlaps the deferred "phase-quantised voice sync" TODO.

- **`setRecOffset`** — the record head can lead/lag the play head by an arbitrary offset: the primitive for **delay-style overdub** (record what you played N ms ago) and Fripp-ish feedback-loop textures — something shuttle/tape structurally can't do and softcut gives for free.

- **`setRecPreSlewTime`** — smooths record/pre-level changes; unused, so turning **ENV (overdub feedback)** or arming record steps the level instantly (a slew would make those click-free).

- **`setRecOnceFlag`** — one-shot "capture one pass then auto-disarm" record (pairs with the Mode-switch record-mode idea in **B**).

- **`getActivePosition`** — the audio-thread play-head position (the engine only uses `getSavedPosition`, for closing the record-defined loop). This is what a precise position display or a **loop-phase CV out** reads.

### Recommendations (by value / cost)

1. **CV in + gate in + CV/gate out** — near-free (override the hooks, route into existing setters + `getActivePosition`), the single biggest capability jump: softcut becomes a modular-playable, clock-emitting looper. Confirm V/Oct→rate as the pitch axis. *Standout.*

2. **Grit → pre-filter** — near-free, uses lib DSP already present; a record-chain vs playback-chain filter pair. **Only if** the reserved TapeFx port is not wanted on Grit (mutually exclusive).

3. **Clock-synced loops** (Alt+Cycle + `phaseQuant` + transport) — medium cost, most musical; upgrades the manual Seq realign to beat-locked sync. Folds in the existing phase-quant TODO.

4. **Mode switch → record mode** (Overdub / Replace / One-shot via `setRecFlag`/`setRecOnceFlag`/`preLevel`) — low cost.

Suggested first pass: the CV/gate I/O batch (#1) — near-free and independently testable — then clock-sync (#3). #2 and #4 are small opportunistic adds gated on the Grit/TapeFx decision.

## TODO / open items

Build: `make engine-softcut` (clean → build → DFU); `make ENGINE=softcut METER=1` for the load meter.

### Features (deferred, by user decision)

- [ ] **Additional FX, degree-blendable, *preserving* softcut's built-ins.** Do NOT remove any built-in softcut DSP (the user is explicit). Only ADD opt-in FX if there's CPU/space headroom. The shuttle `TapeFx` (wow/flutter + Jiles-Atherton saturation + resonant LP) port is scoped; the **GRIT pad is reserved** for it (grit+PITCH/grit+MIX), since FLUX already drives softcut's own filter. Gate: a `METER=1` headroom check with FX engaged on all 4 voices (softcut alone is ~62%/79%); bypass when neutral like shuttle.

- [ ] **6 voices.** Gated on removing the `std::function`-per-sample dispatch in `vendor/src/Voice.cpp::processBlockMono` (templated/switch dispatch, ~15-30% on M7). Then bump `kTracks` 2→3 and re-run METER.

- [ ] **Phase-quantised voice sync.** The Seq realign (`cutToPos`) covers the musical case; softcut's `setPhaseQuant`/`syncVoice` for locked phase is a later add.

### Polish / nice-to-have

- [ ] **`softcut/` dir auto-create on save** — `start_record` fails if the folder is missing; create it (or fall back to card root).

- [ ] **Erase confirm flash** — `clear_sequence` empties the voice silently (it just goes idle); a brief distinct flash would confirm the destructive action.

- [ ] **Save progress on the ring** — currently a solid amber ring while writing; a progress arc (`_save_pos`/total) would be clearer for long takes.

- [ ] **Deferred-save UX** — the Alt+Seq full-save commits ~1.5 s after the tap (so a hold can become an erase). If that lag annoys in practice, revisit (e.g., a shorter dedicated hold, or commit-on-release if a release hook is added to the platform).

- [ ] **Deferred-save drop edge case** — if a trimmed save (Alt+Rev) is in flight when the deferred full-save commits, `_start_save` no-ops (deck busy) and the full save is silently dropped; re-arm or queue if it matters.

### Pre-release cleanup

- [ ] **Strip the `SOFTCUT_DIAG_*` hooks** (passthrough/tone in `process()` + the `SOFTCUT_EXTRA` Makefile line) before a release tag, or keep them as documented debug aids.

- [ ] Commit (engine + vendored core + docs + tests; currently all uncommitted).
