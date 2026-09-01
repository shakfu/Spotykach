# Scope: one accepted WAV format across every engine

Goal: replace the **four incompatible on-card audio formats** ([`docs/sd-card.md`](../sd-card.md)) with a single permissive *acceptance* rule — WAV, 8/16/24-bit PCM or 32-bit float, mono or stereo, any sample rate — while leaving every engine's *write* format exactly as it is today.

Scope was produced by a full read of `src/memory/{wav,wav_stream,raw_stream,pcm_loader,pcm_convert,sample16,audio_stream}.h`, `src/hw/{card,stream_deck}.cpp`, and the load/playhead paths in the `tape`, `shuttle`, `softcut`, `radio`, `bard`, `pstretch` and `granular` engines. Line numbers drift; grep the named symbols.

## Current state: four formats, three copies of the chunk walk

| Path | Engines | Accepts | Conversion on load |
|---|---|---|---|
| `card.cpp` `init_read_audio` + `PcmLoader` | granular | WAV, **stereo**, 48 k, f32 **or** int16 | **yes** — depth converted (`convert_pcm_block`) |
| `WavStreamReader::begin` (`wav_stream.h`) | tape, shuttle, softcut | WAV, **mono**, 48 k, f32 only (int16 only under `LOFI_INT16`) | **none** — body bytes land directly in float frames |
| `RawStreamReader::begin` (`raw_stream.h`) | radio | headerless `.raw`, int16 mono, rate by convention / `radio/rate.txt` | int16 → float in the engine |
| `RawStreamReader::begin_wav` | radio, bard, pstretch | WAV, int16, mono, **any rate** | int16 → float + rate rebase on the playhead step |

`chuck` and `csound` read text patches (`.ck` / `.csd`) only and are out of scope.

The RIFF chunk walk itself is written **three times** in C++ (`wav.h wav_header()`, `wav_stream.h:52`, `raw_stream.h:44`), plus once in Python (`scripts/card_layout.py`) and once in JS (`web/`). The three C++ copies already agree on the hard parts (`WAVE_FORMAT_EXTENSIBLE` unwrapping, word-aligned chunk stepping, a `kMaxChunks` guard) — they are duplicated, not divergent. That duplication is a maintenance cost independent of the format question and is worth paying off first.

## The analytical key: four axes, only one is load-bearing

Every difference in the table above lives on exactly one of four axes. Sorting them is what turns this from "unify four formats" into "one small decorator plus one policy decision".

- **Bit depth** — pure sample arithmetic. No timing, no length, no indexing consequence.
- **Channel count** — pure sample arithmetic, same as depth (a fold, not a reindex, because every path is frame-aligned).
- **Sample rate** — the only axis that changes what a *frame count means*. Everything downstream that measures time in source frames (loop lengths, RAM caps, cue marks, tempo sync) is coupled to it.
- **Container** (`.wav` vs headerless `.raw`) — not a format question at all; it is a compatibility obligation.

Consequence: **depth and channels can be unified with a byte-level decorator and no engine changes whatsoever. Rate is a separate, per-engine policy decision. Container is out of scope.**

## The principle: read wide, write narrow

The write side does **not** change:

- `WavStreamWriter` keeps emitting 48 kHz mono f32 for the tape / shuttle / softcut decks.
- `storage.cpp` / `card.cpp` keep writing granular's 48 kHz stereo loop buffer.

This is what makes the change safe:

- Existing cards keep round-tripping. Every file the firmware has ever written is still exactly what the firmware most wants to read.
- Shuttle's **bit-faithful varispeed replay** guarantee (`shuttle_engine.cpp:169`) survives untouched, because a 48 k f32 mono file still takes the `memcpy` fast path with zero conversion.
- Record→play round trips inside one session are byte-identical, so nothing in the tape/softcut overdub or save paths needs re-verifying.

A second, free consequence: **`LOFI_INT16` becomes a write-side-only flag.** Today it also silently re-gates what `WavStreamReader` will *accept* (`wav_stream.h`, via `kWavAudioFormat`/`kWavBitsPerSample`), which is precisely the "saved files mislabel float data as 16-bit PCM" trap that [`lofi-int16-scope.md`](lofi-int16-scope.md) warns not to ship into. Once the reader accepts both depths on every build, enabling `LOFI_INT16` can only affect what is written.

## What genuinely changes

### A. One parser (no behaviour change) — **DONE**

A `WavSource` in `src/memory/` that walks the chunk list once and yields:

```
{ audio_format, channels, sample_rate, bits_per_sample, data_start, data_size }
```

