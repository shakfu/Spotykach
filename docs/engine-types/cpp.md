# Native C++ engines (hand-written `IEngine`)

The default way to build an engine: write the DSP **and** the `IEngine` wrapper directly in C++, with no code generation. It is the most code but the most control, and the only option for engines that are not a pure signal graph - sequencers, sample/SD streaming, dual-deck instruments, custom displays, or wrappers around an existing C++ DSP library.

**Implementations:** [granular](../engines/granular.md) (the full instrument), [delay](../engines/delay.md), [edrums](../engines/edrums.md) (transport-sequenced), [reso](../engines/reso.md) (vendors Mutable Instruments Rings), [passthrough](../engines/passthrough.md) (the reference minimum), and the **runtime-compiler engines** [csound](../engines/csound.md) and [chuck](../engines/chuck.md) (vendor a whole audio language/VM; the patch defines the sound — see "Vendoring a runtime VM" below). For the contract, transport, and knob grammar these all share, see [../engines/README.md](../engines/README.md).

## The shape

Subclass `IEngine` (`src/engine/iengine.h`), implement the three required methods, override only the optional regions you support, and advertise them via `capabilities()`:

```cpp
class MyEngine : public IEngine {
  void init(const EngineContext& ctx) override;   // SR, block size, SDRAM arena, clock, transport
  void prepare() override;                          // non-RT main-loop housekeeping
  void process(const float* const* in, float** out, size_t size) override;  // 48 kHz / 96-frame block
  Capabilities capabilities() const override { return CapOwnDisplay | CapDualDeck; }
  void set_param(ParamId, DeckRef::Ref, float) override;   // platform knob -> your DSP (value is 0..1)
  void render(DisplayModel&) override;             // if CapOwnDisplay
};
```

`set_param` arrives normalized 0..1; the engine scales into its own units (delay: `v * 0.95` feedback, `exp2f((v-0.5)*2)` ratio; reso: `v` straight into Rings `patch.damping`). Categorical switches (mode, routing, model) come through `set_config(ConfigId, ...)`; transport-synced engines subscribe via `ctx.transport->set_on_tick(cb)` (edrums steps its sequencer there). See `../engines/README.md` for the full physical-knob -> `ParamId` routing and which `ParamId`s reach a single-deck engine.

## Owning the DSP

Hold your DSP as members, or behind an opaque `Impl*` (PIMPL) when a dependency would leak into the engine header. **PIMPL is mandatory, not stylistic, when a dependency declares a global symbol that collides with the composition root.** reso uses it because `stmlib/stmlib.h` declares a global `namespace impl`, and `src/app.cpp` (which includes the engine header via `engine_select.h`) has a `static AppImpl impl;` - if Rings/stmlib reach the header, the two `impl`s collide and `app.cpp` fails to compile. Keeping the third-party types inside `reso_engine.cpp` (`struct ResoEngine::Impl`) avoids it.

## Memory and block size

- **Allocate big state from the arena, not static members.** Large buffers (delay lines, resonators, reverb tanks) placement-new'd into `ctx.arena` keep `SRAM` flat; held as static members they overflow `SRAM` (see [README.md](README.md)). reso placement-news two ~108 KB `rings::Part`s + two 64 KB reverb buffers into the arena at `init()` via the `Arena` bump allocator (`src/engine/arena.h`).

- **Chunk if your DSP's max block < 96.** The platform block is 96 frames. reso runs Rings at `kMaxBlockSize = 24`, so it processes the block in four <=24-frame chunks.

## Vendoring third-party DSP

