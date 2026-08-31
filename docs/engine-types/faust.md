# Faust engines (cyfaust)

Author the DSP in [Faust](https://faust.grame.fr) (`.dsp`), generate C++ kernels with [cyfaust](https://github.com/shakfu/cyfaust) (a Cython wrapper of libfaust), and wrap them behind `IEngine`. There are two ways to write that wrapper:

- **Generated** (a `.dsp` + a small JSON manifest, **no C++**) - for a simple single-kernel mono/stereo effect. The generator emits the wrapper on the shared `FaustEngine<Traits>` template. See [Generated engines](#generated-engines-no-hand-written-c) below and the design in [`docs/dev/engine-gen.md`](../dev/engine-gen.md).

- **Hand-written** - for a rich engine with several interchangeable algorithms, route-aware topology, or custom display (the `reverb` case). You keep full control of selection, topology, and rendering.

**Implementations:** [reverb](../engines/reverb.md) (hand-written: Dattorro plate + Zita hall, mode switch selects, route-aware), [tape](../engines/tape.md) (a native engine whose `tapefx` wow/flutter + hysteresis is a Faust kernel), [chorus](../engines/chorus.md) (generated, the demo). The build-system detail and footprint table also live in `src/engine/reverb/README.md`.

## Generated engines (no hand-written C++)

For a single-kernel mono/stereo effect or knobs-only instrument, `scripts/gen_faust_engine.py` produces the whole engine from `src/engine/<name>/<name>.dsp` + a `<name>.json` manifest. The manifest only maps platform knobs to Faust slider labels (ranges are captured from the kernel at `init()`, so they are not duplicated) plus a few feature flags (`meter`, `soft_limit`, a software `wet_dry`). It builds the cyfaust kernel, emits `<name>_engine.h` (a `Traits` struct - the `ParamId`->slider bind table + flags - bound to the shared `FaustEngine<Traits>` in `src/engine/faust/faust_fx.h`), wires the Makefile/`engine_select.h`/CMake (marker-delimited blocks, like the gen~ generator), and emits the control-diagram spec so one manifest drives the engine, the build, and the diagram.

```text
make faust-engine MANIFEST=src/engine/chorus/chorus.json   # kernel + wrapper + build + diagram
make -j8 ENGINE=chorus
```

The MODFREQ knob (which reaches an engine via `set_mod_speed`, not `set_param`) is available as a binding onto `ParamId::ModSpeed`. The generated `<name>_engine.h` is preserved on re-run unless `--force-glue`.

### Dual-deck modes

A manifest `deck_mode` key lets a generated engine use **both** decks as independent control banks (the default, absent, is the single shared control set of `chorus`):

- **`parallel`** (DoubleMono) - two instances of **one mono kernel**, deck A on the left channel and deck B on the right, each with its own knob bank; the two never interact. On `FaustEngine<Traits>` with `decks = 2`. Demo: [filter](../engines/filter.md) (a resonant low-pass per channel).

- **`series`** (chain) - **two different kernels** wired A→B, deck A driving stage 1 and deck B stage 2; covers FX→FX and instrument→FX (a 0-input generator into an effect). On the sibling `FaustChainEngine<Traits>` template. Demo: [voice](../engines/voice.md) (a drone oscillator into a resonant filter).

Both advertise `CapDualDeck` and need no platform change - the platform already delivers every knob for both decks. Still hand-written (out of the generator's scope): a runtime route/mode switch (Stereo↔DoubleMono selection, the `reverb` case), per-deck voice allocation, and >2 decks/stages. See [`docs/dev/engine-gen.md`](../dev/engine-gen.md) §9.

## The pipeline

```text
<name>.dsp ──cyfaust (cpp backend)──▶ faust_kernel_<name>.h   (class mydsp in namespace spotykach::<ns>)
                  make faust-kernels                │
   reverb_engine.cpp  ─ placement-new into arena ───┘ + CaptureUI binds zones to roles
```

`make faust-kernels` reads `FAUST_KERNELS` in the root `Makefile` - one `<dir>:<namespace-prefix>:<name>` spec per kernel - and for each compiles `<dir>/<name>.dsp` to `<dir>/faust_kernel_<name>.h`. cyfaust runs only at codegen time on the host; the generated headers are checked in, so a normal `arm-none-eabi-g++` build needs no cyfaust.

cyfaust's cpp backend has no class-rename flag - every kernel is `class mydsp`. To let several coexist in one binary, `faust-kernels`, per kernel: wraps the generated class in its own namespace `spotykach::<ns>`, rewrites the generated `__mydsp_H__` include guard to a per-kernel name (else the first kernel's guard suppresses the rest), and hoists the kernel's own `#include`s to global scope (a namespaced `#include` would pull `<cmath>` into the namespace). The class's unqualified `dsp`/`UI`/`Meta` base types then resolve to **`src/engine/faust_arch.h`** - a hand-written, MIT-licensed arch shim, so Faust's GPL-with-exception architecture headers are not vendored.

## The wrapper (what you hand-write)

The kernel is pure DSP; the engine `.cpp` wires it to the platform:

- **Arena construction.** A reverb's state is hundreds of KB of comb/allpass/FDN delay lines, which Faust emits as fixed member arrays of `class mydsp`. Held as a static value member that overflows `SRAM` (a static Zita member overflows by ~300 KB). So **placement-new each kernel into the SDRAM arena at `init()`** (the pattern reso uses for Rings); only pointers live in the engine. All kernels are built up front and only the **active** one's `compute()` runs per block - combining algorithms into one Faust `process` would run every one every sample (Faust has no branch elision).

- **Knob binding.** A generic `CaptureUI : public UI` (shared in `src/engine/faust/faust_capture.h` - one implementation serves the generated path, reverb, and tape) walks each kernel's `buildUserInterface` and captures the `FAUSTFLOAT*` control zones by label, filling a Role table from a small per-kernel **bind list** (label -> role, plus the enclosing box where a label repeats). The platform's 0..1 knob is then linear-mapped into each slider's native range captured at bind time - so each algorithm's units (plate 0..1, hall RT60 seconds / damping Hz / pre-delay ms) just work with no per-algorithm scaling code.

- **Selection / display.** Multiple algorithms select via `CapAux` + Alt+PITCH (`round(v*(N-1))`, re-applying cached knob values to the newly-active kernel); `render` meters the output.

## Memory and footprint

As with all engines, arena placement-new keeps `SRAM` (data) flat regardless of delay-line size, so **`SRAM_EXEC` (code) is the binding constraint**. Measured (Faust 2.85.5, `-O2`, from `-Wl,--print-memory-usage`). **`SRAM_EXEC` is 260 KB** (`linker/alt_sram.lds`); the byte counts below were taken when the region was 186 KB, so the percentages have been re-derived against 260 KB and the originals kept for reference:

| Build | SRAM_EXEC | % of 260 KB | (was, of 186 KB) | SRAM (data) |
|---|---|---|---|---|
| passthrough (platform floor) | 149,304 B | 56.1% | 78.4% | ~52 KB |
| reverb, Dattorro only | 155,912 B | 58.6% | 81.9% | 52,104 B |
| reverb, Dattorro + Zita | 175,696 B | 66.0% | 92.3% | 52,088 B |
| Zita as a static member (counter-example) | - | - | - | 638 KB -> SRAM overflow |

The old conclusion here - "at ~92% a third sizable algorithm would need `-Os` or one dropped" - has been overtaken by the region change *and* by events: the shipped `reverb` now carries **three** algorithms (plate + hall + Greyhole) at **69.8%** (185,720 B, measured 2026-08-31). It is still built `-Os`, but see [`../dev/reverb-impl.md`](../dev/reverb-impl.md) - that override works around an `-O2` overflow of the old 186 KB region and is worth re-testing.

## Setup, build, add

cyfaust lives in the repo-local `.venv` (gitignored):

```text
python3 -m venv .venv && .venv/bin/pip install cyfaust   # one-time
make ENGINE=reverb              # build; the link prints SRAM_EXEC usage
make ENGINE=reverb program-dfu  # flash (device in DFU first)
make engine-reverb              # one-shot: clean + build + flash
make faust-kernels              # regenerate faust_kernel_*.h for every spec in FAUST_KERNELS
#   CYFAUST_PY=/path/to/python  # pin a different libfaust version
```

**Add an algorithm:** drop `<name>.dsp` in the engine dir, append its spec to `FAUST_KERNELS`, bump the engine's kernel count, and register a bind table + concrete kernel type in the engine `.cpp`. Then re-check `SRAM_EXEC`.
