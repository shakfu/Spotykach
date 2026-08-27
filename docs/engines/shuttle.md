# shuttle engine

`ENGINE=shuttle` · `src/engine/shuttle/shuttle_engine.{h,cpp}` · class `ShuttleEngine`

A **buffer-based bipolar/reverse varispeed tape** - the random-access counterpart to the SD-streaming [`tape`](tape.md) engine. Where `tape` streams forward-only from the card, `shuttle` holds audio in SDRAM, so the read pointer is random-access and **reverse**, **freeze**, and **loop windowing** are trivial. The trade is a finite, RAM-capped tape length (currently 30 s per track).

It runs **four independent mono tape tracks** - two per deck (A/B). All four free-run at their own speeds and sum into the stereo bus; you flip a deck's edit focus between its two tracks with the **Rev pad** (the edrums mechanism), and re-align all four to a common downbeat with the **Seq pad**.

The headline control is **PITCH as a bipolar capstan-speed knob**: noon stops the tape (silence), clockwise plays forward up to +2x, counter-clockwise plays in reverse down to -2x. Unity (+1x) lands off-centre, so the **Play pad snaps** the focused track to normal speed.

> Implementation, architecture, the file map, and dev notes live in [`docs/dev/shuttle-impl.md`](../dev/shuttle-impl.md).

---

## Controls

![Shuttle control surface](../media/shuttle-controls.svg)

*Generated from [`docs/diagrams/controls/shuttle.json`](../diagrams/controls/shuttle.json) via `make diagrams`.*

| Control | `ParamId` / pad | Scope | Effect |
|---|---|---|---|
| **PITCH** | `Speed` | per-track | bipolar capstan speed: noon = stop (silence), CW = forward to +2x, CCW = reverse to -2x |
| **POS** | `Pos` | per-track | loop window **start** (slides the loop through the recording) |
| **SIZE** | `Size` | per-track | loop window **length** (full buffer down to a short stutter) |
| **MIX** | `Mix` | per-track | track volume |
| **Alt + PITCH** | `Aux` | per-track | tape-slot select (8 files/deck under `/shuttle/`) -> loads the slot into RAM |
| **Alt + POS** | `AltPos` | per-deck | pan (equal-power) |
| **Mix fader** | `Crossfade` | global | deck A/B blend |
| **MOD_AMT** | `ModAmp` | per-track | tape-FX wow/flutter **depth** (see [Tape FX](#tape-fx)) |
| **MODFREQ** | `ModSpeed` | per-track | tape-FX wow/flutter **rate** |
| **grit + PITCH** | `GritIntensity` | per-track | tape-FX saturation **drive** |
| **grit + MIX** | `GritMix` | per-track | tape-FX saturation **character** |
| **flux + PITCH** | `FluxIntensity` | per-track | tape-FX filter **cutoff** (boots fully open) |
| **flux + MIX** | `FluxMix` | per-track | tape-FX filter **resonance** |
| **Play pad** | - | per-deck (focused) | toggle rolling; **snaps the track to unity (+1x)** on engage |
| **Rev pad** | - | per-deck | **swap** which of the deck's two tracks is focused (the other keeps playing) |
| **Alt + Play** | - | per-deck (focused) | record the focused track (play XOR record) |
| **Seq pad** | - | global | **re-align all four tracks** to their loop start (one atomic, declicked gesture) |

The four tracks free-run independently, so they drift out of phase (organic tape phasing) - the Seq pad pulls them back to a common downbeat. Because the absolute pots can't physically move, the Play->unity snap holds via the platform's pickup soft-takeover (`CapPitchPickup`): after a snap, the PITCH pot must be swept across unity before it retakes the speed.

## Loop window (POS / SIZE)

Each track plays a window `[start, start+length)` of its buffer, not always the whole recording:

- **SIZE** sets the length (full buffer down to a `kMinLoopFrames` floor - a sub-loop / stutter).

- **POS** slides the start across the part of the buffer the window doesn't cover, so the window always stays inside the recording. At SIZE = full, POS is inert.

The varispeed read pointer wraps within the window (both directions); the Seq re-align snaps to the window start.

## Audio source: record + load

- **Record** (Alt+Play) captures the live input into the focused track's buffer (overwrite from the start), monitored as it records. Deck A records input A, deck B records input B.

- **Load** (Alt+PITCH selects a slot) drains an existing `/shuttle/` WAV from the card into the track's RAM buffer over a few main-loop passes, then shuttles it like any other take. Reuses the platform's streaming service via the `SPK_USE_STREAM` capability flag (shared with the `tape` engine). A loaded file must be **mono 32-bit-float WAV at 48 kHz** (other formats are rejected); convert sources with [`scripts/convert_tape_audio.py`](../../scripts/convert_tape_audio.py) - pass `--engine shuttle` so it warns past the ~30 s RAM cap - or the one-liners in [`docs/preparing-audio.md`](../preparing-audio.md).

- **Boot preload.** On power-up, **all four tracks** are filled from the card if their files exist - track *t* of each deck loads slot *t*'s file (deck A: `tape_a_1/_2.wav` -> tracks 0/1; deck B: `tape_b_1/_2.wav`) - so a freshly powered shuttle is ready to jam without a manual load. (The RAM engine can't stream lazily the way the `tape` engine does, so it loads up front.) It is **per-track conditional** (a track whose slot file is absent is left empty) and, because a deck's two tracks share one SD stream, **serialized per deck** (the two decks load in parallel). Since the card mounts cooperatively early in boot, the load waits for the volume to come up, then is abandoned after a short deadline if there is no card / no files.

## Tape FX

Each track can run the same tape-FX kernel as the [`tape`](tape.md) engine - **wow/flutter** (a modulated fractional delay) -> **Jiles-Atherton hysteresis saturation** -> a **post-FX resonant low-pass** - shared verbatim from `src/engine/tape/tapefx.h`. The FX is **per-track** and addresses the focused track, like every other knob (it repoints on a Rev swap).

Because `shuttle` already uses POS/SIZE for the loop window (where the `tape` engine puts drive/character), the FX is driven from this engine's free controls - the two end-of-chain effect pads plus the MOD knobs:

- **grit pad = saturation:** hold **grit + PITCH** for drive (amount), **grit + MIX** for character (the J-A tone).

- **flux pad = filter:** hold **flux + PITCH** for cutoff, **flux + MIX** for resonance. Cutoff **boots fully open**, so the filter is inert until you sweep it down (the flux+PITCH pickup seeds open -> turn down to engage).

- **MOD_AMT / MODFREQ = wow/flutter** depth / rate.

**Neutral = bypassed.** With drive/character/wow at zero, cutoff fully open and resonance zero (the boot defaults), the kernel is **skipped entirely** rather than run flat: the wow/flutter delay line imposes a fixed ~25 ms delay even at zero depth and the filter is not bit-identity even fully open, so a "neutral" kernel would not be transparent. Skipping it keeps shuttle's varispeed playback **bit-faithful** when the FX is untouched, and confines the (non-trivial) Jiles-Atherton cost to tracks that are both rolling and actually using the FX. The summed bus is `SoftLimit`ed only while the FX is engaged (saturation + a resonant peak can overshoot 0 dBFS); with the FX off the bus stays a plain linear sum.

## Build / test

```sh
make -j8 ENGINE=shuttle        # build the firmware
make -C host test-shuttle      # run the headless test (or `make -C host test` for all engines)
```
