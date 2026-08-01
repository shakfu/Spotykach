# mosc engine

A dual **macro-oscillator**, built on the Mutable Instruments **Plaits** DSP (`src/engine/mosc/thirdparty/plaits`) — the *full* 24-engine synthesis voice. Each deck wraps one `plaits::Voice`, so the two decks are two independent macro-oscillators (virtual analog, waveshaping, FM, wavetable, granular, additive, chord, speech, modal, drums, and the newer "engine2" models). Sibling of the [reso](reso.md) engine (both are Mutable Instruments DSP placement-new'd into the SDRAM arena and share the same `stmlib`).

> Implementation (PIMPL + QSPI build), the file map, the 24-engine order, and the build internals live in [`docs/dev/mosc-impl.md`](../dev/mosc-impl.md).

Like [csound](csound.md) and [chuck](chuck.md), mosc is a **QSPI build** — the full 24-engine voice is ~292 KB of code, too big for the 186 KB execution SRAM, so it runs from QSPI flash (`make engine-mosc`). Unlike those two it still synthesises from the platform's SDRAM arena (it is a normal SRAM-class engine that simply *executes* from QSPI), so no SD card or external library is required.

---

## Concept

Plaits is a "macro-oscillator": one knob picks a **synthesis model** (engine), and four continuous controls — **harmonics**, **timbre**, **morph**, plus the note — shape it, with an internal low-pass gate (LPG) for decay/colour. mosc gives each deck a complete Plaits voice and projects Plaits' panel onto the Spotykach controls.

Two switches set behaviour:

- **Mode (per deck, `ConfigId::Mode`)** — *Gate* vs *Drone*. **Gate**: each trigger (Play pad / gate-in / MIDI / Seq) strikes the LPG/decay envelope, so notes have a percussive/plucked shape (`MOD_AMT` = decay). **Drone**: the LPG is bypassed and the engine runs open/continuous (good for pads and held tones).

- **Routing (global, `ConfigId::Route`)** — how the two voices reach the L/R outputs:

  - **Stereo** (center) — deck A → left, deck B → right (two independent oscillators, hard-panned).

  - **DoubleMono** (left) — both voices summed to a centred mono image (layer two engines into one sound).

  - **GenerativeStereo** (right) — an out/aux spread for width: each channel carries one voice's main output plus the *other* voice's **aux** output (Plaits' secondary signal — a sub/variation), so two mono voices bloom into a decorrelated stereo field.

### Control map (per deck)

![Mosc control surface](../media/mosc-controls.svg)

_Generated from [`docs/diagrams/controls/mosc.json`](../diagrams/controls/mosc.json) via `make diagrams`._

| Knob | `ParamId` | Plaits target |
|---|---|---|
| PITCH | `Speed` | note (pitch) → `patch.note` (+ V/Oct CV) |
| SIZE | `Size` | `patch.harmonics` |
| POS | `Pos` | `patch.timbre` |
| ENV | `Env` | `patch.morph` |
| MOD_AMT | `ModAmp` | `patch.decay` (LPG decay) |
| MODFREQ | `ModSpeed` | `patch.lpg_colour` |
| SOS / MIX | `Mix` | output level |
| Alt+PITCH | `Aux` | **engine select** (one of 24 models) |

`capabilities() = CapOwnDisplay | CapDualDeck | CapAux`.

### CV map (per deck)

mosc wires the three per-deck CV jacks the platform provides. All readings are calibrated and signed (≈0 when nothing is patched), so the knobs rule until a cable is inserted.

| CV jack | Destination |
|---|---|
| V/Oct | note (additive transposition, like reso) |
| CV_SIZE_POS | `harmonics` modulation (summed) |
| CV_MIX | `timbre` modulation (via Plaits' timbre attenuverter) |

Plaits' other CV inputs (MODEL CV, FM CV, MORPH CV, LEVEL/accent CV, and the FM/MORPH attenuverters) have no hardware home on the Spotykach panel and are left unpatched. The modulation depths are tunable constants (`kHarmCvDepth`, `kTimbreModAmt`) at the top of `mosc_engine.cpp`.

### Engine selection (Alt+PITCH)

There are **24 engines**; **hold Alt and turn PITCH** to scroll them (pickup-gated, like the other `CapAux` selectors). While Alt is held the ring shows the current engine position in place of the pitch dot. The boot default is **#8, Virtual Analog**. The full order (Plaits' own registration order — newer "engine2" bank first, then the classic models) is listed in [`docs/dev/mosc-impl.md`](../dev/mosc-impl.md#engine-order).

### Display

`render` draws, per deck: an energy meter coloured by the deck's Mode (Gate green, Drone orange), a white **pitch dot** at `pitch_n` (replaced by the engine dot while Alt is held), and a play-LED flash on each trigger. The center/left/right **mode LEDs** show the global **Route** (Stereo = center, DoubleMono = left, GenerativeStereo = right) — they sit under the routing switch.

---

## Build / flash

mosc is a **QSPI-execute** target (`BOOT_QSPI` + `alt_qspi.lds`), like csound/chuck:

```text
make engine-mosc             # clean + build + DFU flash (board in DFU)
make program-mosc            # re-flash the last build without rebuilding
make ENGINE=mosc APP_TYPE=BOOT_QSPI LDSCRIPT=linker/alt_qspi.lds   # explicit form
```

No prerequisite fetch is needed — the Plaits DSP and the shared `stmlib` are vendored in the tree (`src/engine/mosc/thirdparty/plaits`, `src/engine/common/thirdparty/stmlib`). A plain `make ENGINE=mosc` (BOOT_SRAM) will **not** link — the 292 KB of code overflows SRAM_EXEC; that is by design, mosc runs from QSPI.
