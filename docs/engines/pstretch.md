# pstretch engine

`ENGINE=pstretch` · `src/engine/pstretch/pstretch_engine.{h,cpp}` · class `PstretchEngine`

A **real-time PaulStretch** ambient time-smear. PaulStretch (Nasca Octavian Paul) turns audio into a diffuse, evolving spectral wash by FFT-ing large overlapping windows, **randomizing the phases** (keeping the magnitudes), and overlapping back to time. Advancing the analysis read head slowly - or freezing it - stretches and smears time without limit.

Each deck (A/B) runs an independent stretcher over its input channel (A = left, B = right, like the delay engine); the two are blended by the crossfader and placed by the routing switch. This is a **clean-room** reimplementation written from the published algorithm description (not derived from the GPL PaulStretch / PaulXStretch sources), so it stays **MIT**, and it is **self-contained** - a small vendored FFT, no external DSP library.

> Implementation, the source modes (live / capture / SD-file), and the design notes live in [`docs/dev/pstretch-impl.md`](../dev/pstretch-impl.md).

---

## Controls (per deck)

![Pstretch control surface](../media/pstretch-controls.svg)

*Generated from [`docs/diagrams/controls/pstretch.json`](../diagrams/controls/pstretch.json) via `make diagrams`.*

| Control | `ParamId` / config | Effect |
|---|---|---|
| **SIZE** (`Size`) | | **STRETCH** amount, 1x..64x (exponential) - how slowly the read head crawls. More = more smear. |
| **POS** (`Pos`) | | **DIFFUSION** - 0 = clean window resynthesis, 1 = full phase-randomized PaulStretch wash. |
| **PITCH** (`Speed`) | | **pitch shift** of the grain, +/- 1 octave. |
| **ENV** (`Env`) | | **tone** - a one-pole low-pass to soften the wash (open at max). |
| **MIX** (`Mix`) | | **dry/wet**. |
| **Mode switch** (`Mode`) | | **SOURCE** select (per deck). The 3-position switch is silkscreened (top to bottom) **Reel / Slice / Drift** - legacy labels from the stock granular firmware; here they map **Reel = Capture, Slice = Live, Drift = SD-file** (stream a clip from `/pstretch`). See below. |
| **Play pad** | | **FREEZE** the read head - an infinite, evolving drone on the current spot. Works in any source. |
| **Rev pad** | | **RE-GRAB** the ring at the current instant - refreshes the Capture snapshot (Capture mode only; no-op in Live/SD). |
| **Alt+PITCH** (`Aux`) | | **CLIP select** (SD source) - pick which clip in `/pstretch` this deck streams. Takes effect live if the deck is already streaming, else on the next switch to SD. Hold Alt to show the selector. |
| **Alt+POS** (`AltPos`) | | **SCRUB** (SD source) - seek the stream playhead to a position in the clip (debounced, so a sweep opens where you land; works while frozen to audition spots). |
| **Cycle** (`ModSpeed`) | | **MOD rate** - the per-deck LFO speed (~0.03..8 Hz). **Alt+Cycle** locks the rate to the tempo (musical divisions). |
| **Glow** (`ModAmp`) | | **MOD depth** - 0 = modulation off (the engine is un-modulated until you raise this). |
| **Mod Type switch** (`ModType`/`LfoShape`) | | LFO **shape** (sine / triangle) or **Follow** (input-envelope follower). |
| **Size/Pos mod switch** (`StartModOn`/`SizeModOn`) | | MOD **target**: Pos = diffusion, Size = stretch, both = tone (pitch is modulated via the V/Oct CV jack). |
| **CV in** (`cv_voct`/`cv_size_pos`/`cv_mix`/`cv_crossfade`) | | additive CV: **V/Oct** -> pitch, **Size/Pos** -> stretch, **Mix** -> dry/wet, **Crossfade** -> A/B blend. |
| **Gate in** | | in Capture, **re-grab** the ring; in Live/SD, **toggle freeze** - rhythmic re-sampling / stutter from a clock. |
| **Mod CV out** (`process_cv`) | | each deck's **LFO** as a 0..1 CV (free-runs as a usable modulation source even at Glow=0). |
| **Gate out** (`gate_out_triggered`) | | a pulse on every **LFO cycle** - a tempo-synced clock/reset out when the LFO is clock-synced. |
| **Mix fader** (`Crossfade`) | | A/B blend of the two decks. |
| **Routing switch** (`Route`) | | stereo topology (below). |

### Source modes - what the stretch acts on

