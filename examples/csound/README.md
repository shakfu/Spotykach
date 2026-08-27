# Csound engine example patches

Reference orchestras for the `csound` engine (`ENGINE=csound`). Each is a full, correctly-structured
CSD that compiles on the Daisy build.

## Use

Copy the files into a `csound/` folder at the **root of the SD card**, keeping the numbered names:

```text
<card>/csound/0.csd
<card>/csound/1.csd
```

Boot the unit (with the `csound` firmware flashed): it auto-loads the lowest-numbered patch. **Hold Alt
and turn PITCH** to scroll the selector (a dot per patch around the rings); **release** to switch live.
The centre mode LED is cyan for an SD patch, white for the built-in. MIDI NoteOn plays each patch's
`MidiNote` instrument over the top.

- `0.csd` — clean two-saw "fifth" chord (drone, knob-controlled; MIDI adds plucks over the top).
- `1.csd` — fat detuned super-saw with a sub-octave (drone; obviously different, for testing live switching).
- `2.csd` — **MIDI-driven, polyphonic**: the keyboard plays the main voice, no drone. Silent until you
  play. NoteOn-only means each note is a fixed ~0.6 s stab (chords work), not a hold-while-pressed pad.
  Voices accumulate into a global bus that an always-on instrument `tanh`-limits, so polyphony can't
  hard-clip `0dbfs` (the standard Csound pattern — a per-voice `outs` would sum to distortion). A
  `maxalloc` caps simultaneous voices so very fast playing drops notes instead of overrunning the CPU.
- `3.csd` — **resonant "acid" voice**: a saw through a self-oscillating Moog ladder filter
  (`moogladder`) with an LFO-swept cutoff (`lfo`). FEEDBACK = resonance, MODSPEED/MODAMP = sweep
  rate/depth — the squelchy character `vco2`+`tone` can't make.
- `4.csd` — **stereo reverb pad**: a soft detuned chord and the MIDI notes feed one shared bus that an
  always-on instrument runs through `reverbsc` (a real stereo reverb). SIZE = decay, ENV = damping,
  MIX = dry/wet. Shows a built-in effect + the bus-routing pattern (one effect over the whole patch).
- `5.csd` — **dub echo**: a self-plucking source + MIDI feed one feedback delay line (`delayr`/`delayw`
  with a movable `deltapi` tap). SIZE = echo time, FEEDBACK = repeats, MODAMP = repeat tone.
  Demonstrates delay lines + feedback.
- `6.csd` — **generative line**: a sample-and-hold random opcode (`randomh`) picks a new semitone on
  each `metro` tick and plays itself, no input. MODSPEED = note rate, MODAMP = range — Csound's
  algorithmic side. MIDI plays over the top.

All seven use only core, table-less opcodes (so they build the same way on the Daisy) and pass desktop
`csound --syntax-check-only`.

## Writing your own

Control channels the panel drives (read with `chnget`): `speedA` (PITCH), `mixA` (MIX), `sizeA` (SIZE),
`envA`, `fbA`, `modspA`, `modampA`, and the `B`-deck equivalents. Define `instr MidiNote` (p4 = freq Hz)
to be keyboard-playable.

Format rules that matter (the on-device CSD parser is line-oriented):

- Each section tag on its **own line** — `<CsScore>` and `</CsScore>` separately, never
  `<CsScore></CsScore>` on one line.
- Core opcodes only — no plugin opcodes, no soundfile I/O (`diskin`/`GEN01`); prefer table-less
  oscillators (`vco2`). Opcodes proven across these examples: `vco2`, `tone`, `moogladder`, `lfo`,
  `reverbsc`, `delayr`/`delayw`/`deltapi`, `randomh`, `metro`, `port`, `limit`, the `*seg`/`expon`
  envelopes, and `tanh`. A UTF-8 BOM and CRLF line endings are tolerated (the engine normalizes them).

See `docs/engines/csound.md` (user guide), `docs/dev/csound-impl.md` (internals), and the official orchestras in
`thirdparty/csound/Daisy/DaisyCsoundExamples/` for more.
