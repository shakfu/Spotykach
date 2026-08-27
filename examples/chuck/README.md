# ChucK engine example patches

Reference programs for the `chuck` engine (`ENGINE=chuck`). Each is a complete, self-contained `.ck`
program that compiles on the Daisy build and reads the panel knobs.

## Use

Copy the files into a `chuck/` folder at the **root of the SD card**, keeping the numbered names:

```text
<card>/chuck/0.ck
<card>/chuck/1.ck
<card>/chuck/2.ck
<card>/chuck/3.ck
```

Boot the unit (with the `chuck` firmware flashed): it auto-loads the lowest-numbered patch. **Hold Alt
and turn PITCH** to scroll the selector (a dot per patch around the rings); **release** to switch live
(the engine swaps the patch inside one persistent VM — compiled the first time it's selected, then cached,
so later switches are instant and memory stays flat). The centre mode LED is cyan for an SD patch, white
for the built-in.

- `0.ck` — clean two-saw "fifth" drone (calm baseline, knob-controlled).
- `1.ck` — fat detuned super-saw with a sub-octave (obviously different, for testing live switching).
- `2.ck` — **concurrent generative pad**: several voice shreds play themselves in polyrhythm. The patch
  that shows off ChucK's strongly-timed concurrency; on a `METER=1` build ring B (the shred count) shows
  several dots instead of one.
- `3.ck` — STK **Rhodey** auto-arpeggio, showing the STK instruments are available, not just the raw
  oscillators.

All four are self-running (sound immediately, no MIDI). MIDI-driven patches arrive with M4.

## Writing your own

Globals the panel drives (declare and read them): `speedA` (PITCH), `mixA` (MIX), `sizeA` (SIZE),
`envA`, `fbA`, `modspA`, `modampA`, and the `B`-deck equivalents. A program reads whichever globals it
declares; the rest are ignored. The convention across these patches is PITCH = pitch/tempo, SIZE =
brightness, MIX = level - keep to it so the knobs feel consistent when you switch patches.

What is and isn't available (this is a bare-metal, core-only ChucK build):

- **Available:** the oscillators (`SinOsc`/`SawOsc`/`SqrOsc`/`TriOsc`/`PulseOsc`/`Noise`), filters
  (`LPF`/`HPF`/`BPF`/`ResonZ`/...), envelopes (`ADSR`/`Envelope`), `Gain`, delays, the reverbs
  (`JCRev`/`NRev`/`PRCRev`), the FFT/analysis UANAs, and the **STK instruments** (`Rhodey`, `Wurley`,
  `TubeBell`, `Mandolin`, `Moog`, ...). `Math`/`Std`, sporked shreds, events, and global variables.
- **Not available:** chugins (no dynamic loading), sound-file I/O (`SndBuf`/`WvIn`/`WvOut` - no
  filesystem audio), and MIDI/OSC/HID/serial device UGens (the host owns I/O).

Keep an eye on CPU: every concurrent shred and UGen costs time inside one audio block. The `METER=1`
build's ring A (CPU load) tells you how close you are to the budget - stay out of the red. A UTF-8 BOM
and CRLF line endings are tolerated (the engine normalizes them); an empty or whitespace-only file
falls back to the built-in.

See `docs/dev/chuck-impl.md` (internals/roadmap) and `docs/dev/chuck-pod-poc.md` (bring-up + the meter).
