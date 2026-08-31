# Dev notes - the storyteller (`bard` engine)

Implementation and bring-up notes for `ENGINE=bard`. The user-facing reference (controls, card layout, the sidecar grammar, build commands) is [`docs/engines/bard.md`](../engines/bard.md); that document also records the seven design decisions and the reasoning behind them. This file holds the internals, the file map, the bugs found on the way, and the risks.

## Status - pick up here (2026-07-30)

- **Feature-complete in code, host-tested, and confirmed working on hardware.** `make -j8 ENGINE=bard` links at **62.8% SRAM_EXEC** (167,080 B of 260 KB, built `-Os`), SRAM 52.7%, SDRAM 78.2%. CMake is at parity (63.2%). `make -C host test-bard` runs **144 checks** green and is wired into `make -C host test`; `make test-scripts` covers the card-prep script.

  > Figures re-measured 2026-08-31. They previously read "~88% SRAM_EXEC (167,000 B of 186 KB), SRAM ~41%": the byte counts had barely moved, but the SRAM_EXEC region has since grown from 186 KB to 260 KB and the percentages were never re-derived from a build. Quote bytes-of-region, not a bare percentage, when updating these.

- **What is left is the measured pass**, the same one TODO P2 scopes for the other engines: no `Meter::cpu` number has been taken and none of the feel constants have been tuned by ear.

- Everything in the design doc is now built, including the two items originally deferred: the **WSOLA pitch-keep** on ENV (decision #3's experiment) and the **Grit room**.

## Reused platform stack

Built on the same SD-streaming service as [tape](../engines/tape.md), [radio](../engines/radio.md) and [pstretch](../engines/pstretch.md): the platform `StreamDeck` (lock-free per-deck SDRAM ring + a main-loop FatFs pump), gated by `SPK_USE_STREAM` and injected via `EngineContext::stream`. bard reuses `RawStreamReader` (the int16-mono `.raw`/`.wav` codec), `scan_bank` (the 8.3 + macOS-AppleDouble filter), `bank_sort`, and `start_play_raw`/`start_play_wav`'s seek-on-open. The selector hysteresis and settle-every-prepare anti-stutter guards come from `radio_engine.cpp`.

### Two contract additions

Both are mirrors of calls that already existed, both have `return false` default bodies so no other engine is affected:

- **`IStreamDeck::write_text(path, buf, n)`** - the mirror of `read_text`, backed by `FatFile`'s `open_write`/`write`. Used for the resume table and for committing a mark list to a sidecar. `StreamDeck::write_text` opens its own local `FatFile`, so it can never race a streaming deck's handle.

- **`IStreamDeck::seek_play(deck, frame)`** - the **light in-file seek**: an `f_lseek` on the deck's already-open handle plus a ring flush, instead of the close + `f_open` + (for a `.wav`) header re-parse that `start_play_*` pays. This is the addition [`radio-impl.md`](radio-impl.md) already listed as wanted, so bard pays down a recorded debt rather than inventing scope. Sequencing matters and is commented at the call site: drop the deck to `idle` first so the ISR stops consuming (an idle deck's `play_consume` returns 0, which the engine reads as an underrun and handles as silence), then seek, then re-arm. `PlayStream::start` resets the ring and clears the EOF latch, so a deck that had already run out becomes playable again.

`_seek()` tries `seek_play` first and falls back to `stop` + `start_play_*`. Because `seek_play` returns false on a deck that is not playing, the initial open of a book falls through naturally, and `_open_book` always stops the deck first, so a live deck is never on a different file when the light path is taken.

## Architecture: the ISR does nothing slow

- **`process()` (audio ISR):** per deck, pull int16 frames from the ring, resample, feed/drain the WSOLA time-scaler, then apply the Flux colour, the Grit room and the seam fade to the finished voice; then the per-block duck envelopes and the stereo mix. No FatFs, no allocation, no seeks, **no `powf`** (the rate chain is precomputed on the main loop).

