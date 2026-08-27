# Dev notes — buffer-based `shuttle` engine (bipolar/reverse varispeed tape, 4 tracks)

> The `shuttle` engine is the **buffer-based** counterpart to the SD-streaming `tape` engine (`tape-impl.md`). Where `tape` streams from the card and is forward-only, `shuttle` holds audio in SDRAM so the read pointer is random-access — which is what makes **reverse**, **freeze**, and **loop windowing** trivial. The trade is a finite, RAM-capped tape length. The user-facing reference (controls, loop window, audio source, Tape FX, build commands) is [`docs/engines/shuttle.md`](../engines/shuttle.md); this file holds the internals, the file map, the memory-variant menu, and dev notes.

## TL;DR — where we are

Four independent in-SDRAM mono tape tracks (2 decks x 2 tracks). Each track has a signed fractional read pointer over its own buffer; PITCH is a **bipolar capstan-speed** knob (noon = stop/silence, CW = forward to +2x, CCW = reverse to -2x). The four tracks free-run and sum into a stereo bus; you flip the edit focus per deck with the Rev pad (edrums mechanism) and re-align all four to a common downbeat with the Seq pad. Each track plays a **loop window** (POS = start, SIZE = length) of its buffer.

- **Current config:** `kBufSeconds = 30`, 48 kHz, mono `float32`. 4 buffers x 5.76 MB = **23 MB** of the 48 MB engine arena.