`WavStreamReader`, `RawStreamReader::begin_wav` and `card.cpp::init_read_audio` all consume it; each keeps its own accept/reject policy on top. This is pure de-duplication — same acceptance matrix in, same acceptance matrix out — and should land on its own commit so the format widening that follows has a clean diff.

Host tests already cover this surface (`host/test_stream.cpp` chunk-walk cases, `host/test_radio.cpp` A2 rejection cases, `host/test_wav_cues.cpp`); they should pass unmodified. That is the acceptance criterion for step A.

Landed as `src/memory/wav_source.h` (`WavInfo` + `parse_wav`, a template `walk_wav` over a `bool at(offset, dst, n)` source, with a `FileSrc`/`MemSrc` adapter each — no vtable, and it stays main-loop-only code). All three call sites now parse through it and keep only their own accept/reject policy:

| Call site | Now |
|---|---|
| `wav.h wav_header(bytes, size, ...)` | a shim that re-shapes `WavInfo` into the canonical 44-byte `WavHeader` |
| `WavStreamReader::begin` | `parse_wav(f, info)` + the mono / native-depth / 48 k gates |
| `RawStreamReader::begin_wav` | `parse_wav(f, info)` + the PCM/16/mono gates + the `filesize` clamp |
| `card.cpp init_read_audio` | `parse_wav(_buffer, bytesread, info)` + the stereo / 48 k / depth gates |

Outcome, all measured:

- **The whole host suite passes unmodified** — no test file was touched.
- **SRAM_EXEC went down**, not up: granular −8 B, tape −112 B, radio −112 B. Three copies of the walk collapsing to one more than pays for the two template instantiations, so the size risk flagged below did not materialise for step A. (It still applies to step B, which adds code rather than removing it.)
- Two deliberate deltas, both narrowing toward what the streaming readers already did:
  - `card.cpp` now bounds the walk by `bytesread` rather than `kChunk`. A file shorter than one 32 KB chunk previously let the walk step into whatever the *previous* read left in `_buffer`; it now fails cleanly instead of parsing stale bytes.
  - `card.cpp` inherits `WAVE_FORMAT_EXTENSIBLE` unwrapping (granular accepts one more legal file shape) and the `fmt`-must-precede-`data` rule (it rejects one pathological shape no tool emits).

### B. A converting `IChunkSource` decorator — **DONE**

The streaming path is byte-oriented by design (`audio_stream.h`: "the stream is format-agnostic"), which is exactly the seam this needs. Insert between the reader and `PlayStream`:

| Concern | Resolution |
|---|---|
| Where it runs | `StreamDeck::_pump` / `PlayStream::pump` — **main loop only**. Never the ISR; `play_consume` stays a ring drain. |
| Depth | Reuse `convert_pcm_block` (`pcm_convert.h`). Add 24-bit int and 8-bit unsigned — ~5 lines each in the existing loop, routed through the same normalized float. |
| Channels | Stereo → mono downmix (average) for the streaming engines; mono → stereo duplicate for granular's interleaved buffer. A fold, not a reindex. |
| Fast path | When file format == engine target, the decorator is bypassed entirely and the existing `memcpy` path runs. Today's files pay nothing. |
| Scratch sizing | Conversion **expands**: int16→f32 is 2:1, 24→32 is 4:3, and a mono→stereo duplicate is another 2:1. The shared scratch (`StreamDeck::Mem::scratch`, sized once for both decks) must either be read in smaller source chunks or staged through a second buffer. Size for the worst case: 8-bit mono → f32 stereo is 8:1. |
| Frame alignment | The decorator must consume whole *source* frames and emit whole *target* frames, or a short read straddles a sample. Keep the "always move whole frames" convention from `audio_stream.h`. |

Adding 24-bit is worth doing here rather than later: no engine accepts it today, and it is what DAWs and field recorders export by default.

Landed as `src/memory/converting_source.h` (`ConvertingSource`), plus the format vocabulary in `pcm_convert.h`: a `PcmFormat` tag (`u8`/`i16`/`i24`/`i32`/`f32`), `pcm_format_of()` mapping a header's `(AudioFormat, BitsPerSample)` onto it, per-sample `pcm_read1`/`pcm_write1`, and `convert_pcm_frames()` — the one place channel folding is defined, shared by the streaming decorator and the loop-buffer loader so "what a stereo 24-bit file sounds like" has a single answer on this device.

**The accepted matrix is now the same on both read paths:** WAV, any of the five PCM formats above, 1–8 channels, 48 kHz. Both paths still write exactly what they wrote before.

| Path | Engines | Adaptation |
|---|---|---|
| `WavStreamReader` → `ConvertingSource` → `PlayStream` | tape, shuttle, softcut | to native mono frames, in `StreamDeck::start_play` |
| `card.cpp` → `PcmLoader` | granular | to native **stereo** frames, on load |

