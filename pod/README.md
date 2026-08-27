# Daisy Pod — Csound harness

A standalone, quick-iteration target that runs the real `CsoundEngine` on a bare **Daisy** (Pod /
Seed), without the spotykach platform and front panel. A Pod is faster to flash and poke at than the
full firmware, so this is a handy sandbox for working on Csound synthesis, opcodes, and CPU behaviour
before changes land in the actual engine.

**This is not part of the spotykach firmware** and is **not** covered by `make test`. The shipping
engine lives in `src/engine/csound/` and is built with `make engine-csound` (see
[`docs/engines/csound.md`](../docs/engines/csound.md) and [`docs/dev/csound-impl.md`](../docs/dev/csound-impl.md)).

## What it does

`harness.cpp` stands in for the platform: it builds an `EngineContext`, `init()`s `CsoundEngine`,
drives `process()` from the audio callback, and forwards the Pod's two knobs to `set_param()`
(knob 1 → PITCH/pitch, knob 2 → MIX/level). It compiles the **actual** engine
(`../src/engine/csound/csound_engine.cpp`), so it exercises the real `IEngine` code path.

## What it does NOT exercise

The harness injects no platform services, so it is a **synthesis / opcode / CPU sandbox**, not a full
repro of the engine's features:

- **No SD card** (`ctx.stream = nullptr`) → only the built-in orchestra; no `/csound/*.csd` patch
  bank and no Alt+PITCH selector / live recompile.
- **No MIDI** wiring (the `MidiNote` path is untested here).
- **No transport/clock or QSPI-settings** services.

## Build & flash

Prerequisite (once): build `libcsound.a` with [`scripts/fetch_csound.sh`](../scripts/fetch_csound.sh),
same as the main engine. Then:

```text
cd pod
make                 # build the BOOT_QSPI harness
make program-dfu     # flash over USB DFU (catch the bootloader's DFU window; tap RESET after)
```

The Makefile's `../src`, `../lib/{libDaisy,DaisySP}`, and `../thirdparty/csound/Daisy` paths assume
this directory sits at the repo root (one level deep) — keep it there.

## Two important differences from the spotykach build

1. **It replaces the bootloader.** The harness uses the stock **Daisy v5.4** bootloader
   (`dsy_bootloader_v5_4.bin`) and the Csound-port QSPI linker script. Flashing it **overwrites the
   spotykach bootloader** — reflash the spotykach bootloader to go back to the SRAM engines.

2. **Different heap model.** The Pod puts Csound's heap straight in SDRAM via the port's
   `STM32H750IB_qspi_custom.lds` (newlib `malloc`). The spotykach build instead uses the dual-heap
   free-capable pool (`csound_alloc.cpp` + `--wrap`). So `csound_heap_arm()` is a **no-op** here
   (defined in `harness.cpp`), and the allocator tests/notes don't apply to this target.

## Caveat

`harness.cpp` predates the SD/MIDI/selector work and isn't kept in lockstep with the engine — if a
build breaks after engine changes, it's usually a new symbol the harness needs to stub or a context
field to set. It builds the real engine, so it's worth keeping in sync as a fast hardware loop.
