# reverb engine

`ENGINE=reverb` · `src/engine/reverb/reverb_engine.{h,cpp}` · class `ReverbEngine`

A **route-aware** stereo reverb with **three all-Faust algorithms** - a **Dattorro plate**, a **Zita-rev1 hall**, and a **Greyhole** (a modulated, evolving/ambient reverb) - the physical **Reel/Slice/Drift mode switch selecting between them** live, per deck. Built with the [Faust / cyfaust method](../engine-types/faust.md); that page covers the pipeline (codegen, namespacing, the arch shim, arena placement-new, the generic `CaptureUI` binding). This page is the reverb specifics.

> Implementation, the Faust footprint/device-load measurements, the file map, and "adding a reverb" live in [`docs/dev/reverb-impl.md`](../dev/reverb-impl.md).

## The two algorithms

A deliberately contrasting pair, both MIT-licensed Faust demos (the `maths.lib` they pull is LGPL with the standard Faust runtime exception):

- **Dattorro plate** (`dm.dattorro_rev_demo`, ~126 KB state) - a modulated figure-8 plate (JAES 1997, "Effect Design Part 1"). Short, lush; the tank's LFO excursion avoids the metallic ring of a fixed-comb reverb.

- **Zita-rev1 hall** (`dm.zita_rev1`, ~937 KB state) - Fons Adriaensen's reference FDN hall: an 8x8 feedback-delay network with frequency-dependent decay (separate low/mid RT60) and HF damping. Lusher and longer than the plate.

It began as a spike to measure what Faust-generated C++ costs in `SRAM_EXEC` on the H7, and was promoted to a real reverb once that held up (closed-form DSP is a few KB of code). The original spike voice (a saw -> Moog VCF -> ADSR, `voice.dsp`) is retained as an alternate source for code-size measurement, not wired into the build.

## Route-aware topology

The reverb reads the panel **routing switch** (`ConfigId::Route`) and adapts, like the rest of the instrument:

