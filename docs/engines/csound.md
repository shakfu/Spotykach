# csound engine (`ENGINE=csound`)

The `csound` engine runs a full [Csound](https://csound.com) 7 instance as a synth on the spotykach. An orchestra (a `.csd` text document) is compiled and performed at runtime, so the **patch — not the firmware — defines the sound**. You load orchestras from the SD card, switch between them live, and play them from the panel knobs and over MIDI.

It behaves like any other engine on the panel, but is built and flashed a little differently: Csound's code is ~2 MB (too big for SRAM), so this is a **QSPI build** that executes from flash. Internals (memory model, allocator, bring-up notes, roadmap) are in [`docs/dev/csound-impl.md`](../dev/csound-impl.md).

![Csound control surface](../media/csound-controls.svg)

_Generated from [`docs/diagrams/controls/csound.json`](../diagrams/controls/csound.json) via `make diagrams`._

## Build & flash

One-time: build the Csound library (needs `cmake` and the `arm-none-eabi` GCC toolchain):

```text
scripts/fetch_csound.sh          # download Csound 7 + cross-build libcsound.a
```

Then build + flash the engine (put the board in DFU first):

```text
make engine-csound               # clean + build the QSPI image + flash
make program-csound              # re-flash the last build without rebuilding
```

Recover the board at any time by flashing any normal engine.

## Controls

With the built-in orchestra (used when no SD patch is loaded):

| Control | Effect |
|---|---|
| **PITCH** | pitch |
| **SIZE** | brightness (filter cutoff) |
| **MIX** | level |
| **MIDI NoteOn** | plays a note (channel 1 → deck A, channel 2 → deck B) |
| **centre mode LED** | white = built-in orchestra, cyan = an SD patch is loaded |

What each knob actually does is up to the patch (it reads named control channels) — the table above is the convention the bundled patches follow.

## Loading patches from the SD card

Put full CSD documents in a `csound/` folder at the card root, named `0.csd` … `7.csd`:

```text
<card>/csound/0.csd
<card>/csound/1.csd
```

Ready-to-copy examples are in [`examples/csound/`](../../examples/csound/) — a drone+pluck, a fat bass, and a polyphonic MIDI lead, with a README.

- At boot the engine **auto-loads** the lowest-numbered patch (or the built-in if the card has none).

- **Hold Alt and turn PITCH** to scroll the selector (a dot per patch around the rings); **release** to switch to it live. The centre LED turns cyan for an SD patch, white for the built-in.

- Anything missing, malformed, or that fails to compile falls back to the built-in, so the unit always makes sound.

## Writing a patch

A patch is an ordinary CSD. To be driven by the panel, read these control channels with `chnget` (deck A / deck B):

| Knob | Channel |
|---|---|
| PITCH | `speedA` / `speedB` |
| MIX | `mixA` / `mixB` |
| SIZE | `sizeA` / `sizeB` |
| ENV | `envA` / `envB` |
| modifier layer | `fbA`, `modspA`, `modampA` (+ `B`) |

Define `instr MidiNote` (with `p4` = note frequency in Hz) to make the patch playable from MIDI. The `examples/csound/` orchestras are the templates.

**Format rules that matter** — the on-device CSD parser is line-oriented and strict:

- Each section tag on its **own line**: `<CsScore>` then `</CsScore>`, never `<CsScore></CsScore>` on one line (an empty section collapsed to one line won't compile).

- Include a `<CsOptions></CsOptions>` block (it can be empty).

- A UTF-8 BOM and CRLF (Windows) line endings are tolerated — the engine strips them on load.

```text
<CsoundSynthesizer>
<CsOptions>
</CsOptions>
<CsInstruments>
  ; sr / 0dbfs / nchnls, then your instr 1.. and instr MidiNote
</CsInstruments>
<CsScore>
</CsScore>
</CsoundSynthesizer>
```

**MIDI is NoteOn-only** on this platform (no note-off, no velocity), so MIDI notes are fixed-length stabs, not hold-while-pressed. For clean polyphony, sum your voices through a limiter rather than each calling `outs` (otherwise overlapping notes clip) — see `examples/csound/2.csd`.

## Limitations

- **Opcodes:** the entire Csound **core** opcode library is available (oscillators, filters, envelopes, delays, reverbs, the `pvs` spectral family, granular, physical models — well over a thousand). Excluded by the bare-metal build: **plugin / external opcodes** (no runtime loading) and **soundfile I/O** (`diskin`, `soundin`, `GEN01` / sample-from-WAV — table-generating GENs still work), plus OSC and MIDI-/audio-device opcodes (the host feeds I/O).

- **CPU:** code runs from QSPI flash (slower than SRAM), so heavy or dense patches can glitch. Lighter voices and a capped polyphony (`maxalloc`) help.

- **Memory:** Csound gets a 12 MB pool for its patches.