- **Status: host-verified, pending hardware bring-up.** All eleven host-test groups (`make -C host test-shuttle`) pass and `make ENGINE=shuttle` builds + links on target (SRAM_EXEC ~89% with the tape FX, SDRAM arena reserved whole / static). Not yet flashed; in particular the four-instance Jiles-Atherton CPU load (see [Tape FX](#tape-fx)) is still to be confirmed on a `METER=1` build.

- **Files:** `src/engine/shuttle/shuttle_engine.{h,cpp}`, `host/test_shuttle.cpp`, plus `src/engine/tape/tapefx.h` (shared tape-FX kernel). Platform: the SD-load path reuses the streaming service via the `SPK_USE_STREAM` capability flag (see below).

## Architecture

- **Buffer-based, contiguous.** Each track is one contiguous `float*` buffer carved from the arena bump allocator (`engine/arena.h`) at `init()`. `_cap = kBufSeconds * sample_rate` frames per buffer.

- **Signed fractional read pointer** (`double _read[2][2]`, `double` because a 30 s buffer is >1.4 M frames — too coarse for a `float` index). Playback advances `_read += _speed` and linear-interpolates; reverse is just a negative `_speed`; freeze is `_speed == 0` -> silence. Ends **wrap** (loop), both directions.

- **Per-track state** `[deck][track]`: buffer, length, read pointer, speed (+knob readback), volume, POS/SIZE, tape-slot, load state, realign/declick.

- **Per-deck state:** active (focused) track, pan (Alt+POS, shared by both tracks), pickup-reseed request, slot-existence cache.

- **Per-track FX** (4 kernel instances) placement-new'd in the arena at `init()`, applied to each track's mono signal (when engaged + rolling) before the pan/sum. See [Tape FX](#tape-fx).

- **Audio path** (`process`, the ISR): render all four tracks, run the optional per-track FX, then the gains; each deck = its two tracks summed at their per-track volume (track volume x A/B mix-fader x per-track pan); then per-deck pan x A/B mix-fader -> stereo `out`. All four play at once.

Capabilities: `CapOwnDisplay | CapDualDeck | CapAux | CapAltPos | CapPitchPickup`.

## Control map

| Control | ParamId | Scope | Meaning |
|---|---|---|---|
| PITCH | `Speed` | per-track | bipolar capstan speed (noon=stop, ±2x); pickup-gated (`CapPitchPickup`) |
| POS | `Pos` | per-track | loop window **start** |
| SIZE | `Size` | per-track | loop window **length** (full -> short stutter, floor `kMinLoopFrames`) |
| MIX | `Mix` | per-track | volume |
| Alt+PITCH | `Aux` | per-track | tape-slot select (8 files/deck) -> triggers SD load |
| Alt+POS | `AltPos` | per-deck | pan (equal-power) |
| MIX fader | `Crossfade` | global | deck A/B blend |
| Play pad | `on_play_pad(rev=false)` | per-deck (focused track) | toggle rolling; **snap to unity** on engage |
| Rev pad | `on_play_pad(rev=true)` | per-deck | **swap** focused track (+ pickup reseed) |
| Alt+Play | `on_record_pad` | per-deck (focused track) | record (play XOR record) |
| Seq pad | `on_seq_trigger` | global | **re-align all four** tracks to their loop start (declicked) |

## Key mechanisms

- **Bipolar speed map** (`speed_from_knob`): a deadzone about noon -> exact 0 (reliable stop -> silence); outside it, linear 0 -> ±`kMaxSpeed`. Unity (+1x) lands off-centre (~0.765), hence the Play snap.

- **Play -> unity snap + soft-takeover.** Play sets the focused track to +1x and arms `take_param_reseed`. The platform reseeds its MValue pickup from `param(Speed)` so the absolute pot must be swept across unity before it retakes the speed. Requires routing PITCH through the **pickup-gated** path — `CapPitchPickup` (a one-branch change in `core.ui.cpp`); other engines keep raw PITCH.

- **Rev-pad track swap** (edrums mechanism): `_active[d] ^= 1` + `_want_reseed`. `set_param`/`param` index the focused track, so the knobs repoint; `_reseed_focus` (which already lists Speed/Pos/Size/Mix/ Aux) catches them without a jump. Both tracks keep playing.

- **Seq-pad re-align + declick.** `on_seq_trigger` sets a per-track `_realign` flag (main-loop write); the ISR zeroes the pointer to the window start at the top of `_render_track` — race-free (no cross-thread `double` write), and all four snap on the same block (atomic downbeat). An audibly-rolling track dips through a short gain ramp (`kDeclickRamp`, ~1.3 ms/side: fade out -> jump at the gain minimum -> fade in) so the jump is click-free; a silent track snaps instantly.

- **Loop window** (`_window`): SIZE sets length (clamped `[kMinLoopFrames, len]`), POS slides the start over the uncovered tail (`start = pos * (len - length)`), so the window always stays inside the recording. Computed per block (no cached state to invalidate); the read pointer wraps within `[start, start+length)`; realign snaps to `start`.

- **Audio source = record + load.** Record copies the live input into the buffer (overwrite from 0). Load drains a `/tapes/` WAV from the card into RAM over a few `prepare()` passes; it advances by `play_consume`'s **actual** returned byte count (which zero-fills only the destination tail), so an underrun is re-pulled, never baked in as silence.

## Tape FX

Each track can run the same tape-FX kernel as the `tape` engine — **wow/flutter** (a modulated fractional delay) -> **Jiles-Atherton hysteresis saturation** -> a **post-FX resonant low-pass** — shared verbatim from `src/engine/tape/tapefx.h`. Four kernel instances are placement-new'd in the arena at `init()`; the FX is **per-track** and addresses the focused track, like every other knob (it repoints on a Rev swap). It is driven from shuttle's free controls (POS/SIZE are taken by the loop window): the two end-of-chain effect pads plus the MOD knobs — grit+PITCH drive / grit+MIX character / flux+PITCH cutoff (boots fully open) / flux+MIX resonance / MOD_AMT depth / MODFREQ rate.

- **Neutral = bypassed.** With drive/character/wow at zero, cutoff fully open and resonance zero (the boot defaults), the kernel is **skipped entirely** rather than run flat: the wow/flutter delay line imposes a fixed ~25 ms delay even at zero depth and the filter is not bit-identity even fully open, so a "neutral" kernel would not be transparent. Skipping it keeps shuttle's varispeed playback **bit-faithful** when the FX is untouched, and confines the (non-trivial) Jiles-Atherton cost to tracks that are both rolling and actually using the FX.

- **Conditional limiter.** The summed bus is `SoftLimit`ed only while the FX is engaged (saturation + a resonant peak can overshoot 0 dBFS); with the FX off the bus stays a plain linear sum.

- **CPU watch-item.** Four Jiles-Atherton instances are the unmeasured target cost (SRAM_EXEC ~89% with the FX linked); confirm on a `METER=1` build during bring-up.

## Memory model — read this before "optimizing"

The engine arena is a **statically reserved, engine-exclusive** block: `static uint8_t DSY_SDRAM_BSS _arena[kEngineArenaBytes]` = **48 MB, always linked** (of 64 MB SDRAM). Whatever the shuttle engine bump-allocates, the 48 MB is committed at link time.

**Consequence:** reducing runtime buffer usage (shorter tracks, dynamic growth) frees **nothing usable** on a single-engine firmware — the freed arena bytes just sit idle, because nothing else claims them. The only levers that change the *committed* footprint are static: `kBufSeconds`, sample rate, sample format, and `kEngineArenaBytes` itself. The bump allocator has no `free`/`realloc`, so buffers also cannot grow in place after `init()`.

Current footprint (30 s, 48 kHz, float32): per track 1.44 M frames x 4 B = 5.76 MB; **4 tracks = 23 MB** of the 48 MB arena (~25 MB arena headroom).

## Variants to experiment with

Memory scales **linearly** with seconds, rate, and bytes/frame; they **stack multiplicatively**. Per track = `seconds x rate x bytes`; the table is for 4 tracks.

| Variant | Per track | 4 tracks | vs now | Audio cost | Scope |
|---|---|---|---|---|---|
| **30 s / 48 k / f32 (now)** | 5.76 MB | **23.0 MB** | — | full | shipped |
| 60 s / 48 k / f32 | 11.52 MB | 46.1 MB | +23 MB | full | one constant; ~2 MB arena headroom left |
| **int16** (30 s / 48 k) | 2.88 MB | 11.5 MB | -11.5 MB (½) | ~none (96 dB) | small; convert at the buffer R/W boundary |
| int16 (60 s / 48 k) | 5.76 MB | 23.0 MB | = now, 2x length | ~none | small |
| 32 k storage (30 s / f32) | 3.84 MB | 15.4 MB | -7.6 MB | 16 kHz bw | medium (record decimator) |
| int16 + 32 k (30 s) | 1.92 MB | 7.7 MB | -15.3 MB (⅓) | lo-fi | medium |
| chunked pool | pay-per-use | up to pool | flexible | full | large |

### A. 60 s `kBufSeconds`

One-line change (`kBufSeconds = 60`). 4 x 11.52 MB = 46.1 MB — fits the 48 MB arena with **~2 MB to spare** (near the ceiling for 4 contiguous float tracks). No quality cost. Verified to build + host-green when tried. The only risk is arena exhaustion: `Arena::alloc` returns null past 48 MB and `init()` defensively sets `_cap = 0` (silent tape) — host tests catch this (a 0-cap buffer fails the record round-trip assertions).

### B. `int16` storage (recommended memory lever)

Store samples as 16-bit (`int16_t`, 4 B -> 2 B). **Halves memory** (same as 60->30 s but keeps 60 s, or frees 11.5 MB at 30 s), with **full 24 kHz bandwidth and no aliasing** — 16-bit is ~96 dB, inaudible loss for tape. Contained change: convert float->int16 (clamp + scale by 32767) on the record/load write, and int16->float (x 1/32768) on the read; the interpolation, declick, wrap, loop window, and load drain are unchanged in shape. Mirrors the granular engine's path in [lofi-int16-scope.md](lofi-int16-scope.md) (same "lo-fi via bit depth, not rate" rationale). Cost: a multiply+clamp on write, a multiply on read. This is the first lever to reach for if the goal is footprint.

### C. Lower storage sample rate

Store the buffers at `Fs_buf < Fs_engine` (engine stays 48 kHz). **Playback is nearly free** — shuttle already fractional-resamples, so "unity" becomes a pointer step of `Fs_buf/Fs_engine` (e.g. 0.5 for 24 k). **Record needs an anti-alias decimator** (a cheap box/halfband filter; dropping samples without one aliases highs into the band). Saves linearly (24 k = ½, 16 k = ⅓) at the cost of bandwidth — which is on-theme for a lo-fi varispeed tape. Do **not** lower the *platform* rate (`app.cpp`) for this; that touches the codec and every engine. See [lofi-path-b-scope.md](lofi-path-b-scope.md) for the rate-change surface area (frame<->tempo coupling) on the granular side; shuttle is free-running so it avoids most of that, but the record decimator is still required.

### D. Chunked / paged pool allocator (true dynamic + flexible budget)

The only way to **pay-per-use** and let tracks grow. Replace the per-track contiguous buffer with a list of fixed-size blocks (e.g. 1 MB chunks) drawn from one shared pool; total live memory = sum of actual track usage, so an empty track costs ~nothing and one long track can borrow from three short ones (a shared budget instead of four fixed slots). Cost is real:

- Read path maps `frame -> (chunk, offset)` (cheap with power-of-two chunk size: shift/mask), with boundary-crossing interpolation, wrap, and reverse all chunk-aware.

- Chunk acquisition runs in the **main loop**, not the ISR (no allocation at audio time) — record pre-acquires chunks ahead in `prepare()`, with a "tape full" state when the pool is exhausted.

- Declick, loop window, and the load drain become chunk-aware.

This re-introduces some of the non-contiguous complexity the shuttle deliberately avoided. **On a single-engine firmware it does not reduce the committed footprint** (the arena/pool is still statically reserved) — its value is *flexibility* (uneven track lengths) and a higher effective ceiling, not idle RAM savings. Worth it only if you want one very long track alongside short ones. Scope as its own task.

### E. Soft length cap ("short by default, long on demand")

If the goal is just UX, not memory: keep the buffers physically allocated at max, but enforce a runtime `_max_frames` (default e.g. 15 s, raisable) on record/loop. ~10 lines, no allocation change, **zero** memory effect. Orthogonal to A-D.

### Decision guide

- Want **footprint** -> B (`int16`) first; combine with C if you also want the lo-fi color; set `kBufSeconds` + `kEngineArenaBytes` to the static size you actually need.

- Want **more time, have the RAM** -> A (60 s) or B-at-60 s.

- Want **flexible per-track length** (one long, others short) -> D (chunk pool).

- Want **default-short loops** as feel -> E (soft cap).

## Platform touch points

- `CapPitchPickup` (`engine_params.h`): routes PITCH through the pickup-gated path so the Play snap holds (`core.ui.cpp`, `_pitch_pickup`). Other engines unaffected.

- `SPK_USE_STREAM` (Makefile): capability flag (set by `tape` and `shuttle`) gating the SD streaming service — `app.cpp` (construct/pump/inject `_stream`), `hw/buffer.sdram.{h,cpp}` (rings), `hw/stream_deck.cpp`, `hw/fat_file.cpp`, `memory/storage.h` (keep card mounted). Replaced the old engine-name `SPK_ENGINE_TAPE` guards so any streaming engine opts in via one flag.

## Testing

`host/test_shuttle.cpp` (11 groups, `make -C host test-shuttle`): speed map; record + unity playback; reverse-wrap + silence-at-noon + loop; Play->unity snap (one reseed); Rev swap + track independence; SD load via a fake stream; Seq re-align (all four atomic); realign declick (no large output step across the jump); POS/SIZE loop window; plus the tape-FX groups (bit-faithful bypass when neutral, engaged-FX path). Uses a controllable `FakeClock` (to step past the 300 ms pad debounce) and a `FakeStream` (to drive the load drain).

## Open items

- Both-tracks-audible is intentional (the headline feature). A "only-focused audible" mode would be a one-line gate in `_render_track` if ever wanted.

- POS/SIZE knob moves jump the read pointer without a declick (only Seq realign is declicked); acceptable as a scrub artifact, but a ramp could be added if it sounds harsh.

- Variants A-E above are unimplemented experiments.
