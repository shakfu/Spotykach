# Engine types (ways to build an engine)

Spotykach hosts one swappable DSP **engine** behind a fixed platform (see `../engines/README.md` for the `IEngine` contract, the transport, and the knob grammar every engine shares). There are **three ways to author that engine**; this folder documents each as a method. The per-engine pages under `../engines/` document the actual shipped implementations.

| Method | You write | Codegen | Examples | Doc |
|---|---|---|---|---|
| **Native C++** | the DSP *and* the `IEngine` wrapper, by hand | none | granular, delay, edrums, reso, passthrough; csound + chuck (runtime VMs, QSPI) | [cpp.md](cpp.md) |
| **Faust (cyfaust)** | DSP in a `.dsp`; the `IEngine` wrapper + knob binds by hand | `make faust-kernels` -> `faust_kernel_*.h` | reverb, tape (tapefx) | [faust.md](faust.md) |
| **gen~ (gen-dsp)** | a Max/MSP `gen~` patch + a small JSON manifest (the knob map); no C++ | `make gen-engine` -> a whole engine dir | gigaverb | [gen.md](gen.md) |

All three end at the same place: a class implementing `IEngine` (`init`/`prepare`/`process` + opt-in `capabilities`), selected at build time via `ENGINE=`. They differ in **how much you hand-write** and **where the DSP comes from**.

## What the methods share

- **The contract.** Every engine - hand-written, Faust, or gen~ - implements `IEngine` and is registered in `src/engine/engine_select.h` + the root `Makefile`. The platform only ever sees `IEngine`.

- **The arena.** Large DSP state (delay lines, resonators, reverb tanks) is placement-allocated into the injected `EngineContext` SDRAM arena at `init()`, never held as static members - otherwise it lands in `.bss` and overflows the 252 KB `SRAM` region. Consequence across all methods: **`SRAM_EXEC` (code), not `SRAM` (data), is the binding budget** (260 KB per `linker/alt_sram.lds`; `pstretch` gets its own 200 KB / 312 KB split. A big engine may need its branch built at `-Os`).

- **Host-side codegen.** The Faust and gen~ generators (cyfaust, gen-dsp) run only on the host at codegen time and live in the repo-local `.venv`; the firmware build is plain `arm-none-eabi-g++` over checked-in generated files. Native C++ has no codegen step.

## Choosing a method

- **Native C++** - maximum control and the only option for engines that are not a pure signal graph: sequencers (edrums subscribes to the transport), sample/SD streaming (tape), dual-deck instruments, custom displays, or vendoring an existing C++ DSP library (reso wraps Mutable Instruments Rings). Its extreme is the **runtime-compiler engine**: vendoring a whole audio language/VM so the *patch* (not the firmware) defines the sound - csound and chuck, which run from QSPI flash with their own SDRAM heap and an SD patch bank (see [cpp.md](cpp.md) "Vendoring a runtime VM"). Every capability-rich engine is native. Most code to write; no generator can express these.

- **Faust (cyfaust)** - closed-form signal-processing DSP you would rather write declaratively, especially with several interchangeable algorithms (reverb ships a plate + a hall). You still hand-write the wrapper, so you keep full control of knob mapping, selection, and display. The generated kernel is pure DSP; you bind it.

- **gen~ (gen-dsp)** - you already have, or prefer to patch in, a Max `gen~` graph. Fastest path from a gen~ export to a running engine and the least firmware code (the wrapper is fully generic - you only write a `knobs` JSON manifest, same declarative method as Faust, no C++). Currently capability-minimal: stereo audio + knob params, no custom display/MIDI/CV yet (`capabilities() = 0`).

A single engine can mix methods: **tape** is a native C++ engine (SD-streaming decks) that also embeds a Faust `tapefx` kernel for its wow/flutter + hysteresis - it appears under both Native C++ and Faust.
