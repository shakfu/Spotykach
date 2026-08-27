# softcut

A dual-deck crossfaded **overdub looper** built on monome's [softcut-lib](https://github.com/monome/softcut-lib). Each deck holds two softcut voices (a Rev-swapped pair), so there are four voices in all. Unlike the buffer-tape `shuttle`/`tape` engines, softcut's read/write head plays and records the same loop at once, with subsample-accurate click-free loop crossfades and **interpolated overdub** - layering live input onto a running loop with a feedback control. That sound-on-sound overdub is the reason to reach for softcut over the existing tape engines.

Four voices is the hardware-measured safe CPU budget (worst case ~62% avg / 79% peak). See [`docs/dev/softcut-spike.md`](../dev/softcut-spike.md) for the feasibility measurements and the path to a future 6-voice build.

## Controls (focused track)

Each deck edits one **focused** voice at a time; the Rev pad swaps which one, and the platform reseeds the pots so the absolute knobs catch the newly-focused voice without a jump. Both voices of a deck keep sounding.

| Control | Function |
|---|---|
| PITCH | rate, **bipolar** about noon: noon = stop, CW = forward to +2x, CCW = reverse to -2x |
| POS | loop start (slides the loop window through the buffer) |
| SIZE | loop length (a short stutter up to the whole buffer) |
| MIX | track volume |
| ENV | **overdub feedback**: 1.0 = infinite sound-on-sound layering, lower = old layers decay as you add, 0 = overwrite (only acts while overdub is armed) |
| Alt+POS | pan |
| Alt+PITCH | SD loop slot - loads a clip into the loop buffer (boot-preloads slot 1-2 per deck) |
| MOD_AMT | loop crossfade time - short = tight, long = lush/smeared loops |
| MODFREQ | rate slew time (glide on varispeed sweeps) |
| FLUX pad + PITCH | post-filter cutoff (boots open; sweep down to engage) |
| FLUX pad + MIX | post-filter resonance |

## Pads

- **Play** - roll / stop the focused loop (snaps rate to unity on engage).

- **Alt+Play** - the record gesture, context-sensitive on whether the voice has content:

  - **Empty voice (looper record):** first press starts recording a **fresh loop** (the buffer is cleared first); the second press **closes the loop at exactly the length you played** and starts looping it. Further presses overdub.

  - **Voice with content (SD-loaded or already recorded):** arms / disarms **overdub** on top (sound-on-sound).

- **Rev** - swap the deck's focused voice.

- **Seq** - realign **all** voices to their loop start at once: softcut's `cutToPos` does a click-free crossfaded jump, so drifted free-running loops snap back to a common downbeat (the voice-sync gesture).

- **Alt+Seq (tap)** - **save the full take** (the whole recording, lossless) to the focused voice's currently-selected SD slot.

- **Alt+Rev** - **save the trimmed loop** (exactly the POS/SIZE window - "bounce what you hear") to the same slot.

- **Alt+Seq (hold ~1.5 s)** - **erase** the focused voice (clean discard - nothing is written). It empties the buffer so the next Alt+Play records a fresh take. Holding cancels the tap's pending save, so an erase never touches the card.

Both saves write a float32-mono WAV; pick the destination slot with Alt+PITCH first (use an empty slot so you don't overwrite a boot loop). The ring flashes amber while it writes (~1 s, non-blocking - the loop keeps playing). Save and load share the same slot files, so a captured loop reloads on the next boot or via Alt+PITCH. Alt+Seq is the safe everyday save (SIZE/POS stay non-destructive); Alt+Rev prints exactly the loop you hear. Note the Alt+Seq save commits a moment (~1.5 s) after the tap so a hold can turn it into an erase instead; Alt+Rev saves immediately.

The audio input is summed to mono (both input jacks), so a loop records whichever input the source is patched to.

## Routing switch (L / C / R)

Per-voice pan into the stereo bus: **LEFT** (DoubleMono) manual Alt+POS pan; **CENTRE** (Stereo) auto-spread a deck's two voices hard L/R; **RIGHT** (GenerativeStereo) random pans, re-rolled on entry.

## Make a loop

1. On an empty voice, **Alt+Play** to start recording, play your phrase, **Alt+Play** again to close the loop - it now repeats at the length you played.

2. **Alt+Play** once more to overdub. Keep layering (ENV high) or let old layers fade as you add (ENV lower).

3. PITCH for varispeed/reverse, the FLUX pad to filter, Seq to re-sync against the other deck.

Each voice's buffer is **~10.9 s** (2^19 frames @ 48 kHz, 2 MB), so a single take can be up to ~10.9 s at the default 1x record speed.

## Build / flash

```text
make ENGINE=softcut            # normal SRAM build (-O2, ~89% SRAM_EXEC)
make ENGINE=softcut METER=1    # + on-device CPU load meter over USB serial / ring A
make program-dfu               # flash (enter DFU first)
```

Loop clips are **32-bit float, mono, 48 kHz** WAVs at `softcut/loop_<a|b>_<1..8>.wav` on the SD card - the same format Alt+Seq/Alt+Rev save, so saved loops reload directly.

## Not in v1

- **Phase-quantised voice sync** - the Seq-pad realign covers the musical case; locked phase-quant is a later add.

- **6 voices** - gated on the `std::function`-per-sample removal in the vendored core (see the spike doc).
