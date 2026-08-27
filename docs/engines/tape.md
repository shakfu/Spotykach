# tape engine

`ENGINE=tape` · `src/engine/tape/tape_engine.{h,cpp}` · class `TapeEngine`

A **dual streaming tape deck**: two independent **mono** decks (A/B), each playing or recording its own arbitrarily long file on the SD card, removing the in-SDRAM loop-length cap that bounds the other engines (~42 s float / ~84 s int16, `kSourceMaxSeconds` in `src/config.h`). A deck is **play-XOR-record** (no overdub), so the two run like a pair of record decks - play deck A while recording deck B, then play both together and beat-match by ear with each deck's PITCH. It is a **linear** player/recorder, not a granular scrubber - random SD seek cannot meet the 2 ms audio deadline, so each tape only ever streams forward.

> Implementation, architecture, the file map, and the bug/bring-up writeups live in > [`docs/dev/tape-impl.md`](../dev/tape-impl.md).

---

## Audio I/O and routing

The hardware has two **mono** inputs (A, B; input A is normalled to B when B is unpatched), two mono outputs, and a stereo headphone monitor. The tape engine maps them as two independent decks blended to a stereo bus:

- **Inputs:** deck A records **input A** (`in[0]`), deck B records **input B** (`in[1]`). Independent - never summed.

- **Outputs:** the two decks are mixed to a stereo bus (`out[0]` = L, `out[1]` = R) that drives the headphone; the individual jacks tap the same bus.

Three knobs/controls place the decks in that bus:

| Control | `ParamId` / config | Effect |
|---|---|---|
| **Routing switch** | `ConfigId::Route` | topology (see below); also lights the L/C/R mode LED |
| **Alt + POS** (per deck) | `AltPos` | per-deck **pan** (equal-power, 0 = L, 0.5 = C, 1 = R) |
| **MIX knob** (per deck) | `Mix` | per-deck **playback volume** |
| **Mix fader** | `Crossfade` | **A/B blend** (DJ-style: centre = both full, ends = one deck only) |

