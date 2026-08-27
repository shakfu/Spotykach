# Design: manifest-driven engine generator (Faust, then unified with gen~)

**Status: v1 implemented** (the `chorus` demo). Goal: author a **new simple Faust effect or instrument** with a `.dsp` + a small **JSON manifest** and **no hand-written C++** — `make`-it and it's a flashable engine. This generalises the pattern the gen~ generator already uses (`scripts/gen_engine.py`: manifest + marker-wired build + preserved glue) to the Faust path, which was previously all hand-written.

**What shipped:** the shared runtime (`src/engine/faust/{faust_capture.h,faust_fx.h,faust_chain.h}` — `FaustEngine<Traits>` + `FaustChainEngine<Traits>` + the zone-capture `CaptureUI`), the generator (`scripts/engine_gen_common.py` + `scripts/gen_faust_engine.py`, `make faust-engine`), and three demos: `chorus` (single-deck stereo FX, ~80.5% SRAM_EXEC), `filter` (parallel/DoubleMono dual-deck, ~83%) and `voice` (series/chain dual-deck — drone osc into a filter, ~84%). Each is `.dsp`(s) + a JSON manifest → generated `<name>_engine.h`, a host test (`test_chorus`/`test_filter`/`test_voice`), and a control diagram from the same manifest. The dual-deck modes are §9. The hand-written `reverb`/`tape` wrappers were also **refactored onto the shared `faust_capture.h`** (their private `CaptureUI`/`Bind`/`Role` deleted): one zone-capture implementation now serves the generator and both rich engines, re-verified host-green (`reverb`/`tape`/`shuttle` host tests) with reverb's SRAM_EXEC cost +288 B (a `CaptureUI<false>` template flag drops the generator-only slider-default capture from the hand-written path, so they pay nothing extra for it).

This file scopes the design; the plan in [§7](#7-implementation-plan) was the build order.

## 1. Scope

**In scope (v1):** single-kernel, single-voice **mono or stereo** Faust effects/instruments — the common case (a filter, drive, chorus, flanger, wavefolder, a knobs-only drone). Knobs → Faust sliders; the kernel is placement-new'd into the SDRAM arena and driven block-by-block. The author writes only `<name>.dsp` + `<name>.json`.

**Out of scope (v1), stays hand-written:** multi-kernel / runtime-switched / route-aware / dual-deck engines. `reverb` is the exemplar — its wrapper is ~26% boilerplate (the bind tables + zone capture, which this generator *would* emit) and ~74% real integration logic (the DoubleMono plate-only cap, per-deck voice allocation, mode-switch reseed, peak metering). That 74% is genuine code, not data; v1 does not try to express it in JSON. A later phase can emit the boilerplate + a preserved hand-glue region for those.

**Never in scope:** the DSP itself. Faust/gen~ *are* the high-level DSP description; JSON specifies the **wrapper**, not the signal graph.

## 2. What the toolchain already gives us (so the manifest stays minimal)

The generated Faust kernel (`make faust-kernels` → `faust_kernel_<name>.h`, `class mydsp` in `namespace spotykach::<prefix><name>`, base types from `engine/faust_arch.h`) already exposes everything structural:

- **`getNumInputs()` / `getNumOutputs()`** → mono vs stereo. *Not* stated in the manifest.

- **`buildUserInterface(UI*)`** → each slider's **label, range, default** via `addHorizontalSlider(label, zone*, init, min, max, step)`. So the **0..1 → native range** map is captured from the kernel at `init()` (exactly as `reverb_engine.cpp`'s `CaptureUI` does today). Ranges are **not** duplicated in the manifest — the `.dsp`/library is their single source.

- **`compute(int n, FAUSTFLOAT** in, FAUSTFLOAT** out)`**, `init(sr)` → the audio entry points.