When the DSP is an existing C++ library (reso/Rings), colocate it under `src/engine/<name>/thirdparty/`, **trimmed to the exact compile closure** (only the `.cc`/`.h` files the engine pulls, computed from the compiler's `.d` output - reso is ~34 files vs ~50 MB of full checkouts), and scope its `-I` and sources to that engine's build with a make var (reso's `RESO_TP`/`RESO_INC`). Retain the upstream licenses. Three recurring build gotchas, all visible in the reso `Makefile` branch:

- **`-DTEST` host-only** - stmlib gates its ARM `ssat`/`usat`/`vsqrt` asm behind `#ifndef TEST`; the desktop test build defines `TEST` (portable C), the firmware build must not.

- **`-DM_PI`** - strict `-std=c++17` does not expose `M_PI` from `<cmath>` on `arm-none-eabi`.

- **`OPT = -Os` per engine** - a large DSP (Rings ~30 KB) overflowed the **then-186 KB** `SRAM_EXEC` at `-O2`; scope `-Os` to that engine's branch so the others stay `-O2`. (The region is 260 KB today and reso links at 69.1% *with* `-Os`, so the original overflow likely no longer applies - the technique is still the right one when an engine genuinely does not fit.)

## Vendoring a runtime VM (csound, chuck)

[csound](../engines/csound.md) and [chuck](../engines/chuck.md) are the extreme of the above: the vendored "library" is an entire audio **language + runtime compiler/VM**, and the engine is a thin hand-written `IEngine` wrapper that drives it. The **patch** - a `.csd` / `.ck` text loaded from the SD card - defines the sound, so the same firmware plays any program and switches between them live. They are still Native C++ engines (you write the wrapper), but they diverge from the in-binary DSP engines above in three structural ways, all because the runtime is ~1-2 MB of code:

- **QSPI build, not `ENGINE=`.** The runtime's code is far over the 186 KB `SRAM_EXEC` budget, so these are `BOOT_QSPI` images that execute in place from QSPI flash (`alt_qspi*.lds` + a `SCB->VTOR` inject), built via a dedicated `make engine-csound` / `make engine-chuck` target rather than the plain `ENGINE=` path. Code size moves off the binding budget (it lives in 8 MB of flash); the cost is execute-in-place is slower than SRAM, so CPU headroom - not code size - becomes the limit.

- **Own SDRAM heap, not the arena.** A runtime VM mallocs megabytes dynamically (compile, instantiate, free), so instead of placement-new into `ctx.arena` they route the C `malloc` family to a dedicated coalescing SDRAM pool via linker `--wrap` (`chuck_alloc.cpp` / the Csound equivalent over a `.sdram_bss` array), armed after `_hw.Init()`. `ctx.arena` is opted out (`-DSPK_NO_ENGINE_ARENA`). The platform heap stays in SRAM.

- **Live recompile behind the reload gate.** Switching patches recompiles/swaps VM state, which races the audio ISR; both engines gate the live instance out of the audio path during the swap (the engine-agnostic `ReloadGate`) and stream patches from the SD bank (`-DSPK_USE_STREAM`). ChucK additionally keeps one persistent VM and caches compiled programs to bound memory across swaps (ChucK never frees its type system - see [`../dev/chuck-impl.md`](../dev/chuck-impl.md)); Csound destroys + recreates its instance. Internals: [`../dev/csound-impl.md`](../dev/csound-impl.md), [`../dev/chuck-impl.md`](../dev/chuck-impl.md).

## Registering and building

Add the engine to the build in the usual places (see any engine's "Files" section):

- `src/engine/engine_select.h` - `#elif defined(SPK_ENGINE_MYENGINE)` -> `using ActiveEngine = MyEngine;`

- root `Makefile` - an `ENGINE=myengine` block setting `-DSPK_ENGINE_MYENGINE` + `ENGINE_SOURCES`, and an `engine-myengine` one-shot (clean + build + flash) target.

- `host/` - a headless test (`test_myengine.cpp`, `make -C host test-myengine`); the platform/engine boundary (`make check-boundary`) forbids `hw/ui/memory/transport` from including engine DSP.

```text
make -j8 ENGINE=myengine        # the link prints SRAM_EXEC usage
make ENGINE=myengine program-dfu
make engine-myengine            # one-shot: clean + build + flash (device in DFU mode)
```

## Reference engines

- **passthrough** - the minimum: header-only, `process` copies in->out, `render` meters. Start here.

- **delay** - reads the transport tempo; clean `set_param` scaling.

- **edrums** - subscribes to transport ticks, sequences four voices, `set_config` routing, two-drums-per-deck.

- **reso** - PIMPL, vendored Rings, arena placement-new, `-Os`, block chunking, `CapAux` model select.

- **csound / chuck** - the runtime-compiler pattern: a thin wrapper around a vendored audio VM, QSPI build, own SDRAM `--wrap` pool, SD patch bank + live recompile behind the `ReloadGate`. The patch defines the sound.