(Bare **POS** is the tape-FX **saturation drive** — see the Controls table below and [Tape FX](#tape-fx). It was reserved for a future loop-start control when this section was written, which is no longer true: `tape_engine.cpp:127` routes it to `_fx_n[0]`.)

The routing switch (mirrors the panel L/C/R, granular's int convention):

- **LEFT (`DoubleMono`):** each deck panned by **its own Alt+POS knob**.

- **CENTRE (`Stereo`):** both decks centered (summed equally to both outputs); POS ignored.

- **RIGHT (`GenerativeStereo`):** each deck at a random pan position (re-rolled on entering the mode).

Total per-deck gain into the bus = **MIX volume x mix-fader blend x pan(L/R)**.

---

## Controls

![Tape control surface](../media/tape-controls.svg)

*Generated from [`docs/diagrams/controls/tape.json`](../diagrams/controls/tape.json) via `make diagrams`.*

| Control | Action |
|---|---|
| **Play pad** (per deck) | play toggle |
| **Alt + Play pad** (per deck) | record toggle |
| **PITCH** (per deck, `Speed`) | varispeed playback (`exp2((v-0.5)*2)` -> 0.5x..2x, +/-1 octave, pitch+speed linked) |
| **Alt + PITCH** (per deck, `Aux`) | tape-slot select (visual selector while held, see below) |
| **Alt + POS** (per deck, `AltPos`) | pan (LEFT routing) |
| **MIX** (per deck, `Mix`) | playback volume |
| **ENV** (per deck, `Env`) | loop mode (4 quadrants, see below) |
| **Mix fader** (`Crossfade`) | A/B blend |
| **Routing switch** (`Route`) | pan topology |
| **POS** (per deck, `Pos`) | tape FX: **saturation drive** (see Tape FX) |
| **SIZE** (per deck, `Size`) | tape FX: **character** |
| **MOD_AMT** (per deck, `ModAmp`) | tape FX: **wow/flutter depth** |
| **MODFREQ** (per deck, `ModSpeed`) | tape FX: **wow/flutter rate** |
| **Hold grit + PITCH** (per deck, `GritIntensity`) | tape FX: **filter cutoff** (low-pass) |
| **Hold grit + MIX** (per deck, `GritMix`) | tape FX: **filter resonance** |

- Play and record are **mutually exclusive per deck**; each deck has 8 slot files under `/tapes/` (see "Tape slots" below). Control routes through `on_play_pad` (play) / `on_record_pad` (Alt+Play); the **Rev pad is inert** (reserved for future reverse playback). A 300 ms same-deck debounce guards capacitive-pad glitches.

- **Display:** per deck, idle = ring **off**; **bright green** = playing, **bright red** = recording; a rejected start (no file / card not mounted) flashes that ring **amber** ~1.2 s. Status also rides the **Play-pad LED** (the pad you press). The routing switch position shows on the **mode L/C/R** indicator. The live input is **monitored** to the deck's channel while recording.

`capabilities() = CapOwnDisplay | CapDualDeck | CapAux | CapAltPos`. `CapAux` claims Alt+PITCH (tape-slot select); `CapAltPos` claims Alt+POS (pan) - both are platform-gated remaps, so non-tape engines are unaffected. It does **not** advertise `CapTapeStorage` - the tape engine owns its own SD streaming, so the platform `Storage` service stays out of the way (it mounts the card at boot and, in the tape build, leaves it mounted for the stream).

## Tape slots

Each deck has **8 slots**, files `/tapes/tape_a_1.wav` … `/tapes/tape_a_8.wav` (and `tape_b_`), so takes are non-destructive and recallable rather than overwriting one fixed file. **Alt+PITCH** selects the active slot; while Alt is held, the deck's ring shows the **8 slots as evenly-spaced dots with the selected one bright** (the same `set_aux_active` selector seam reso uses for its model picker). Selecting a slot sets the target for the next Play / record - it does not interrupt a deck already playing. Record writes the selected slot (overwriting only that one); Play reads it (amber if empty). The selector shows **recorded vs empty** slots (selected bright / recorded mid / empty dim). The `/tapes/` directory is created on first record. Single-digit slot numbers keep the names 8.3-safe.

To load **your own** audio into a slot, the file must be **mono 32-bit-float WAV at 48 kHz** - the engine does no on-device conversion, and a wrong-format file (16-bit / 32-bit-int / stereo / non-48k) is rejected with a strobing amber error LED. Convert source files with [`scripts/convert_tape_audio.py`](../../scripts/convert_tape_audio.py) or the ffmpeg/sox one-liners in [`docs/preparing-audio.md`](../preparing-audio.md).

---

## Loop modes (ENV knob, per deck)

The ENV knob picks one of four loop behaviors by quadrant, from fully CCW:

| ENV | Mode | Behavior |
|---|---|---|
| `< 0.25` | **None** | play once, stop at end |
| `< 0.5` | **Plain loop** | seamless repeat at full level |
| `< 0.75` | **Faded loop** | repeat with a ~50 ms fade across the seam (de-click) |
| `>= 0.75` | **Frippertronics** | each pass ~0.6x quieter; fades out over ~8 passes, then auto-stops |

Loops are the *recorded take's* length (free-run, not tempo-aligned).

### Varispeed

Playback is resampled in the ISR by a 2-frame linear interpolator: the engine advances `_speed` source frames per output frame and reads fractionally between the two, per deck. Pitch and speed move together (tape-style) - there is no time-stretch. Read-ahead and the SD pump scale with consumption automatically, so faster playback simply drains the ring faster.

---

## Tape FX

Each deck has its own **analog-tape effect chain** on the playback signal: **wow/flutter -> Jiles-Atherton hysteresis/saturation -> resonant low-pass**, with the summed two-deck bus soft-limited so resonant peaks and two decks can't clip. **POS** = saturation drive, **SIZE** = character, **MOD_AMT** = wow/flutter depth, **MODFREQ** = wow/flutter rate; the low-pass rides the grit-modifier pad - hold **grit + PITCH** for cutoff, **grit + MIX** for resonance (cutoff boots open, so sweep it down to engage). The DSP/licensing/CPU details are in [`docs/dev/tape-impl.md`](../dev/tape-impl.md).

---

## Build / flash / test

```text
make faust-kernels                                # regenerate the tape-FX kernel (needs the .venv cyfaust)
make engine-tape                                  # clean + build + DFU flash (Make path)
make -f Makefile.cmake ENGINE=tape program-dfu    # CMake path
make -j8 ENGINE=tape                              # build only (~86.9% SRAM_EXEC, incl. tape FX)
make -C host test                                 # host suites incl. test-tape / test-stream (all green)
```

On hardware, after ~1 s for the SD to mount: pick a slot per deck with **Alt+PITCH** (ring shows the selector), then **Alt+Play(A)** records input A and **Alt+Play(B)** records input B into the selected slots; **Play(A)** / **Play(B)** play them back simultaneously (A->L, B->R). Turn each deck's **PITCH** for varispeed, **Alt+POS** to pan (LEFT routing), **MIX** for volume, **ENV** for the loop mode; the **mix fader** blends A/B and the **routing switch** picks the pan topology. Files land under **`/tapes/`** as `tape_a_<n>.wav` / `tape_b_<n>.wav`. </content>