- **Stereo / GenerativeStereo** -> **one stereo voice** (deck A's selection) reverberates the stereo pair. Deck A's knobs + mode switch drive it; deck B's strip is inert.

- **DoubleMono** -> **two independent mono PLATES, one per deck**: deck A reverberates the left input into the left output, deck B the right, each driven by its own knob bank. The two sides are channel-isolated (a deck with a silent input stays silent - no crosstalk). Each side is mono (the plate is run mono: input fanned to both channels, one output kept), trading the plate's internal stereo width for two independent sends.

**The cap: DoubleMono is plate-only.** The hall and Greyhole are stereo (2-in/2-out) and heavy; *two* of them at once thrash the H7's D-cache (measured ~70% avg / ~89% peak on hardware - too close to the 2 ms deadline). So the heavy voices are **single-voice (stereo route) only** - in DoubleMono the Mode switch is forced to plate (`eff_voice()`), which can never put two heavy voices on the bus at once. The switch selection is remembered, so flipping back to a stereo route restores the chosen hall/Greyhole.

A full set of voices is allocated **per deck** up front, so the route is a runtime branch in `process()` with no reallocation. Per-deck footprint is ~2.1 MB (all three Faust voices) - trivial against the 48 MB arena.

## Control map

![Reverb control surface](../media/reverb-controls.svg)

*Generated from [`docs/diagrams/controls/reverb.json`](../diagrams/controls/reverb.json) via `make diagrams`.*

The six panel knobs map to reverb-agnostic **roles**; the 0..1 knob is linear-mapped into each Faust slider's native range, captured at bind time - so each reverb's units (plate 0..1, hall RT60 seconds / damping Hz / pre-delay ms) just work without per-reverb scaling code.

| Knob | `ParamId` | role | Dattorro (plate) | Zita (hall) |
|---|---|---|---|---|
| SOS | `Mix` | Mix | Dry/Wet (-1..+1) | Wet/Dry Mix |
| **PITCH** | `Speed` | **Decay** | Decay Rate | Mid RT60 |
| ENV | `Env` | Damp | Damping | HF Damping |
| **POS** | `Pos` | **Tone** | Prefilter | Low RT60 |
| SIZE | `Size` | SizeA | input Diffusion (x2) | In Delay (pre-delay) |
| MOD_AMT | `ModAmp` | SizeB | tank Diffusion (x2) | tail EQ (Eq1 Level, +/-15 dB) |
| Reel/Slice/Drift switch | `ConfigId::Mode` | select | **algorithm: plate / hall / greyhole** | |

**Decay** is on **PITCH** (the biggest knob) since it is the most impactful character control; **Tone** is on POS. Selection moved from the old Alt+PITCH (`CapAux`) gesture to the dedicated **3-position Reel/Slice/Drift switch** - one tactile position per algorithm. The switch is per-deck on the hardware, but only takes effect in a stereo route: there deck A's switch selects the voice; in DoubleMono the voice is forced to plate (the cap below), so the switch is inert (selection is remembered for when you return to a stereo route).

`capabilities() = CapOwnDisplay | CapDualDeck`. Output Level is captured and held fixed (plate -6 dB, hall 0 dB); Zita's Eq1 Level is the SizeB knob (tail tone), Eq2 and the EQ frequencies stay default. The mode switch's 0/1/2 position maps straight to the voice index and re-applies the cached knob values to the newly-active kernel, so a switch never strands a knob.

The display (`render`) keeps each deck's ring always legible - it draws three things at once: the **active algorithm** as the ring colour (**plate = blue, hall = violet, greyhole = teal**) over a dim full-ring baseline, so the panel is never dark even in silence; the **DECAY knob (PITCH)** as the bright arc length, so tuning the tail gives instant feedback; and the **output level** as the arc's brightness, layered on top, so the ring pulses with signal and visibly fades out as the reverb tail decays. In DoubleMono each ring shows its own deck's plate + decay; a stereo route shows deck A's voice + decay on both rings. The three mode L/C/R LEDs sit at the Reel/Slice/Drift switch and show the **selected algorithm**, lit in that algorithm's colour - so the third position (greyhole) reads as a distinct **teal** against plate-blue / hall-violet, and you can see which algorithm is active even in silence. (Route is no longer shown on these LEDs; DoubleMono is legible from the two independent per-deck rings.)

A reverb-specific binding wrinkle: a Faust label can repeat across boxes - Dattorro's "Diffusion 1/2" appear in both its Input and Feedback boxes - so each bind-list entry keys on the enclosing box as well as the label. (The generic zone-capture mechanism is in [faust.md](../engine-types/faust.md).)

## Third voice: Greyhole

The third algorithm is **Greyhole** (Faust's `re.greyhole`) - a diffusion network of *modulated* allpasses with a pitch-shifter in the feedback path, giving a lush, evolving, ambient tail very unlike the static plate/hall (originally a SuperCollider plugin by Nick Collins / Julian Parker). A plain Faust kernel like the other two - no gen~ runtime. `greyhole.dsp` owns its dry/wet crossfade (so the Mix role binds straight to its slider, like the plate) and maps all six knob roles to a real control: **Feedback** (tail, PITCH), **Size** (SIZE), **Damp** (ENV), **Diffusion** (POS), **ModDepth** (MODAMT), **Mix** (SOS). Selected at mode-switch position 3 (teal).

Greyhole is the **heaviest** voice (modulated delays + pitch-shifter), so its code overflows `SRAM_EXEC` at `-O2`; the reverb is therefore built at **`-Os`** (like reso), fitting at ~97.4%. Its **per-block CPU on device is unmeasured** - if it glitches, that's the thing to check (`METER=1`). The standalone gen~ [gigaverb](gigaverb.md) engine still exists on its own (`ENGINE=gigaverb`); it was only removed *from the reverb*.

## Build / flash

```text
make -j8 ENGINE=reverb                 # build (plate + hall + greyhole; auto -Os); link prints SRAM_EXEC
make ENGINE=reverb METER=1             # + on-device CPU load meter (reads over serial)
make ENGINE=reverb program-dfu         # flash (device in DFU mode first)
make engine-reverb                     # one-shot: clean + build + flash
make -C host test-reverb               # host test (plate/hall/greyhole; also bench-reverb)
```

See [faust.md](../engine-types/faust.md) for the cyfaust `.venv` setup and the codegen detail.