Deviations from the plan above, both deliberate:

- **The shared scratch was not resized.** `ConvertingSource` carries its own 512-byte stage instead, so `StreamDeck::Mem::scratch` and the SDRAM rings are untouched and `PlayStream`'s read-ahead window is unchanged. The expansion-ratio hazard the table above worried about simply does not arise: the decorator sits *behind* `PlayStream`, so it is asked for destination bytes and pulls however many source bytes that needs.
- **`PcmLoader` grew a straddle carry.** `card.cpp` reads fixed 32 KB blocks, and its old comment ("kChunk is a multiple of both sample widths … no cross-chunk straddle") stops being true the moment a frame is 3 or 6 bytes. A partial trailing frame is now carried into the next chunk rather than converted as though it were whole.

Measured, `-O2`, SRAM_EXEC, against the pre-step-A baseline (so these deltas include step A's small win):

| Engine | Before | After | Δ |
|---|---|---|---|
| granular | 186,840 | 190,600 | +3,760 |
| tape | 173,208 | 178,168 | +4,960 |
| shuttle | 173,696 | 178,672 | +4,976 |
| softcut | 179,156 | 184,124 | +4,968 |
| radio | 159,736 | 164,688 | +4,952 |
| bard | 173,600 | 177,136 | +3,536 |
| pstretch | 164,360 | 169,312 | +4,952 |

`pstretch` is the one to watch — its SRAM_EXEC region is 200 KB rather than 260 KB, so it moved 80.25% → 82.67%.

**One bug found and fixed by the tests, worth recording:** the decorator's first version gave up when a single source read returned less than one whole frame, returning 0 bytes forever — a deck that plays silence with mounting underruns, on exactly the source shape FatFs-behind-a-ring produces. `IChunkSource` says a short read does not mean "stop"; only a zero-byte read does. The `DribbleSource` case in `host/test_stream.cpp` §11 exists to hold that line.

The scanned path's **`.wav`** side went through the same adapter afterwards, targeting the int16 mono frames radio/bard/pstretch consume. `RawStreamReader` now carries a per-instance source frame size instead of assuming 2 bytes, so `frames()` and `seek_to_frame()` stay in SOURCE frames whatever the file holds — which is what keeps the radio's free-running playhead (a frame index modulo the station length) meaningful across formats. `StreamDeck` gained one `_play_src(deck)` helper so start, re-open and seek can never disagree about whether the adapter is in the path, and `seek_play` drops the adapter's carry so a frame is never stitched out of bytes from both sides of a jump. `scan_bank`'s probe widens with it, so a stereo 24-bit file now enters the bank instead of being skipped.

Two things found while wiring it: `seek_to_frame` needed 64-bit offset math (a wide frame times a frame index near the 4 GB ceiling overflows a `uint32` product and wraps the seek back into the file), and `_play_src` had to be called after `raw_src` is set, not before.

Its **`.raw`** side should stay exactly as it is, and not only for RadioMusic compatibility. That path is built to stream files far larger than RAM: position is integer end to end (`_clock`, `L`, `start`, `offset` are `uint32` frames; `_phase` is a per-output-frame fraction that is decremented back, so nothing accumulates in a float), and `seek_to_frame`'s `frame * 2` is exact to 2^31 frames = 4 GB, which is FAT32's own file ceiling. The only float in the position math is the START knob's `fraction * L`, which past ~16.7M frames quantizes to multiples of `L / 2^24` — ~32 frames on a 1 GB station, inaudible on a coarse scrub control.

A decorator there would cost on every axis and buy nothing: a headerless file states no format to adapt *from*, the int16 body is what keeps the card reads cheap at that size, and radio re-seeks on every station change under CV — each one invalidating a carry the current path does not have to hold.

### C. Rate — **DONE** (resampled on the way in, not per playhead)

Mechanically this is nearly free. All three of the currently-48k-locked engines already read with a fractional interpolating playhead:

- `tape_engine.cpp:308` — `_phase[i] += _speed[i]`, linear interpolation between `_cur`/`_next`
- shuttle and softcut — the bipolar capstan maps (`speed_from_knob` / `rate_from_knob`)

So a rebase is one multiply on an existing step, exactly the pattern radio/bard/pstretch already ship (`radio_engine.cpp:285`, `bard_engine.cpp:295`, `pstretch_engine.cpp:451`):

```
step *= file_rate / 48000.f
```

All step B needs to provide is the header rate out of the reader — which `RawStreamReader::begin_wav` already reports and `WavStreamReader` currently throws away in favour of a reject.

**But this is the axis with semantic consequences**, and they are per-engine:

| Engine | What an off-rate file changes |
|---|---|
| tape | Little. Free-running, streamed, no length semantics beyond `loop_frames`. Loop modes `Faded`/`Fripp` use `L` in source frames, which stays self-consistent. |
| shuttle | The ~30 s RAM cap becomes a cap on **source** frames — a 96 k file gets ~15 s of material. The cap must be reported in seconds, not frames, or the truncation surprises. |
| softcut | Loop lengths and tempo sync drift against 48 k-recorded material in the same buffer. A loaded 44.1 k loop and an overdubbed take no longer share a time base. **This one needs a decision, not just code.** |
| bard | Already rate-aware; cue/mark frame math is rate-relative via `_src_rate[i]`. No change. |

Recommendation: ship A + B first with the 48 k gate intact, then take C per engine — tape and shuttle are straightforward, softcut deserves its own think.

**That recommendation was superseded by a better answer, and it dissolves softcut's problem.** Everything above assumes the rate change happens in each engine's *playhead*. It does not have to: put it in the **adapter** instead, and the ring receives frames at the device rate, so a loop length, a RAM cap and a tempo-synced buffer all still count 48 kHz frames whatever the file held. Nothing downstream sees a rate at all.

Concretely, no engine source changed for step C. `ConvertingSource` gained an optional resampler (linear interpolation — the same interpolation the engines' varispeed playheads use, so it is not a quality step down from rebasing one), and `StreamDeck::loop_frames` now reports the **output** frame count via `ConvertingSource::out_frames`. Every consumer of `loop_frames` — tape's loop-seam length, shuttle's and softcut's RAM-load targets — was already asking "how many frames will I receive", so they got the right answer for free.

That resolves each row of the table above:

| Engine | Resolution |
|---|---|
| tape | Nothing to do. `L` is the output length, `_src_pos` counts output frames. |
| shuttle | The ~30 s cap stays ~30 s of **wall clock**, because the buffer fills at the device rate. |
| softcut | **The decision evaporates.** The buffer is always in 48 kHz frames, so a loaded 44.1 kHz loop and an overdubbed take share a time base exactly as before. No drift, no sync change, no code. |
| bard | Untouched: the scanned path does NOT resample here, because those engines already rebase pitch from the header rate themselves. Doing both would correct it twice. |

granular came along too, for the same reason rather than a different one: its load path is push-driven (`PcmLoader::feed` is handed 32 KB blocks) instead of pull-driven, so it got the mirror-image resampler, but the outcome is identical — the buffer is filled at the device rate, so every frame↔tempo↔tick relationship [`lofi-path-b-scope.md`](lofi-path-b-scope.md) inventories is untouched. Rejection now only happens outside a 4–192 kHz sanity range, which exists so the ratio is not divided by a nonsense header.

Measured, SRAM_EXEC, on top of step B: granular +1,648 B, tape/shuttle/softcut ~+2,435 B, radio +2,440 B, bard +720 B, pstretch +2,448 B. pstretch is now at **84.67%** of its 200 KB region — still ~31 KB clear, but it is the number to watch if anything else lands there. radio, bard and pstretch again pay for a resampler they do not use, because they link `start_play` regardless.

Tooling followed: `ACCEPT_48K` became `ACCEPT_WAV` (there is no rate-gated accept rule left), and the fixture's 44.1 kHz tape file — the single most common way to get a card wrong — **moved from an error to a must-produce-no-finding**, with an out-of-range 1 kHz file added to keep the rejection branch covered.

### D. Container: keep `.raw`, never require it

Headerless `.raw` exists solely for RadioMusic card compatibility. Keep accepting it on radio; do not extend it anywhere; let all new content be WAV. It is also the one case that cannot be widened, because nothing in the file states its own format — which is exactly why the "reinterpreted as garbage" hazard in [`sd-card.md`](../sd-card.md) shrinks to `.raw` alone once A–C land, rather than disappearing.

### E. Tooling and docs — **DONE**

`scripts/card_layout.py` is the single source of truth the CLI, `make check-sdcard` and `web/` all read. The collapse there was not four specs becoming one — it was noticing that one dataclass had been doing **two jobs**, and that the widened read path pulled them apart:

- **`Accepts`** — what the firmware will LOAD. Two instances now cover all ten engines, split on the only axis that still differs: `ACCEPT_48K` (the engines with no resampler) and `ACCEPT_SCANNED` (the ones that already resample, so any rate works). `verify` predicts against this.
- **`Fmt`** — what `convert` WRITES, and what a README recommends. The four narrow native formats are unchanged, renamed `TARGET_*` to stop them reading as acceptance rules.

Everything downstream follows from the export: `card_audio.WavInfo.encoding` gained the wider vocabulary (mirroring `pcm_format_of`), `verify`'s findings changed from *"wrong format … plays as noise"* to *"this will not load (…)"*, and each folder's generated `README.TXT` now says both **ACCEPTS** and **BEST**. The web front-end consumes `accepts` from `card_layout.json` and its verify was updated to match; the two are pinned together by `web/test/fixtures/verify_cases.json`, regenerated from the Python.

The broken-card fixture is worth noting as a record of the change: the stereo-float and mono-int16 tape files it contains **moved from findings to must-produce-no-finding**, and a 64-bit-float file was added to keep the genuinely-undecodable branch covered.

Verified: `make test-scripts` (209 passed) and `make test-web` (284 passed), both green, with the firmware-parity tests in `test_sk_card.py` rewritten to grep the new gates (`pcm_format_of`, the channel bound, `kWavMaxChunks` in its new home) rather than the old fixed ones.

User-facing docs updated to match: [`docs/sd-card.md`](../sd-card.md) (its "four incompatible audio formats" opener, the ways-to-get-it-wrong table, and the per-engine table, which is now *accepts-is-shared, rate-and-best differ*) and [`preparing-audio.md`](../preparing-audio.md) (reframed from "exactly one format, anything else rejected" to "this is what loads with no conversion, and 44.1 kHz still does not").

## What does NOT change

Listed so effort is not spent here.

- **Write formats.** See [the principle](#the-principle-read-wide-write-narrow).
- **Granular's stereo loop buffer.** It is stereo interleaved and it is the persistence format for recorded loops. Unify *acceptance* (accept mono, duplicate it), not the on-disk layout.
- **`play_consume` / the ISR side.** All conversion is main-loop. The lock-free `SpscRing` contract, underrun accounting and `finished()` semantics are untouched.
- **`find_cue_points`** (`wav.h`). Already a standalone, order-independent, bounds-checked scan over the raw bytes; it needs the new parser's chunk walk no more than it needs the old one's.
- **Slot/scan directory rules.** The 12-character name limit, the 32 KB floor, the `.raw`/`.wav` extension filter and the leading-dot skip (`stream_deck.cpp::scan_bank`) are a separate class of failure from format and are not touched here.
- **Engine DSP.** With the decorator in place, every engine keeps receiving exactly the frame format it receives today.

## Risks

- **SRAM_EXEC.** The firmware links at ~89% SRAM_EXEC at `-O2` ([`softcut-impl.md`](softcut-impl.md)), and `card.cpp` already carries `#pragma GCC optimize("Os")` to claw space back. The decorator is small (<1 KB expected) but not free — take a size measurement on-target before step B lands, and `-Os` the new TU by default since it is main-loop-only code.
- **Scratch buffer growth.** The expansion ratios in step B are the one place this can quietly cost SDRAM or, worse, silently truncate a read. Cover it with a host test that runs every accepted format through the decorator at a chunk size that is *not* a multiple of the frame size.
- **Fewer rejects means fewer diagnostics.** Today a wrong-format file strobes amber (`tape_engine.cpp` `_err_fmt`). Widening acceptance is a net improvement — a converted file beats a rejected one — but the strobe stops being the signal that something is off-spec. Off-rate playback in particular becomes *audible* rather than *refused*, which is the argument for gating step C per engine.
- **Test surface.** `host/` covers the readers well. New cases needed: each depth × each channel count through the decorator, the non-frame-aligned chunk size above, and a fast-path assertion that a 48 k f32 mono file is still byte-identical end to end (the shuttle guarantee).

## Order of work

1. **A — one parser.** No behaviour change; existing host tests pass unmodified.
2. **B — converting decorator**, depth + channels, 48 k gate still in place. This is where the user-visible win lands: stereo and 16/24-bit files start working on every engine.
3. **C — rate**, per engine: tape and shuttle first, softcut on its own decision.
4. **E — tooling and docs** collapse to match what the firmware actually accepts.

Steps 1 and 2 are the high-value, low-risk core and are worth doing whether or not 3 ever happens.

## See also

- [`docs/sd-card.md`](../sd-card.md) — the user-facing rules, and the "nine layouts, four formats" statement this document is trying to retire
- [`docs/preparing-audio.md`](../preparing-audio.md) — the tape/shuttle format rationale and the ffmpeg/sox one-liners
- [`docs/dev/lofi-int16-scope.md`](lofi-int16-scope.md) — why `LOFI_INT16` must not ship until read and write agree
