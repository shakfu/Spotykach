# Spotykach Manual (firmware-tracked)

This is the in-repo user manual. Unlike the published manual, it is kept in sync with the firmware in this repository: when you change behaviour, update this file in the same commit. Where this document and the published manual (<https://tsemah.notion.site/Spotykach-Manual-22c6331933b880c59108c0de25102bb5>) disagree, this document describes what the current code actually does. Differences known at the time of writing are called out in [Notes vs published manual](#notes-vs-published-manual).

**Scope.** This manual describes the **granular** engine — the stock upstream firmware, and this repo's `ENGINE=granular` default. It doubles as the platform manual, because everything outside the "Decks and modes", "Recording and playback", "Primary controls" and "Effects" sections (power, clock/sync, CV/gate, routing, the card, `config.txt`, MIDI, flashing) is shared by every engine. For the other twenty engines, each of which replaces the knob/pad meanings with its own, see [`docs/engines/`](engines/).

Spotykach is a screenless, dual-deck looping and sampling instrument built on an Electro-Smith Daisy Seed. It runs at 48 kHz. The two decks (A and B) are independent recorders/players that share a clock, FX engine families, modulation, routing and SD-card storage.

## Power

- USB-C (5 V, 1 A) or 15 V barrel jack (1 A, center-positive, 5.5 mm / 2.5 mm). Both may be connected at once.

- There is no power switch.

- A ground loop via computer USB can cause high-pitched noise; a standalone USB adapter avoids it.

## Decks and modes

Each deck records into its own loop buffer (up to 42 seconds) and plays it back through a granular engine in one of three modes, indicated by color:

- **Reel (yellow)** - tape emulation, monophonic. Speed and pitch are linked: faster playback raises pitch.

- **Slice (blue)** - digital sampler/looper with independent pitch and speed. Up to 3-voice polyphony; switch to mono with Alt+Size.

- **Drift (purple)** - granular texture generator; builds evolving soundscapes from a short recording.

## Recording and playback

- **Arm**: Alt+Play. Recording starts when the input crosses about -40 dB.

- **Stop and loop**: tap Play; the recorded material begins looping.

- **Overdub**: Alt+Play on a deck that is already playing. Overdub decay is set with Alt+Mix (feedback).

- **Cross-deck record**: Alt+Reverse records one deck into the other.

- **Reverse**: the Reverse pad plays the loop backwards.

## Primary controls (knobs)

| Knob       | Reel                    | Slice                          | Drift                  |
|------------|-------------------------|--------------------------------|------------------------|
| Pitch      | playback speed          | pitch (independent of speed)   | grain pitch modulation |
| Position   | loop start point        | loop start, quantized to 1/8   | grain position         |
| Size       | loop length (exp.)      | loop length (stepped)          | grain spread           |
| Envelope   | loop envelope: off, fade-out, fade-in/out, fade-in (cycles) | as Reel | as Reel |
| Mix        | input vs playback balance; Alt+Mix sets overdub feedback | same | same |

Alt+Pitch enables quantized pitch values. Alt+Size sets grain size in Drift and toggles mono/poly in Slice.

## Effects

Two families, each on a dedicated pad per deck. Hold the effect pad and tap the Tap button to cycle the effect type within the family; shape parameters live on the main knobs while the effect is held.

- **Grit**: signal degradation.

  - Analog saturation - soft saturation through to distortion (driven by Pitch).

  - Bit crusher / decimator - downsampling and bit reduction to harsh digital clipping.

- **Flux**: tape delay - Pitch sets tape speed (delay time), Mix sets wet level, Position sets feedback while Flux is active.

Each effect can be locked on (Alt + effect pad) so it stays engaged without holding.

## Modulation sources A and B

Each deck has a modulation source that is either an LFO or an envelope follower, with its own CV output (0 to +5 V).

- Source A: sample-and-hold or square LFO.

- Source B: sine or sawtooth LFO.

- The Cycle knob sets LFO rate, with tempo-sync divisions from 1/32 up to 4 bars.

- The Glow knob attenuates the modulation depth.

- The envelope follower tracks its deck's post-Mix output.

## Clock and sync

The internal clock runs 20-250 BPM. Set tempo by:

1. Tap Tempo (Tap button).

2. Hold Tap and turn Cycle A.

3. In Slice mode, hold Tap and turn Size to fit the tempo to the loop.

External sync sources:

- TRS clock in at 4 PPQN.

- MIDI clock in/out at 24 PPQN (Spotykach also converts 4 PPQN TRS to 24 PPQN MIDI out).

Switch clock source with Alt+Tap. The clock LED color shows the source: green (internal), pink (TRS), turquoise (MIDI).

**Key beat**: the quantization interval for loop and trigger alignment. Hold Tap and turn Mix A to set it. The clock LED shows white on the key beat and the source color on intermediate beats.

## Sequencer

Each deck has a 1/16-resolution trigger sequencer. Patterns persist across mode changes.

- **Record**: Alt+Seq to arm (Alt LED blinks white), then tap the deck's Seq pad in rhythm. Tap Alt or Play to stop.

- **Clear**: hold Alt+Seq for about 2 seconds (the Play LED blinks quickly).

## CV and gate

Inputs (tolerate full Eurorack ranges):

- Position/Size CV, with a target switch: up = position, down = size, center = both.

- Mix CV - input vs playback balance.

- V/Oct - modulates pitch/speed.

- Gate in - triggers a one-shot pass per deck.

Outputs:

- Two mono outputs with trim attenuators (up to ~10 Vpp, +/-5 V at max).

- Stereo output via a 3.5 mm TRS jack (use TRS cables).

- Two modulation CV outputs (0 to +5 V).

- Gate outputs - emit a short (about 7 ms) pulse when a deck's granular engine re-triggers. (See notes below: present in this firmware.)

## Routing and panning

The Routing switch sets the input/output topology:

- **Mono (left)**: each deck takes its own input; decks output separately.

- **Stereo (center)**: one input feeds both decks; outputs mix to a stereo pair.

- **Generative stereo (right)**: applies dynamic, mode-dependent panning to the deck outputs:

  - Reel: gradual left-right movement with changing speed and amount.

  - Slice: pan jumps between channels at variable intervals.

  - Drift: each grain gets a randomized pan for width. Pan speed is set with Tap+Cycle B, pan amount with Tap+Glow B.

Input behaviour: input A alone mirrors to deck B internally; input B alone feeds deck B; both inputs feed A->deck A and B->deck B (or L/R in stereo mode).

## SD card storage

- FAT32 card up to 32 GB. Layout: `SK/` containing six color-coded tape folders (`B`, `G`, `P`, `R`, `T`, `Y`), each with up to six files `1.WAV`..`6.WAV`. Filenames must be uppercase. Audio is 48 kHz, stereo, 32-bit float (16-bit PCM is also accepted and converted on load); loops over 42 s are truncated.

- **Other engines use other folders and other formats** — `tapes/`, `shuttle/`, `radio/`, `bard/`, `pstretch/`, `csound/`, `chuck/`. See [`docs/sd-card.md`](sd-card.md): `make sdcard SDCARD_OUT=/media/SK` builds a complete card, and `make check-sdcard CARD=/media/SK` explains anything wrong with an existing one.

- **Enter card mode**: hold Tap, tap a deck's Play.

- **Choose**: tap Seq to cycle tapes; turn Pitch to select the slot.

- **Save**: Alt+Play. **Load**: Play. **Exit without action**: Tap+Play.

- On power-up the last-used sample per deck can preload automatically. Disable with `pre_load 0` in `SK/config.txt`.

## Configuration file (`SK/config.txt`)

Plain text, one property name per line followed by its value on the next line:

| Property   | Range      | Meaning                                  | Default |
|------------|------------|------------------------------------------|---------|
| `mid_ch_a` | 1-16       | MIDI channel for deck A                   | 1       |
| `mid_ch_b` | 1-16       | MIDI channel for deck B                   | 2       |
| `mid_ps_a` | 0 or 1     | enable MIDI Start/Stop control for deck A | 0       |
| `mid_ps_b` | 0 or 1     | enable MIDI Start/Stop control for deck B | 0       |
| `pre_load` | 0 or 1     | auto-preload last sample on boot          | 1       |

## MIDI implementation

- Sends and receives 24 PPQN clock.

- Note On messages trigger one-shot playback (treated like a gate), routed by channel to deck A or B per the config.

- MIDI Start/Continue can auto-play and Stop can stop both decks when the corresponding `mid_ps_*` option is enabled.

## Firmware update

Build with `make -j8` (after `make -j8 libs` once) and flash over the rear USB-C port in DFU mode: hold Reset for about 3 seconds until the bottom pads breathe white, then run `make program-dfu`. See the [repository README](../README.md) for details, and [`docs/engines/`](engines/) for the other engines you can flash onto the same hardware.

## Notes vs published manual

The published manual's "Known Limitations" list is partly out of date relative to this firmware:

- **Gate outputs**: implemented here (short pulse on granular re-trigger); the published manual lists them as not implemented.

- **Buffer clearing**: a buffer clear exists internally and runs when re-arming a deck, but there is no dedicated user-facing "clear without recording" gesture yet.

- **Sample persistence between power cycles**: an auto-preload path exists; confirm the exact behaviour on your unit.

- **Spot pad**: currently used only for calibration entry; broader functionality is not yet implemented.

When you change any of the above (or any feature) in code, update this section and the relevant body text so the manual stays in sync.
