# reso engine

A dual resonator / pluck voice (engine #1 in `docs/engine-ideas.md`, "Resonator / Pluck"), built on the Mutable Instruments **Rings** DSP (`src/engine/reso/thirdparty/rings`) rather than a hand-rolled Karplus-Strong loop. Each deck wraps one `rings::Part` (mono, polyphony 1): modal bodies, sympathetic strings, plucked strings, and an FM voice.

> Implementation (PIMPL), the build system, the file map, and the bug writeups live in [`docs/dev/reso-impl.md`](../dev/reso-impl.md).

---

## Concept and modes

The reel/slice/drift mode switch (`ConfigId::Mode`, per deck - mirrors granular's int mapping: 0 = Slice, 1 = Reel, 2 = Drift) selects how the resonator is excited. Alt+PITCH (`ParamId::Aux`, `CapAux`) selects the Rings model. Two orthogonal axes.

- **Reel** - the resonator is fed from *outside* (Rings `internal_exciter = false`): the live input drives it continuously (a body you can play with audio), and each **trigger** injects a short ~4 ms noise burst so the pad/gate/MIDI plucks it. Silent when idle (no constant drone). `SIZE` (damping) sets how long a triggered note sustains.

- **Slice** - discrete **plucks** (`internal_exciter = true`): each trigger strums Rings' internal exciter at the current note. `MODFREQ` (`ModSpeed`) above a threshold turns on a tempo-synced triad+octave arp.

- **Drift** - a scatter cloud: a free-running internal scheduler auto-strums at randomized intervals/notes; `MODFREQ` = density, `MOD_AMT` = randomization spread.

Slice and Drift use Rings' internal exciter only - they feed **silence** into the resonator, not the live input. Only Reel is fed from outside. (Feeding the live input into the internal-exciter modes layered a continuous unpitched drone over every pluck; see the resolved-bug section.) `MODFREQ`/cycle defaults to **off** (engine-seeded to 0 via the param cache); a non-zero default previously free-ran the Slice arp from boot.

### Control map (per deck)

![Reso control surface](../media/reso-controls.svg)

*Generated from [`docs/diagrams/controls/reso.json`](../diagrams/controls/reso.json) via `make diagrams`.*

| Knob | `ParamId` | Rings target |
|---|---|---|
| PITCH | `Speed` | note (pitch) -> `performance_state.note` |
| SIZE | `Size` | `patch.damping` (decay/sustain) |
| POS | `Pos` | `patch.position` (excitation/pickup position) |
| ENV | `Env` | `patch.brightness` |
| MOD_AMT | `ModAmp` | `patch.structure` |
| SOS | `Mix` | dry/wet |
| MODFREQ | `ModSpeed` | Drift density / Slice arp rate |
| Alt+PITCH | `Aux` | model: modal / sympathetic-string / string / FM / string+reverb |

`capabilities() = CapOwnDisplay | CapDualDeck | CapAux | CapTransport`. Deck A -> left out, deck B ->
right out. Output is soft-clipped.

### Knob defaults (engine-seeded)

The platform seeds the SIZE/ENV/MODFREQ knob start positions from each engine's param cache (the same `param()`-seed mechanism as POS/MOD_AMT/Alt+PITCH), so reso carries sensible defaults without changing other engines:

- **ENV = 0.5.** ENV drives Rings brightness, and at 0 the excitation filter collapses the pluck to near-silence - a 0 default made the voice silent when a pad was pressed. 0.5 boots it audible. (Note: reso's ENV morphs brightness; it is not the granular amplitude-envelope-shape knob the manual describes for the same physical control.)

- **SIZE = 0.5.** SIZE is `patch.damping`, whose decay (rt60) is steep near the top, so the knob is very sensitive there; 0.5 centres it in a usable range. (Was 1.0.)

- **MODFREQ/cycle = 0.** Off by default so the Slice arp / Drift scatter does not free-run from boot.

Other engines are unchanged: granular and delay carry their own 1.0 SIZE / 0.3 cycle in their caches; ENV stays 0.0 (granular's documented "envelope off" at fully CCW).

The display (`render`) draws, per deck: a mode-coloured energy meter (Reel yellow, Slice blue, Drift purple), a white **pitch dot** whose ring position equals `pitch_n` (a live readout used as a diagnostic), a play-LED flash on each trigger, and the mode L/C/R indicators.

**Model selector (Alt+PITCH).** The Rings model has no dedicated hardware indicator, so **while Alt is held** `render` draws the five model options as evenly-spaced points around the ring - the selected model bright, the rest dim - in place of the pitch dot, the whole time (not just on a change). The platform pushes the held state each loop via the defaulted `IEngine::set_aux_active(deck, active)` hook (`core.ui.cpp`, before the `render` call): for a `CapAux` engine it sends `Alt down && that deck's PITCH not claimed by an fx touch`. reso stores it per deck (`aux_held`) and shows the selector accordingly; other engines ignore the no-op default. The host test asserts the selector lights while held (more points than the lone pitch dot), persists across frames, and clears on release.

### Level note

Reel's external-exciter path is ~4x quieter than Rings' internal plucker, so the Reel trigger burst is boosted (gain `2.0f` in the Reel branch of `process`) to match a Slice pluck (~0.4 peak). That `2.0f` is the Reel level knob if it needs nudging.

---

## Build / flash

```text
make ENGINE=reso             # build (~96% SRAM_EXEC)
make engine-reso             # clean + build + DFU flash
make -C host test            # host suites incl. test-reso
```