The stretch is orthogonal to its **source** - the analysis ring is just "what's being read," and the per-deck **Mode switch** selects what fills it. That switch is a 3-position toggle silkscreened (top to bottom) **Reel / Slice / Drift**; those are legacy labels from the stock granular firmware and do *not* describe pstretch's use. Here they select the source:

| Switch position | Label | Source |
|---|---|---|
| top | Reel | **Capture** |
| middle | Slice | **Live** |
| bottom | Drift | **SD-file** |

Live and Capture are pure live effects (no files); SD streams a clip. All three respond to SIZE / PITCH / DIFFUSION / ENV / MIX and to **Freeze** identically. The two decks are independent, so you can run one Live while the other streams an SD clip and blend them on the crossfader.

- **Live (Slice, middle position).** The read head trails the incoming sound. Because real-time stretching cannot stretch the literal present (the output would fall infinitely behind), at high **SIZE** the head holds near the trailing edge and you hear an evolving, smeared wash of the **recent past** - a few seconds back - rather than the instantaneous input. This is the ambient "blur the room" mode.

- **Capture (Reel, top position).** On entering the mode it grabs the last ~5 seconds of input, stops listening, and loops the read head *through* that captured span - so a big **SIZE** slowly traverses the whole captured phrase. This is the classic PaulStretch "stretch a short sound into a long drone." The **Rev pad** re-grabs a fresh snapshot at the current instant.

- **SD-file (Drift, bottom position).** Streams a clip from the `/pstretch` folder on the SD card *through* the stretch, so you can smear a whole recording into an hour-long drone (see below). **Alt+PITCH** picks the clip, **Alt+POS** scrubs the playhead.

- **Freeze (Play pad).** Orthogonal to all three: pins the read head so the same spectral region keeps re-randomizing into an endless evolving pad. This is how you drone on *this* moment (or a scrubbed SD spot).

### SD-file source

Put 16-bit mono `.wav` (or `.raw`, assumed 48 kHz) clips in a flat `/pstretch` folder on the card (8.3 names, at least ~32 KB each, up to 32 clips); the folder is scanned once and sorted **alphabetically**, so **Alt+PITCH** position N is the Nth clip by name (zero-pad any numeric names). Both decks stream independently.

Because the stretch makes the read head crawl (it consumes the source at `input_rate / stretch`, ~1 KB/s at 50x), the clip is **streamed** slowly from the card - never fully loaded into RAM - so an arbitrarily long file plays for hours. Off-rate clips (e.g. 44.1 kHz) are pitch-corrected from the `.wav` header, so they play at native pitch rather than sharp. The card mounts a moment after boot and the engine keeps rescanning until a clip streams, so a slow or late-inserted card self-heals; if SD is selected but the folder is empty/unreadable the pad LED turns **red** (vs magenta when streaming) as a diagnostic.

### Modulation, CV, and gate

The engine self-animates and patches into a modular rig. All of this is **per deck** and independent of the source (Live / Capture / SD), and everything is **off by default** - the un-modulated engine is unchanged until you raise **Glow** or patch a jack.

- **Mod LFO (Cycle / Glow).** A free-running LFO whose **rate** is **Cycle** (~0.03..8 Hz, exponential) and **depth** is **Glow** (0 = off). The **Size/Pos mod switch** picks the **target**: Pos = **diffusion** (breathing clean↔wash), Size = **stretch** (a slow time-wobble), both = **tone** (a slow filter sweep). The **Mod Type switch** picks the LFO **shape** (sine / triangle) or **Follow**, which replaces the LFO with an **envelope follower** on the deck's live input so the sound's own dynamics drive the target. Ranges are deliberately gentle for an ambient wash. (Modulation is applied once per audio block - ample for the sub-10 Hz rates.) **Clock sync:** hold **Alt while turning Cycle** to lock the LFO rate to the transport tempo - the knob then selects a musical division (a cycle every few bars up to several per beat), tracked live as the tempo changes, and the **Cycle LED** lights in the clock-source colour. A plain Cycle turn returns to free-running Hz. **Pitch** is not on the switch because it already has its own modulation route - the **V/Oct CV** jack (patch an external LFO for vibrato/detune); diffusion and tone have no CV jack, so the internal LFO is their only route.

- **CV inputs (additive).** The CV jacks sum on top of the knobs (calibrated to ~0 when unpatched, so the knob alone rules with nothing patched): **V/Oct** -> pitch (1 V/oct), **Size/Pos CV** -> stretch, **Mix CV** -> dry/wet (per deck), and **Crossfade CV** -> A/B blend (global). (A negative Mix CV can null the wet to a dry passthrough.)

- **Gate in.** A rising edge **re-grabs** the ring in **Capture** (rhythmic re-sampling of the input from a clock), or **toggles freeze** in **Live / SD** (rhythmic hold / stutter). It shares the freeze state with the Play pad.