So the manifest only carries what the kernel *cannot* tell us: **which platform control drives which slider**, the capabilities, and a little metadata. Faust `.dsp` top-level `declare name/description` exist but per-slider metadata is library-dependent and unreliable, so the binding lives in the side JSON, not in `.dsp` metadata (decision in [§6](#6-open-decisions)).

## 3. The manifest

Minimal case — a flat-slider stereo FX (labels match the `hslider("…")` names in the `.dsp`):

```json
{
  "engine": "wfold",
  "backend": "faust",
  "title": "Wavefolder - drive + fold + tone",
  "knobs": {
    "Pitch":     "fold",
    "Position":  "drive",
    "Size":      "tone",
    "Envelope":  "symmetry",
    "Mix (SOS)": "mix",
    "Glow":      "bias"
  },
  "capabilities": ["CapOwnDisplay"],
  "features": { "wet_dry": "mix", "soft_limit": true }
}
```

- `knobs` maps the **platform control names** (same vocabulary as the control-diagram specs in `docs/diagrams/controls/`) → **Faust slider labels**. The generator resolves the control name to its `ParamId` (`Pitch`→`Speed`, `Position`→`Pos`, `Size`→`Size`, `Envelope`→`Env`, `Mix (SOS)`→`Mix`, `Cycle`→`ModSpeed`, `Glow`→`ModAmp`).

- `features` is a small, fixed vocabulary the generator expands: `wet_dry` (name a slider/knob as a software dry/wet crossfade — for kernels with no internal mix), `soft_limit` (cubic soft-clip the bus), later `meter` (own-display level ring). Anything outside the vocabulary → hand glue, not JSON.

Expanded entry for the rare **repeated-label** case (Dattorro's "Diffusion 1" lives in both an `Input` and a `Feedback` box, and one knob can drive two zones). The dict value becomes an object/list:

```json
"Size": [ { "label": "Diffusion 1", "box": "Input", "slot": 0 },
          { "label": "Diffusion 2", "box": "Input", "slot": 1 } ],
"Envelope": { "label": "Wet/Dry Mix", "invert": true }
```

i.e. the binding key is `(box, label) → role(+slot, +invert)`, exactly the `Bind` struct reverb uses (`{ section, label, role, slot, invert }`). Simple FX never need this; it exists so the schema can express the hard case without a different format.

## 4. What the generator emits

For `<name>.dsp` + `<name>.json`:

1. **`faust_kernel_<name>.h`** — via the existing `make faust-kernels` (cyfaust `compile -b cpp`). The generator registers `<name>` in the Makefile's `FAUST_KERNELS` list so this happens automatically.

2. **`src/engine/<name>/<name>_engine.h`** — the wrapper. For a simple FX this is fully generated and never edited: a `const Bind[]` table from the manifest, and a thin `class <Name>Engine : public IEngine` built on the shared Faust adapter ([§5](#5-shared-faust-runtime-new)) that captures zones, forwards `set_param`/`param` with range mapping, runs `compute` with mono/stereo marshalling from `getNumInputs/Outputs`, applies the `features`, and reports `capabilities()`.

3. **Build wiring**, idempotent and marker-delimited (reusing `gen_engine.py`'s `_upsert` + `>>> gen:<name> >>>` mechanism, renamed generically): the `ENGINE=<name>` block in `Makefile` (`-DSPK_ENGINE_<NAME>`, the kernel registration, `ENGINE_SOURCES`), the `#elif defined(SPK_ENGINE_<NAME>)` block in `src/engine/engine_select.h`, and the `CMakeLists.txt` branch.

4. **Validation at generate time**: the generator parses the slider labels out of the generated `faust_kernel_<name>.h` (`addHorizontal/VerticalSlider("…")`) and checks every manifest label exists — catching a typo or a renamed slider before the C++ ever compiles. It can also print a default `knobs` mapping (sliders in declaration order over the 6 plain knobs, like gen~'s `_PRIMARY`) to seed a new manifest.

The generated `<name>_engine.h` is **preserved across regeneration** unless `--force-glue` (same rule as gen~), so a hand-edited engine survives a kernel re-export.

## 5. Shared Faust runtime (new): `src/engine/faust/`

Today the zone-capture `CaptureUI` is **duplicated** (a full version in `reverb_engine.cpp`, a trimmed one in `tape/tapefx.h`). Factor it into a shared header, mirroring how `src/engine/gen/` shares `GenEngine<W>`

- `genlib_arena`:

- **`faust/faust_capture.h`** — the generic `CaptureUI : UI` (label+box → zone\*, captures range) and the `Bind`/`Role` structs, lifted from `reverb_engine.cpp` verbatim.

- **`faust/faust_fx.h`** — a `FaustFx<Kernel>` helper: placement-new the kernel in the arena, bind a `Bind[]` table, `set(role, v01)` range-maps to the zone(s), `process(in, out, n)` with mono/stereo marshalling + optional wet/dry + soft-limit. The generated `<name>_engine.h` is then ~20 lines: the `Bind[]` table + a `FaustFx`-backed `IEngine`.

`reverb` and `tape` are refactored onto `faust_capture.h` (their `CaptureUI` deleted) and **re-verified behaviour-identical / host-tests green** — a safe dedup, not a behaviour change, proving the shared runtime against the existing rich users. (Done: reverb/tape/shuttle host-green, reverb SRAM_EXEC +288 B; the `CaptureUI<false>` flag keeps the generator-only default-capture off the hand-written path.)

## 6. Open decisions

1. **Side JSON vs `.dsp` metadata.** Recommend **side JSON**: uniform across Faust *and* gen~, tooling-friendly, and Faust per-slider metadata is library-dependent. Cost: a second file to keep aligned with the `.dsp` — mitigated by the generate-time label validation (§4).

2. **Extend `gen_engine.py` vs a new front-end.** Recommend **a small shared module** (`scripts/engine_gen_common.py`) for the marker-wired build registration + preserved-glue, used by both the gen~ generator and a new **`scripts/gen_faust_engine.py`** Faust front-end.
   Same manifest *schema* across both backends (`"backend": "faust" | "gen"`); different kernel build step.
   **Done (later):** the gen~ generator was unified onto the manifest method too - it now reads a hand-authored `<name>.json` `knobs` map (control name -> gen~ param name) and emits `index_of()` from it, instead of a hand-tuned C++ switch. The control-name->`ParamId` resolver is shared in `engine_gen_common.knob_to_paramid` (extended to accept raw `ParamId` names for modifier-layer params like `Feedback`/`EnvSize`). So both backends are now declarative-JSON: Faust maps knobs->slider labels, gen~ maps knobs->gen~ param names. `gigaverb` is the worked example (`src/engine/gigaverb/gigaverb.json`).

3. **Demo engine.** A small **flat-slider stereo FX** so the demo `.dsp` is ~10 lines of `hslider()` (the `tapefx.dsp` style — no boxes, no repeated labels) and the manifest is the §3 minimal form. Candidate: a **wavefolder + tone** or a **chorus/flanger**. Exact pick at implementation.

## 7. Implementation plan

1. **Shared Faust runtime** — `src/engine/faust/{faust_capture.h,faust_fx.h}`; refactor `reverb` + `tape` onto it; `make ENGINE=reverb` / `ENGINE=tape` + `make -C host test` re-verified unchanged.

2. **Generator** — `scripts/engine_gen_common.py` (markers/upsert lifted from `gen_engine.py`) + `scripts/gen_faust_engine.py` (parse manifest, parse kernel sliders to validate/seed bindings, emit `<name>_engine.h`, register the kernel + wire the build). A `make faust-engine` / one-shot target.

3. **Demo** — write `<demo>.dsp` + `<demo>.json`, run the generator, `make ENGINE=<demo>` links and fits (watch SRAM_EXEC), add a host test (param round-trip + finite/bounded audio), generate its control diagram from the same manifest.

4. **Convergence (small, high-value)** — the manifest already holds the `knobs` → `ParamId` → slider map, so the generator can also emit the **control-diagram spec** (`docs/diagrams/controls/<name>.json`) and the **doc control table**: one manifest → engine + registration + diagram + doc, closing the duplication the control-spec work started.

5. **Docs** — update `docs/engine-types/faust.md` (the new "drop a `.dsp` + manifest" path) and link this file.

## 8. Risks / limits

- **Label drift**: if a `.dsp` renames a slider, the binding breaks — caught at generate time by parsing the kernel's `addSlider` labels (§4), not at runtime.

- **Library-generated labels**: `dm.*` stdlib demos emit their own labels/boxes (the Dattorro case); a manifest over those needs the expanded `(box,label)` form. Flat hand-written `hslider()` FX (the v1 demo) avoid it.

- **SRAM_EXEC**: the kernel lives in the SDRAM arena (placement-new), as today — generation adds no SRAM pressure beyond the wrapper.

- **Instruments**: a Faust synth needs CV/gate/MIDI → slider bindings (`cv_voct`→a freq slider, gate→a gate button). v1 covers knob→slider (FX, and knobs-only drones); the input-binding vocabulary is a straightforward schema extension, called out but not built in v1.

- **ROI**: this is machinery for a backend with ~2 engines today. It's justified only by the stated goal — making *new* simple Faust FX near-zero-effort. If that goal holds, v1 pays for itself on the third engine.

## 9. Dual-deck modes (v1.1 — implemented)

v1 generated a **single** logical control set: `FaustEngine::set_param` discarded the `DeckRef` arg, so deck A and deck B knobs collapsed onto one state (chorus). v1.1 adds two ways a generated engine can use **both** decks as independent control banks. The platform already delivers every knob for both decks (`core.ui.cpp` calls `set_param(id, A, ..)` *and* `set_param(id, B, ..)` for all engines); the engine just has to keep per-deck state. `CapDualDeck` is advertised for the dual-deck display/layout, but the routing needs no platform change.

The manifest selects the mode with a `deck_mode` key (absent / `"single"` = v1 behaviour):

### 9a. `parallel` (DoubleMono) — same kernel ×2

Two instances of **one** mono kernel: deck A processes the **left** channel, deck B the **right**, each driven by its own knob bank (the reverb DoubleMono shape, generalised). The two never interact; the crossfader is unused. Requires a **mono** kernel (1-in/1-out). The `knobs` map is shared (both decks expose the same controls), so the manifest is the v1 form plus `"deck_mode": "parallel"`:

```json
{ "engine": "filter", "backend": "faust", "deck_mode": "parallel",
  "knobs": { "Pitch": "cutoff", "Position": "reso", "Size": "drive", "Mix (SOS)": "mix" },
  "features": { "meter": true, "color": "0xff8833" } }
```

Runtime: `FaustEngine<Traits>` gains `static constexpr int decks` (1 default, 2 for parallel). State becomes `_k[decks]`, `_role[decks][Count]`, `_v[decks][Count]`; `set_param`/`set_mod_speed`/`param` index by the (clamped) deck; `process` runs each instance on its own channel (`decks==1` keeps the v1 mono/stereo/0-in marshalling unchanged); `render` shows a per-deck output meter. For `decks==1` the arrays are size-1 and the codegen collapses to v1 (re-verified).

### 9b. `series` (chain) — two **different** kernels, deck A → stage 1, deck B → stage 2

The engine is a chain of **two distinct** kernels: deck A's knobs drive the first stage, deck B's the second, and the signal flows A→B. This covers **FX→FX** (e.g. drive→filter) and **instrument→FX** (a 0-input generator into an effect — which also exercises the otherwise-untested 0-in path). The chain is mono internally and duplicated to the stereo bus at the output. The manifest lists the two stages (each its own `.dsp` in the engine dir, with its own knob map):

```json
{ "engine": "voice", "backend": "faust", "deck_mode": "series",
  "stages": [
    { "dsp": "osc",    "knobs": { "Pitch": "freq", "Size": "shape", "Mix (SOS)": "level" } },
    { "dsp": "filter", "knobs": { "Pitch": "cutoff", "Position": "reso", "Size": "drive", "Mix (SOS)": "mix" } }
  ],
  "features": { "meter": true, "color": "0x88ff33" } }
```

Runtime: a sibling template **`FaustChainEngine<Traits>`** (`src/engine/faust/faust_chain.h`) holds `StageA*`/`StageB*`, two per-deck role tables (deck A's bound to stage A's zones, deck B's to stage B's), and chains `compute` in `process` (stage A's 0-input instrument case feeds silence in). `set_param` is uniform — it writes `_role[deck][r].set(v)`, and because each deck's role table already points at the correct kernel's zones, the wrapper never special-cases which stage. The two stage kernels get engine-prefixed namespaces (`fx_<engine>_<stage>`) so stage names can't collide across engines.

**Still out of scope** (stays hand-written): the runtime route/mode switch (Stereo↔DoubleMono selection, reverb's case), per-deck voice allocation, and >2 decks/stages. v1.1 is exactly "two independent instances/stages, fixed topology" — the largest step that stays pure data, not integration logic.

**Examples shipped:** `filter` (parallel — two independent resonant low-pass voices, one per channel) and `voice` (series — a drone oscillator into a resonant filter, deck A = osc, deck B = filter). Both host-tested for per-deck/per-stage independence (the property chorus structurally cannot have).
