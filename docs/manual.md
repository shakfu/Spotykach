# Spotykach platform manual (firmware-tracked)

This is the in-repo manual for the **platform** — the parts of the instrument that are the same
whichever engine you flash: power, the clock and sync, CV and gate, routing, the SD card, `config.txt`,
MIDI, and firmware update.

**What each engine does with the knobs and pads is per-engine**, and lives in
[`docs/engines/`](engines/). Start there for the instrument you are actually running:

| If you flashed... | Control reference |
|---|---|
| the default build (`make`) | [granular](engines/granular.md) — the stock looper/sampler |
| an effect | [delay](engines/delay.md) · [qdelay](engines/qdelay.md) · [reverb](engines/reverb.md) · [gigaverb](engines/gigaverb.md) · [chorus](engines/chorus.md) · [filter](engines/filter.md) · [pstretch](engines/pstretch.md) |
| a looper or player | [tape](engines/tape.md) · [shuttle](engines/shuttle.md) · [softcut](engines/softcut.md) · [radio](engines/radio.md) · [bard](engines/bard.md) · [graincloud](engines/graincloud.md) |
| an instrument | [edrums](engines/edrums.md) · [reso](engines/reso.md) · [mosc](engines/mosc.md) · [glitch](engines/glitch.md) · [voice](engines/voice.md) · [csound](engines/csound.md) · [chuck](engines/chuck.md) |

Unlike the published manual, this document is kept in sync with the firmware in this repository: when
you change behaviour, update it in the same commit. Where this document and the published manual
(<https://tsemah.notion.site/Spotykach-Manual-22c6331933b880c59108c0de25102bb5>) disagree, this one
describes what the current code actually does. The published manual documents the granular engine, so
the differences are called out in [the granular page](engines/granular.md#notes-vs-the-published-manual).

Spotykach is a screenless, dual-deck instrument built on an Electro-Smith Daisy Seed. It runs at
48 kHz. The two decks (A and B) are independent and share a clock, modulation, routing and SD-card
storage; what a "deck" *is* depends on the engine — two recorders in granular, two delay lines in
delay, two macro-oscillators in mosc.

## Power

- USB-C (5 V, 1 A) or 15 V barrel jack (1 A, center-positive, 5.5 mm / 2.5 mm). Both may be connected at once.

- There is no power switch.

- A ground loop via computer USB can cause high-pitched noise; a standalone USB adapter avoids it.

## Clock and sync

The internal clock runs 20-250 BPM. Set tempo by:

1. Tap Tempo (Tap button).

2. Hold Tap and turn Cycle A.

3. Hold Tap and turn Size to fit the tempo to what is loaded, where the engine offers it (granular's Slice mode does; an engine advertises this through `size_sets_tempo`).

External sync sources:

- TRS clock in at 4 PPQN.

- MIDI clock in/out at 24 PPQN (Spotykach also converts 4 PPQN TRS to 24 PPQN MIDI out).

Switch clock source with Alt+Tap. The clock LED color shows the source: green (internal), pink (TRS), turquoise (MIDI).

**Key beat**: the quantization interval for loop and trigger alignment. Hold Tap and turn Mix A to set it. The clock LED shows white on the key beat and the source color on intermediate beats.

## CV and gate

Inputs (tolerate full Eurorack ranges):

- Position/Size CV, with a target switch: up = position, down = size, center = both.

- Mix CV - the `Mix` parameter. What that blends is the engine's choice (input vs playback in granular, wet/dry in the effects, output level in mosc).

- V/Oct - modulates pitch/speed.

- Gate in - triggers a one-shot pass per deck.

Outputs:

- Two mono outputs with trim attenuators (up to ~10 Vpp, +/-5 V at max).

- Stereo output via a 3.5 mm TRS jack (use TRS cables).

- Two modulation CV outputs (0 to +5 V).

- Gate outputs - emit a short (about 7 ms) pulse when a deck re-triggers, for engines that report one (`gate_out_triggered`); granular fires it on a loop reset. Present in this firmware, though the published manual lists it as unimplemented.

## Routing and panning

The Routing switch sets the input/output topology:

- **Mono (left)**: each deck takes its own input; decks output separately.

- **Stereo (center)**: one input feeds both decks; outputs mix to a stereo pair.

- **Generative stereo (right)**: the engine is free to move the two decks around the stereo field rather than hold them still. What that means is per-engine — granular applies mode-dependent panning (see [granular](engines/granular.md)), mosc spreads its two voices out+aux — and an engine that has nothing to offer here simply behaves as Stereo.

Input behaviour: input A alone mirrors to deck B internally; input B alone feeds deck B; both inputs feed A->deck A and B->deck B (or L/R in stereo mode).

The switch reaches the engine as `ConfigId::Route`; the three positions are `Route::DoubleMono`, `Route::Stereo` and `Route::GenerativeStereo`.

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