- **CV / gate out.** pstretch is also a CV *source*: the **Mod CV out** emits each deck's LFO as a 0..1 control voltage, and the **gate out** fires a pulse on every LFO cycle. The LFO free-runs from Cycle/Mod-Type/Alt+Cycle regardless of the internal depth (Glow), so you can use pstretch as a plain **LFO / clock generator** (tempo-synced when clock-synced) to drive other modules even when it isn't modulating itself.

### Routing / stereo image

- **LEFT (DoubleMono):** deck A hard-left, deck B hard-right (independent L/R smear).

- **CENTRE (Stereo):** both centred.

- **RIGHT (GenerativeStereo):** each deck at a random pan (re-rolled on entering the mode).

### Display

Per deck the play LED and a ring marker show the stretch amount and the state colour: **green** = live, **amber** = capture (looping a grab), **magenta** = SD (streaming a clip), **red** = SD selected but no clips found, **cyan** = frozen (takes priority). While Alt is held on a streaming SD deck, the ring shows a **dot per clip** with the selected one bright, instead of the stretch marker. The routing switch lights the mode L/C/R indicator.

---

## Notes

- **Three sources, one stretch.** Live and Capture need no SD card or prep; the SD-file source (Mode R) streams a clip from `/pstretch`. The per-deck input rings live in the SDRAM arena, but the FFT working set is in on-chip SRAM (see the dev notes - scattered SDRAM access is why the first build was unusable).

- **Delayed smear** (live mode) is intentional and inherent to real-time stretching - use **Freeze**, **Capture**, or the **SD-file** source to play *through* a fixed moment / phrase / clip instead of trailing the input.

- **Window is 8192 samples (~171 ms)** by default - a lush, smooth wash. A lighter **4096-sample (~85 ms)** window is available with `make ENGINE=pstretch WINDOW=4096`. Both are now measured on hardware (below); if you ever hear underruns, drop to `WINDOW=4096` or raise `kWorkBudget`.

- **CPU + memory, measured on hardware 2026-08-01.** Taken over the terminal channel (`reset cpu` -> sample at 1 s / 3 s / 6 s -> `query cpu`/`cpumin`/`cpumax`), both builds `TERMINAL=1`, engine passive rather than driven hard. The 4096 figures confirm the estimate this doc previously carried (~32% / ~64%); the 8192 default had never had a number taken.

  | | WINDOW=8192 | WINDOW=4096 |
  |---|---|---|
  | CPU avg | 41.5% | **33.3%** |
  | CPU max | **87.6 -> 88.2 -> 91.3%** | **63.47 -> 63.57 -> 63.76%** |
  | CPU min | 2.00% | 1.93% |
  | `SRAM` (312K) | 97.40% | **82.01%** |
  | `SRAM_EXEC` (200K) | 94.38% | 94.35% |
  | `make test-hw` | 30 passed | 30 passed |

  **Read the max column, not the average - and read it as a sequence.** At 8192 the peak was still CLIMBING through the sample (87.6 -> 91.3, +3.7 pp), i.e. it had not converged and 91.3% is *not* the ceiling, just the worst block seen in six passive seconds. At 4096 it converged immediately (+0.3 pp drift) and reads like an actual bound. So the real 8192 headroom is smaller than "100 - 91" suggests and is not yet known; driving both decks hard will push it further. Neither figure is a worst case - both were measured passive.

- **Why the window choice is not purely a voicing question here.** pstretch is the one engine large in *both* halves of the AXI SRAM split, which is why it needs its own linker script (`linker/alt_sram_pstretch.lds`). 4096 is the only configuration with real margin on both axes at once - ~36% CPU headroom and ~50 KB data slack, against ~9% and ~8 KB at 8192. That is an argument for 4096 as the default, but it is not a decision the numbers can make: 4096 is a shorter, snappier smear (~85 ms vs ~171 ms), and the long wash may be the point of the engine. **The default remains 8192 pending a listening call.**

- **Soft start.** Entering SD mode (and each scrub re-seek) briefly primes the ring - expect ~170 ms of soft/near-silent output; benign for an ambient scrub.

---

## Build / flash

```text
make -j8 ENGINE=pstretch      # build (8192 window; WINDOW=4096 for the lighter build)
                              # uses linker/alt_sram_pstretch.lds automatically - pstretch does NOT
                              # link against the default 300K/212K split (see that file's header)
make ENGINE=pstretch program-dfu
make engine-pstretch          # one-shot: clean + build + flash (device in DFU mode)
make -C host test             # host suites incl. test-pstretch
```