- **`prepare()` (main loop):** every `f_open` / `f_lseek` / directory scan / sidecar read / sidecar write / resume write, plus the selector settle logic, segment-end handling and gate-out edge detection.

- **Pads, gates and the transport tick only set flags.** The transport `set_on_tick` callback runs in the audio-block context, so an armed clock advance sets `_tick_advance[i]`. Every jump funnels through `_request_jump()` into a single pending `{_req, _req_frame}` per deck, so one main loop performs at most one seek per deck however many sources asked.

### The owned playhead

`_pos[i]` counts source frames actually consumed. `_pull()` increments it **only when the ring returned a full frame**, so a starved ring freezes the playhead instead of running it ahead of what was heard - otherwise an underrun would silently skip the book forward and then checkpoint that wrong position to the card (host test C18). `_pos` is written by the ISR and read by the main loop as a plain aligned 32-bit word: a single-instruction load/store on the M7, so no torn read; the main loop may see it one block stale, which is irrelevant here.

### Segment boundaries: the ISR gates, the main loop seeks

A segment end needs a seek, which cannot happen in the ISR. `_render_deck` stops feeding the moment `_pos` crosses the cached `_seg_end` and the drain then comes up short, so the block is zero-filled from that point; `prepare()` sees `_pos >= _seg_end` and loops / advances / holds per the mode. Audio therefore never bleeds into the next chapter even though the seek is up to one main-loop period late.

In `Read` the segment is the whole book (`_seg_end = book_frames`), so marks are jump targets only, and a deck that simply hits EOF (books open with `loop=false`) lands in the same handler via `!is_playing`.

### The rate chain (RATE x PITCH-KEEP)

Resample by `k = rate^(1-keep)`, then WSOLA time-scale by `alpha = rate^-keep`. Composed, that is a speed change of exactly `rate` with pitch `rate^(1-keep)` - so **ENV changes only how the narrator sounds, never how fast the book reads** (host test C22 pins that invariant). `keep = 0` gives `k = rate, alpha = 1`, and at `alpha == 1` the time-scaler is a **bit-exact passthrough** (test D1), so zero is the varispeed path unchanged rather than a close approximation. `_update_rate_chain()` recomputes `k`/`alpha` when SIZE or ENV moves - both main-loop writes - so the ISR never calls `powf`.

### WSOLA, and why not the pstretch FFT

`src/engine/bard/wsola.h`. Reusing pstretch's PaulStretch looks free but is the wrong algorithm: its mechanism *is* phase randomization, which destroys exactly the phase coherence that makes consonants intelligible - right for an ambient wash, wrong for a book at 1.5x. WSOLA (Verhelst & Roelands, ICASSP 1993) picks each next frame by waveform similarity, so periodicity, pitch and intelligibility survive, and it is far cheaper.

1024-sample frame (~21 ms, at least two pitch periods of a low male voice), 512 hop, Hann at 50% overlap (which sums to unity, so no output normalization). The similarity search is the entire cost: searching +/-256 lags naively would be ~46 MMAC/s per deck, so the search runs on a **4x-decimated** signal and then refines within +/-4 - order 3 MMAC/s. **That figure is an estimate, not a measurement**; only correctness was verified on the host.

Buffers are plain members (SRAM) rather than arena (SDRAM) on purpose: the similarity search is scattered access over a small window, and scattered SDRAM access on the H7 is ~10x slower - the lesson `pstretch-impl.md` paid for. That is what puts SRAM at ~41%.

### The room, and a licensing trap

`src/engine/bard/room.h`. The design doc said to build the Grit room over the existing `src/dsp/diffuser.h` - but **that file is GPLv3** (a port of qdelay's Diffusor) while the rest of the repo is MIT, so linking it would have relicensed the whole engine. Instead the room is written from the classic published structure: four parallel damped feedback combs at mutually prime lengths into two series Schroeder allpasses (Schroeder, JAES 1962; Moorer, CMJ 1979). Textbook topology, no code or coefficient tables from any GPL source, so bard stays MIT. Three characters (plate / hall / slap) share one allocation sized for the longest, so switching live never reallocates. Delay lines are sequential-access, which SDRAM handles well.

Note `set_character()` clears the tail, so switching character drops the current reverb abruptly - deliberate, since reading stale samples at a different line length would be worse.

## Bugs found and fixed during bring-up

All were caught by the host suite. The first would have been genuinely maddening to diagnose by ear on hardware.

1. **The idle POS knob undid every Seq-pad / gate / clock advance.** The bookmark selector re-quantized every `prepare()`, comparing the knob against the current segment - so the moment anything *else* moved the segment, the parked knob "disagreed" and dragged the playhead back ~180 ms later. Fixed by acting on **movement**, not disagreement (`_mark_x` / `_mark_moved`): it engages only when knob+CV shifts by more than 1%, and `_open_book` seeds it at the knob's current position so a fresh book honours its resumed position instead of snapping to wherever POS sits. The BOOK selector needs no guard - nothing but that knob changes which book is open. Test C19.

2. **The first pad or gate press of a session was swallowed.** `now_ms()` is ~0 at boot and the debounce timestamps started at 0, so `now - last < kDebounceMs` held for the first 180 ms. Fixed with explicit `_pad_seen` / `_gate_seen` flags. (radio has the same latent shape; it matters less there because its Play pad is a no-op on a live deck.)

3. **Two decks on the same book fought over one resume key.** Both decks default to the same shelf and can hold the same file, and `_save_resume` wrote both, so the stored position depended on loop order. Deck A now wins deterministically. "Where was I" genuinely has two answers here, so the fix makes the arbitrary choice *stable and documented* rather than inventing a merge rule.

4. **The loop/hold toggle was dead on a parked deck.** The segment-end handler only runs on a non-paused deck, so flipping to loop (Alt+Seq held) while already held at a segment end did nothing until the next Play press. `clear_sequence` now restarts the segment (or advances, in Wander) when it turns looping on at a boundary.

5. **`thin_marks` could place a mark inside its own minimum gap** (prep script): the start-of-book mark was inserted *after* the gap filter, so it could land closer than `min_gap` to the first detected boundary. Seeded before filtering instead.

Three further failures were **flaws in the tests, not the engine**, and all three are worth remembering for the next engine harness:

- **Deck B leaks into every measurement.** Both decks default to shelf 0 and open a book, so "deck A is silent" can never be observed on the shared bus. Single-deck tests call a `solo_a()` helper that zeroes B's volume.

- **A non-repeating source makes A/B comparisons meaningless.** The fake stream originally served a ramp of period 2000, which does not divide the 96-frame block, so two consecutive equal-length runs never matched and "these knobs change nothing" could not be asserted. The period is now 32.

- **A comparison window shorter than the effect's own pre-delay reads as silence.** The room test first ran 40 ms windows, but each character is silent until its shortest comb (or the ~200 ms slap tap) has filled, so every character compared equal. The window is now 600 ms.

Also worth recording: two `str.replace`-based patches silently no-op'd on an indentation mismatch, leaving a flag that was set but never consumed (the commit-marks handler) and a `test:` target that never ran `test-bard`. Both were caught only because a test failed or an expected line was missing from a log - a reminder to grep for the inserted text after a scripted edit.

## Files

New, all under `src/engine/bard/`:

- `bookmarks.h` - the sidecar grammar (`parse_sidecar`, `scan_time`, `parse_directives`), open-ended-segment resolution and play-order construction (`resolve`), deterministic auto-marks (`auto_marks`, `book_seed`), the writer (`serialize_marks`, `format_time`), the lookups (`mark_at`, `order_slot`), and the `bard.cfg` reader. Header-only, `<cstdint>` only, no allocation, no `strtod`/`sscanf`.

- `resume_table.h` - the 64-entry LRU resume table plus its text `parse`/`serialize`.

- `room.h` - the MIT Schroeder/Moorer room (see above).

- `wsola.h` - the decimated-search time-scaler with a bit-exact bypass.

- `bard_engine.{h,cpp}` - the engine.

Elsewhere:

- `host/test_bard.cpp` - 144 checks in four layers (grammar / resume table / WSOLA / engine through `IEngine`). Uses a **controllable clock** (`FakeTime : ITimeSource`) because every anti-stutter guard is time-based; a wall-clock source would make those paths untestable or flaky.

- `scripts/prepare_audiobooks.py` + `scripts/test_prepare_audiobooks.py` - convert to 24 kHz mono, 8.3-rename, derive marks, write the sidecar and `BOOKS.TXT`. Written against the two real free libraries, both of which carry exact boundaries, so silence detection is the last resort: **LibriVox** ships one 64 kbps MP3 per chapter (`--join` concatenates them into one book with a mark per join, offsets accumulated from each part's actual frame count so they cannot drift, inputs ordered by a natural sort so `chapter_2` precedes `chapter_10`), and a **LoyalBooks `.m4b`** carries an embedded chapter list read via `ffprobe -show_chapters`, titles included. Mark-source priority: `--marks-from` > join points > embedded chapters > silence. The tests assert the limits that would otherwise fail silently on the device (the 64-mark cap thins evenly rather than truncating, and warns when real chapters are dropped; the sidecar is trimmed under `read_text`'s 4 KB; `wav_frames` parses the chunk table rather than assuming a 44-byte header, since ffmpeg may write a LIST chunk and every join mark after the first would drift).

- `scripts/align_bookmarks.py` - **prototype**, opt-in (numpy + a TTS voice), outside `make test-scripts`. Places marks by DTW-aligning the recording against its known text, and indexes word / phonetic-skeleton / rhyme occurrences to author a cut-up. Two lessons from its bring-up are worth carrying: (1) both bugs found were step-label bookkeeping in the DTW backtrack, and both produced plausible-looking output - the tell was that `max drift` equalled the band width *exactly*, and real measurements do not land on round numbers; (2) the first self-test (align synthetic audio against itself) **passed while the code was broken**, because at slope 1.0 the true path is a pure diagonal and never exercises the up-step branch. Only `--self-test-stretch F`, which time-stretches the synthetic audio so the true slope is F, caught it. A test that passes on the degenerate case proves almost nothing.

- `docs/diagrams/controls/bard.json` -> `docs/media/bard-controls.svg` via `make diagrams`.

Edited:

- `src/engine/istreamdeck.h` - `write_text`, `seek_play` (both defaulted).

- `src/hw/stream_deck.{h,cpp}` - both implementations, plus a `raw_src` flag marking whether the live play source is frame-seekable.

- `src/engine/engine_select.h`, `Makefile`, `CMakeLists.txt`, `Makefile.cmake`, `host/Makefile` - register `bard` with `SPK_USE_STREAM` and `-Os`.

## Design notes worth keeping

- **`-Os`, like `reso`/`reverb`.** WSOLA plus the room reach ~94% SRAM_EXEC at `-O2`, which is too little headroom to work in; `-Os` gives ~88%. The M7 at 480 MHz has ample compute margin for this engine (a resampler, a decimated correlation search, four biquads and six delay lines).

- **No `CapStepSequencer`.** The design doc listed it, but `src/ui/core.ui.pads.cpp` calls the Seq-pad hooks unconditionally (gated only on Storage being idle), so bard gets its three Seq gestures without advertising a sequencer it lacks. That file also showed the gesture grammar is the inverse of the doc's first assumption: **plain** Seq tap is `on_seq_trigger`, **Alt**+Seq is `on_seq_toggle_arm`, and Alt+Seq **held** arrives as `clear_sequence` via the platform's hold timer. `stop_if_generating` (tap-hold Play) was free and became COMMIT MARKS.

- **The RATE curve is piecewise.** Unity at centre, 0.5x at 0 and 2.5x at 1 - `exp2((v-0.5)*2)` below centre, `exp2((v-0.5)*2*log2(2.5))` above. The halves meet at 1.0 in value with a slope change at centre, because the range speech wants is not log-symmetric.

- **SEAM is a fade-in, not a crossfade.** A seek flushes the ring, so there is no old stream to cross-fade against; Alt+SOS sets a declick ramp on the new stream. Naming it SEAM keeps that honest.

- **Directives survive a mark-less sidecar.** A sidecar with an `#!bard` line but no timestamps keeps its `order=`/`loop=` and takes the auto-marks.

- **Labels are discarded, so an on-device commit erases them.** `parse_sidecar` drops the label text (nothing can display it) and `serialize_marks` writes timestamps only, so tap-hold Play replaces a hand-labelled sidecar with a bare list. Documented as a trade rather than fixed: retaining 64 labels to round-trip them would cost roughly 2.5 KB of SRAM plus parser and writer complexity, for text the module has no way to show. If it ever becomes worth it, the place to add it is a parallel `char label[kMax][N]` in `MarkList` populated by `parse_sidecar` and re-emitted by `serialize_marks` - both already walk the line.

- **Auto-play on open.** A book plays as soon as it opens, including at boot at its resumed position. The Play pad is the pause; there is no other start gesture, and radio behaves the same way.

## Risks / watch-items

- **The jump rate is still the gating unknown.** Bookmark jumps now take the light `seek_play` path, so the cost is an `f_lseek` plus a ring flush rather than a reopen - but the flush and re-prime remain, and armed segment advance faster than roughly one jump per second is still unproven. The two remaining fixes, in order: **prime shallow** (resume audio after a bounded few ms of ring data rather than a full refill), then the ~615 KB/deck **segment-head cache**. Measure before building the cache.

- **No CPU measurement exists.** Build `make ENGINE=bard METER=1` and read the load meter on the FS_EXTERNAL USB-CDC with both decks streaming, PITCH-KEEP up (WSOLA active) and the room engaged - that is the worst case. The WSOLA cost figures in this document are arithmetic, not measurements.

- **PITCH-KEEP cuts up to ~21 ms of a segment tail.** The time-scaler holds about a frame of pipeline, and the segment-end handler seeks as soon as the *input* crosses the boundary, discarding what is still buffered. Inaudible in speech; worth knowing if it ever matters.

- **Position tracking counts pulls, not emitted samples**, and with WSOLA active the input runs ahead of the output by the pipeline depth. Fine for display and resume; not sample-accurate.

- **`read_text` truncates silently** past 4 KB. The prep script enforces the cap and warns; a hand-written sidecar can still lose its tail.

- **Untuned by ear:** `kSettleMs` (180), `kDebounceMs` (180), `kJumpBackSec` (15), `kSeamMaxMs` (500), `kCheckpointMs` (30 s), `kDuckKnee` (4), the Flux colour band range, the duck attack/release, and every room constant (comb lengths, feedback and damping ranges).

## What's left

1. **Hardware bring-up.** A card of real books, then tune the feel constants by ear. This is the only item that cannot be done off-target, and it now gates everything else.

2. **A `METER=1` CPU number** in the worst case (both decks, WSOLA active, room on).

3. **The jump-rate measurement** and whichever of the two remaining fixes it demands. This unblocks the armed/clocked cut-up modes - the engine's most distinctive feature and its least proven.

4. **A by-ear pass on WSOLA quality.** The frame/hop/search constants are first-cut; speech artifacts below ~0.8x and above ~1.8x are inherent to the algorithm but their severity depends on those numbers.
