# Dev notes — ChucK as an sk-engines engine (`ENGINE=chuck`) — ROADMAP

> **Status: M0–M3 DONE + HARDWARE-VERIFIED; patch-swap memory leak RESOLVED (2026-06-22).** ChucK boots, > compiles, runs the VM, makes sound, and responds to the knobs on the cased spotykach (and a bare Pod). > `M0`: `libchuck.a` cross-builds + links (`scripts/fetch_chuck.sh`, pinned `chuck-1.5.5.8`). > `M1`: Pod tone from QSPI — the `SawOsc => LPF => dac` drone plays, knobs drive `speedA`/`mixA`. `M2`: > `ChuckEngine : IEngine` (`src/engine/chuck/`) wired into both the Pod harness (`make -f Makefile.chuck`) > and the full firmware (`make engine-chuck`); flashed on the cased unit, boots/runs/sounds. **`M3`** — > the SD `chuck/<n>.ck` patch bank + Alt+PITCH live swap — is hardware-verified: boot auto-load, the > selector, and live switching all work with a card of `examples/chuck/*.ck` (after two fixes: the > `ENGINE=chuck` branch was missing `-DSPK_USE_STREAM` so the bank saw no card, and the 48 MB engine arena > is opted out via `-DSPK_NO_ENGINE_ARENA` since ChucK brings its own pool — SDRAM 97% -> 22%). > **The live-swap memory leak is FIXED (2026-06-22):** the swap mechanism is now **one persistent VM + > a compile-once bytecode cache** (NOT the old destroy+recreate or `CK_MSG_CLEARVM` reset, both of which > leaked ChucK's never-freed type system). Memory rises once per distinct patch then plateaus; confirmed > on the cased unit by ear + the panel pool meter. See the "Patch-swap memory leak — RESOLVED" section > below for the full root-cause + fix writeup. An **on-panel meter** (`METER=1`): ring A = CPU load, > ring B = SDRAM pool usage (the leak readout); production build's rings = stereo dB output meter. The one > remaining rough edge is slot 5 (a CPU-heavy patch that the overrun safeguard mutes — not a leak).

---

## Patch-swap memory leak — RESOLVED (hardware-confirmed 2026-06-22)

**TL;DR.** Live patch-swapping leaked SDRAM cumulatively until the pool exhausted and patches that played fine alone began overrunning / latching the double-red panic. Root cause was **inside ChucK 1.5.5.8, not our code**: ChucK never frees its type system on VM teardown (it relies on process exit), so *every* "tear down and reload" strategy we tried leaked. The fix stops fighting it: **keep ONE ChucK VM for the whole session (never destroy it) and compile each distinct patch at most once (cache the emitted bytecode, re-spork it on subsequent swaps)**. Memory now rises only the first time each patch is visited and then plateaus, flat across unlimited cycling. Verified on the cased spotykach via the panel pool meter (ring B steps once per patch, then holds) and by ear (every patch recovers memory on swap). The one patch that still misbehaves — slot 5 — is a *separate, CPU* problem (see the last subsection), not memory.

### The symptom

Cycling patches with the Alt+PITCH selector (SD slot → SD slot, repeatedly) progressively degraded the unit. Each patch played correctly in isolation, but after several swaps audio began glitching and the CPU-overrun safeguard latched (solid-red rings on both decks). The degradation was **cumulative across swaps** and cleared **only on power-cycle**. It reproduced in the **production** build (`make engine-chuck`, no METER), so it was a genuine resource leak, not a metering artifact.

### Root cause: ChucK's type system is immortal by design

ChucK builds its built-in type system (every UGen/STK class, the global namespace) into a `ChucK` instance at `init()`, and **never frees it on teardown** — it assumes the host process will exit and let the OS reclaim everything. On a bare-metal device that never exits, every VM-create/destroy or recompile cycle that touches the type system leaks. The evidence in `thirdparty/chuck/src/core`:

- **`Chuck_Env::cleanup()`** (`chuck_type.cpp` ~264, run from `~Chuck_Env`) unlock-deletes only the ~25 *base* types (`void`/`int`/`float`/…/`Object`/`Type`) and carries a literal `// TODO: free all types in the Env`. Every *registered* type (the UGens, the STK instruments — the multi-MB bulk), which lives in `global_nspc`, is left alone.

- **`global_nspc`** is `CK_SAFE_ADD_REF`'d when the Env is built (`chuck_type.cpp` ~151) with **no matching release** in `cleanup()` or `~Chuck_Env` — so its refcount can't reach 0.

- **`Chuck_Context::~Chuck_Context()`** (`chuck_type.cpp` ~6532) releases its namespace **only `if(has_error)`**; on a *successful* compile it deliberately keeps it, with the comment "the nspc stays as part of the type system, since many things have been added to it." So each successful `compileCode()` also retains a context's worth of namespace.

Net: the global type system is leaked once per VM creation, and a user context is leaked once per compile. Neither is ever reclaimed within a running process.

This is **why every teardown strategy we tried failed** — they differ only in *how much* they re-leak:

| # | Strategy | What it did | Why it still leaked |
|---|----------|-------------|---------------------|
| 1 | `removeAllShreds()` + `compileCode()` on one VM (original M3) | freed the shreds, recompiled | each `compileCode` retains a context (`~Chuck_Context` keeps `nspc`) — small but unbounded over swaps |
| 2 | `CK_MSG_CLEARVM` reset + `compileCode()` (one VM) | `removeAll` + `clear_user_namespace` + `cleanup_global_variables` | CLEARVM clears *user* types, but not the retained compile contexts / emitted code — KB-scale per swap |
| 3 | Pool 12 MB → 40 MB | more headroom | only delays exhaustion; not a fix (kept anyway — free headroom after the arena shrink) |
| 5 | VM destroy+recreate (`delete _ck; new ChucK()` per swap) | full teardown + fresh VM each swap | **worst case** — re-leaks the *entire* built-in type system (multi-MB) every swap; this is the one the panel meter caught climbing |

(#4 was the separate observer-effect metering bug — see below.) Note the destroy+recreate path is otherwise sound: `Chuck_VM::stop()` (`chuck_vm.cpp` ~547) clears `m_is_running` synchronously, so `~ChucK`'s `while(vm->running()) ck_usleep()` busy-wait (`chuck.cpp` ~1028) can't hang on bare metal; and `o_isGlobalInit` persists across cycles (the dtor never calls `globalCleanup()`), so repeated create/destroy is stable. It simply can't reclaim the type system, so it leaks the most.

### Confirming it on hardware: the per-swap pool meter

Before committing to a fix we added a **non-destructive** pool-usage readout so we could *measure* the leak (which resource, what rate) instead of guessing — the roadmap's "classify the leak" step. `ChuckEngine::note_pool_usage()` samples `CsoundPool::used_bytes()` **once per swap**, inside `do_reload` between the ReloadGate `take()` and `publish()` — i.e. with the VM out of the audio path, so the walk is race-free and entirely off the per-block hot path. (This is the deliberate opposite of the earlier **observer-effect bug** (#4): a pool meter that walked the whole pool under `PRIMASK` *every render* starved the audio ISR and itself triggered false overruns, worsening as the pool filled. That walk was removed; the new sample is swap-time only.) `csound_pool.h` is **unchanged** (no risk to the shared Csound allocator); `chuck_alloc.cpp` just exposes `chuck_pool_used()` / `chuck_pool_capacity()`.

Readouts:

- **Cased unit (no SWD): panel, `make engine-chuck METER=1`.** Ring A = CPU load. Ring B = SDRAM pool usage — bright arc = live bytes right after the last swap, dot = high-water mark. Under destroy+recreate the arc + dot **climbed multi-MB per swap and never recovered**, which both confirmed the leak and confirmed it was *memory* (not fragmentation — fragmentation would drop the arc while the dot stayed high).

- **Bare Pod: SWD globals** `g_chuck_pool_used_kb` / `_peak_kb` / `_cap_kb` / `g_chuck_swaps` (always built). `openocd-attach` then `mdw <addr-from-build/spotykach.map>`.

### The fix: one persistent VM + compile-once cache

Two levers, both required, both in `do_reload` / `load_program` (`chuck_engine.cpp`):

**Lever 1 — one persistent VM, never destroyed.** `build_vm()` now creates the `ChucK` **once** at `init()` (`new ChucK` / `setParam` / `init` / `start`, *no* program compiled) and the engine keeps it for the whole session. Because the VM is never torn down, the built-in type system is built exactly once and can never be re-leaked — this removes the entire multi-MB-per-swap term. A swap no longer touches the VM's existence, only its *shreds*:

- `do_reload` takes the ReloadGate (ISR sees null → outputs silence during the swap), then **stops the current patch synchronously**. ChucK's `removeAllShreds()` only *flags* removal (`m_asap_remove_all_shreds`, `chuck_vm.cpp` ~1471); the flag is enacted at the **top of the next `compute()` tick** (`chuck_vm.cpp` ~572 → the protected `removeAll()` ~1314 → `free_shred()` → `detach_ugens()` ~1989, which disconnects the shred's UGens from `dac`). So we must enact it **before** sporking the new patch — otherwise that deferred remove-all would also kill the just-sporked shred, and the old patch's UGens would keep ticking on `dac` (a CPU leak). We force it with a **one-frame `_ck->run(_inbuf, _outbuf, 1)`** under the gate: one tick runs `removeAll()` at the top, freeing + detaching the old shred before any new spork. (`removeAll`/`free_shred` are `protected`, hence the flush trick rather than calling them directly.)

**Lever 2 — compile each distinct patch at most once (cache the bytecode).** The remaining leak is per-*compile* (the retained context). `load_program()` eliminates repeat compiles:

- ChucK's `compileCode()` does parse/type-check/emit (`compiler->compileCode`, `chuck.cpp` ~1302) → `vm_code = compiler->output()` (~1306) → `vm->spork(vm_code, …)` (~1320). `Chuck_Compiler::output()` (`chuck_compile.cpp` ~1161) returns `this->code`, which is the **freshly emitted** `Chuck_VM_Code` for that compile (assigned at ~761). So after a successful compile we grab that pointer, `add_ref()` it (`Chuck_VM_Object::add_ref`, `chuck_oo.h` ~81), and **pin it in a cache** keyed by slot (`_builtin_code` + `_slot_code[kMaxChuckSlots]`, via `cache_cell()`).

- On a swap to an **already-visited** patch, `load_program()` finds the pinned code and **re-sporks it directly** — `_ck->vm()->spork(code, NULL, TRUE)` (public, `chuck_vm.h` ~640) — with **no recompile**, so no new context is created and nothing new is retained. Re-sporking cached code is ChucK's normal "run this program again" path; the new shred gets a fresh stack and fresh UGen instances, and `reseed_globals()` replays the current knob positions into it.

- Lifetime: the pinned `add_ref` keeps each cached `Chuck_VM_Code` alive even as its shreds come and go (when a shred is removed it releases *its* ref; ours holds the object at refcount ≥ 1). The cache is session-lifetime (never evicted), so total compiles — hence total retained contexts — are bounded by the number of **distinct** patches (≤ `kMaxChuckSlots + 1`), independent of how many times you swap.

**Resulting memory model.** Steady state is a fixed, bounded footprint: one type system (built once at boot) + one retained context per distinct patch (compiled the first time it's selected). After every slot has been visited once, usage is **flat forever** regardless of further cycling. On the meter: ring B steps up on each patch's first load, then holds — exactly what was observed.

**The swap, end to end (`do_reload`):** gate `take()` → `removeAllShreds()` + 1-frame `run()` (stop + detach old, synchronously) → `load_program(target)` (re-spork cached code, or compile-once + pin) → `reseed_globals()` (replay knobs) → clear `_panic`/overrun counter → `note_pool_usage()` (sample, ISR still quiesced) → gate `publish(_ck)` (re-arm the ISR with the same VM).

### Caveats / known limitations (documented in `chuck_engine.h`)

- **Cached code survives SD rescans.** Editing a slot's `.ck` file on the card mid-session won't take effect until power-cycle (the slot's bytecode is already pinned). Fine for a fixed patch bank.

- **Shared user namespace across patches.** Because there's no per-swap reset, all compiled patches share one user namespace; two patches that define the *same top-level type name* would collide on the second one's compile. Fine for the curated examples; a real concern only if users author colliding classes.

- **Broken patches aren't cached.** A slot that fails to compile produces no code to pin, so it recompiles (and retains one context) on each visit, then falls back to the built-in. Bounded by how often a *broken* slot is selected; acceptable.

### Slot 5 / heavy patches: a separate CPU axis, not memory

After the leak fix, all patches recover memory across swaps **except slot 5**, which still latches the double-red panic. This is the **CPU-overrun safeguard doing its job**, not a leak: slot 5 is the heavy FM/STK patch (multiple `HnkyTonk` 4-op voices + reverb) whose `ck->run()` can't finish within the audio block, so `_panic` mutes it to keep the controls alive (see "CPU-overrun safeguard" in the memory note / `process()`). It's a headroom problem, fixable only by lightening the patch (fewer voices, drop the reverb/chorus) or raising the block size — orthogonal to the memory work here. The leak fix is complete and independent of it.

### Current code state (all uncommitted on branch `dev`)

- `src/engine/chuck/chuck_engine.{h,cpp}` — M3 bank + selector; **one persistent VM + compile-once cache**: `build_vm()` (create/init/start ONCE at init, no compile); `compile_and_spork()` (the only compile path, bring-up-captured); `load_program()` (re-spork cached `Chuck_VM_Code` or compile+pin, keyed by slot via `cache_cell()`, built-in fallback); `do_reload()` (gated take → `removeAllShreds` + 1-frame `run()` flush → `load_program` → reseed → `note_pool_usage` → republish, VM never destroyed); `_builtin_code`/`_slot_code[]` pinned-code cache; `note_pool_usage()` (per-swap pool sampling → members + `g_chuck_pool_*` SWD globals); CPU-overrun safeguard (`_panic` mute-and-wait, always-on, DWT-timed in `process()`); METER meter (ring A CPU / ring B pool-usage arc + high-water); production stereo dB output meter; bring-up capture (`g_chuck_init_*`).

- `src/engine/chuck/chuck_alloc.cpp` — 40 MB `--wrap` SDRAM pool (`CsoundPool`) + PRIMASK `CritSec`; `chuck_pool_used()`/`chuck_pool_capacity()` diagnostics (used_bytes walk, swap-time only).

- `src/engine/chuck/chuck_patch.h` — SD `chuck/<n>.ck` selector (host-tested).

- `scripts/fetch_chuck.sh` + `thirdparty/chuck/Daisy/shim/chuck_posix_stubs.c` — `random()` xorshift PRNG seeded from DWT CYCCNT (fixes frozen `Math.random*`). `libchuck.a` was rebuilt with this (`ar r`).

- `Makefile` `ENGINE=chuck` branch — `-DSPK_USE_STREAM` (SD bank) + `-DSPK_NO_ENGINE_ARENA` (arena shrink, shared with csound). `src/hw/buffer.sdram.cpp` — arena → 64 KB token under `SPK_NO_ENGINE_ARENA`.

- `examples/chuck/0..6.ck` + `README.md` (desktop-`chuck`-compile-checked); `host/test_chuck_patch.cpp`.

- Build: both variants link clean — production SDRAM 65.82% / QSPI 17.02% / SRAM 65.22%; host suite green.

- Nothing committed this whole effort — the user commits.

---

> > **Bring-up took four bug fixes** (`-u _printf_float`, `_ck->start()` before audio, a PRIMASK guard on > the non-reentrant SDRAM pool, and a sane knob cadence) — none ChucK-on-Cortex-M incompatibilities. The > full story, the SWD flash/inspect tooling, and the open items live in > [`chuck-pod-poc.md`](chuck-pod-poc.md) (read it for current state). The roadmap below (M3 SD bank, M4 > MIDI) and the risk analysis are still accurate; the `METER=1` CPU-headroom pass (risk #1) is the main > open measurement — `ck->run(256)` is CPU-heavy (~near the block budget).

> This is a design + roadmap for embedding > the [ChucK](https://chuck.stanford.edu) language as a swappable engine, modelled directly on the > already-shipping **Csound** engine ([`docs/dev/csound-impl.md`](csound-impl.md)). ChucK is the same > *class* of problem — embed a runtime audio compiler/VM as a QSPI app behind `IEngine` — so most of > the hard, already-solved Csound machinery (QSPI boot, the dual-heap SDRAM allocator, the lock-free > live-recompile handoff, the SD patch bank) is reused, not re-invented. Read the Csound doc first; > this one is written as a **delta** against it.

[ChucK](https://github.com/ccrma/chuck) is a strongly-timed audio programming language with a runtime compiler + virtual machine: a program (`.ck` text) is compiled at runtime into shreds (concurrent strongly-timed processes) that the VM schedules sample-accurately against a UGen graph. Like Csound, **the patch — not the firmware — defines the sound**: you load `.ck` programs from the SD card, switch between them live, and play them from the panel knobs and over MIDI.

---

## Why this is feasible (the short version)

1. **ChucK is designed to be embedded.** Its source is split into `src/core/` (the language, VM, UGens — no I/O assumptions) and `src/host/` (the CLI + RtAudio driver). You compile `core/`, drop `host/`, and drive the `ChucK` host class yourself — the *same* core/host split that made the Csound port tractable.

2. **The host API maps almost 1:1 onto `IEngine`** (see the table below).

3. **The audio I/O boundary is single-precision.** `chuck_def.h` defines `#define SAMPLE float` (the 64-bit sample path is a commented-out opt-in), so `run()` exchanges `float` buffers — no double marshalling at the block boundary, FPU-friendly on the Cortex-M7.

4. **WebChucK is the proof of concept.** [WebChucK](https://github.com/ccrma/webchuck) compiles `core/` to WebAssembly and drives it from a **single-threaded** Web Audio worklet — no OS, no filesystem, no chugins, no on-the-fly (OTF) server, `compileCode()` from a string, `run()` per block. That is almost exactly the bare-metal contract we impose here, so WebChucK's emscripten build config is effectively a ready-made template for *which* `core/` files and `#define`s survive without an OS. **Mine the WebChucK Makefile/CMake before writing our own.**

---

## How ChucK maps onto IEngine

`ChuckEngine : public IEngine` (`src/engine/chuck/chuck_engine.{h,cpp}`) would override only what it uses, exactly like `CsoundEngine`. The host class is `ChucK` (`src/core/chuck.h`).

| `IEngine` | ChucK host API | Csound equivalent (already shipped) |
|---|---|---|
| `init(ctx)` | `new ChucK()` → `setParam(CHUCK_PARAM_SAMPLE_RATE, sr)` / `..._INPUT_CHANNELS` / `..._OUTPUT_CHANNELS` → `init()` → `compileCode(text)` | `csoundCreate` / options / `csoundCompileCSD` / `csoundStart` |
| `process(in,out,n)` | `chuck->run(in, out, n)` — **`SAMPLE = float`**, interleaved by channel-count; one call per block | de-interleave → `csoundPerformKsmps` → de-interleave |
| `set_param(id,deck,v)` | `chuck->globals()->setGlobalFloat("speedA", v)` (the program reads the global) | `csoundSetControlChannel(name, v)` |
| `handle_midi_note(ch,note)` | enqueue → in `process()` `setGlobalInt("midiNote", n)` + `broadcastGlobalEvent("noteOn")` (a `MidiNote`-style shred sporks a voice) | `csoundEvent(CS_INSTR_EVENT, …)` |
| Alt+PITCH live patch swap | one persistent VM: `removeAllShreds()` + 1-frame `run()` flush, then re-spork the patch's cached `Chuck_VM_Code` (compile-once) behind the `ReloadGate` | `ReloadGate` + `csoundDestroy` + recompile |
| `render(m)` | output-level meter + patch selector + run-state | identical |
| `capabilities()` | `CapAux \| CapOwnDisplay` | identical |

**The program is the param vocabulary.** Swap the `.ck`, swap the synth, no C++ change — same principle as the CSD `chnget` convention. The platform writes a fixed set of named globals (`speedA`/`mixA`/`sizeA`/`envA`/… + the `B` deck); the program opts in by declaring `global float speedA;` etc. and reading them in its synthesis loop.

### Built-in fallback program

Ship a compiled-in `kProgram` string (a `SinOsc`/`SawOsc => LPF => dac` drone reading the panel globals, plus a `MidiNote` shred sporked on the `noteOn` event) so a card-less unit always sounds and a bad SD program falls back — mirroring Csound's `kOrchestra`.

---

## What transfers verbatim from the Csound engine

Nearly everything hard about Csound is reusable with only names changed:

- **QSPI boot model.** `BOOT_QSPI` + `alt_qspi.lds` (code in QSPI flash `0x90040000`, data/bss in SRAM/DTCM) + the `init_priority(101)` `SCB->VTOR` inject (`spotykach_qspi_vtor.cpp`). ChucK's linked code is also far over the 186 KB `SRAM_EXEC` budget. The verified bootloader fact (the v2 bootloader runs QSPI apps; no bootloader change needed) carries over.

- **Dual heap.** Platform heap stays in SRAM (global ctors `malloc` before `_hw.Init()` powers the FMC); ChucK's heap is the 12 MB SDRAM pool reached via `--wrap=malloc/free/calloc/realloc`, armed *after* `_hw.Init()`. The free-capable coalescing allocator (`CsoundPool` in `csound_pool.h`) is engine-agnostic — rename to a shared `SdramPool` and reuse for sustained patch swapping. ChucK is STL/heap-heavy, so this is mandatory, not optional.

- **Lock-free handoffs.** `ReloadGate` (ISR↔main live-instance swap, seq_cst) and the SPSC `NoteQueue` (MIDI main→ISR) are reusable as-is — the live-recompile and MIDI-into-ISR hazards are identical.

- **SD patch bank + Alt+PITCH selector + boot auto-load + built-in fallback** — same UX and the same FatFs gotchas (relative paths, no leading slash; strip BOM/CRLF on load). `.ck` files instead of `.csd`, under `/chuck/0.ck` … `/chuck/7.ck`.

- **`scripts/fetch_chuck.sh`** modelled on `fetch_csound.sh`: fetch a pinned `ccrma/chuck` tag (the `~MB` source is gitignored, reproduced from upstream), cross-build `libchuck.a` + headers for cortex-m7 single-precision bare metal, install to `thirdparty/chuck/Daisy/{lib,include}` where the `ENGINE=chuck` Makefile branch points `-I` / link. The Makefile branch needs `APP_TYPE=BOOT_QSPI LDSCRIPT=linker/alt_qspi.lds` and `-DSPK_USE_STREAM` (for SD), exactly like the csound branch.

---

## The ChucK-specific risks (where it diverges from Csound)

These are the unknowns to retire on the Pod **before** committing to the spotykach build. Ordered by how much they could sink the effort.

1. **Double-precision UGen math (CPU headroom — downgraded after M0).** The I/O `SAMPLE` is `float`, but ChucK's language-level `float` is `t_CKFLOAT = double`, and many UGen `tick()` functions compute in double internally (phase, gain, freq). **Correction from M0:** the STM32H7 Cortex-M7 has a *hardware double-precision FPU* — the Daisy build flags are `-mfpu=fpv5-d16` (the `-d16` double-capable unit, not `-sp-`), confirmed in `lib/libDaisy/core/Makefile`. So ChucK's double math is hardware-accelerated, *not* software-emulated. It is still ~2× single-precision and uses more registers, and it stacks on the QSPI execute-in-place penalty (Csound's roadmap #4) — so CPU headroom is still the **first thing M1 must measure** (`METER=1`), but it is a tuning question, not a likely showstopper. Mitigations if it bites: larger block size, lighter UGens / capped shred count, pinning hot `.text` into SRAM.

2. **Threading / mutexes in the Globals Manager. — LARGELY RETIRED by M0.** ChucK's host↔VM global exchange uses a mutex, and `core/` references `pthread_t`/`pthread_mutex_t` (the OTF server, the main-thread hook, the globals queue). The fix is WebChucK's `-D__DISABLE_THREADS__`, which compiles those paths out — with it, the whole core compiles and links for cortex-m7 with **no pthread symbols** (M0 verified). Still disable OTF via params (`CHUCK_PARAM_OTF_ENABLE = 0`). The remaining question is runtime: with threads disabled, the globals queue is drained on the audio thread, so the host must enqueue global writes (mirroring the Csound `set_param`→channel pattern) rather than touching VM state across the ISR boundary — an M2 wiring detail, not a build blocker.

3. **Feature exclusions (structural, like Csound's libsndfile-off).**

   - **Chugins** (dynamically `dlopen`'d UGen plugins) — gone; bare metal has no dynamic loading. Only statically-linked UGens exist. (Biggest synthesis limit, same as Csound's plugin opcodes.)

   - **`SndBuf` / `WvIn` / `WvOut`** (disk sample I/O) — either wired to the platform `IStreamDeck` (a real project, deferred) or disabled. Synthesis UGens (oscillators, the STK instruments, filters, delays, the FFT/analysis UGens) remain.

   - **OSC / HID / serial / MIDI-device** UGens — gone; the host feeds I/O.

4. **Code size + opcode/UGen set.** Likely smaller than Csound's ~2 MB, but confirm `libchuck.a` links into the 8 MB QSPI image alongside `SPK_USE_STREAM`'s ~2 MB of SDRAM rings. Audit the statically-registered UGen set for this build and document it.

5. **Strongly-timed semantics vs a fixed block.** ChucK schedules shreds at sample accuracy *inside* `run(n)`; `n` = our platform block (256). This is fine (WebChucK does the same), but very fine `1::samp` timing across a 256-sample block is computed within the one `run()` call — verify dense per-sample shreds don't blow the ISR budget (related to risk #1).

---

## Memory & boot model (same as Csound)

- Code in **QSPI flash** (`0x90040000`, 8 MB), executed in place. Data/bss in SRAM/DTCM; ChucK heap in the 12 MB SDRAM pool; platform heap in SRAM.

- `alt_qspi.lds` (code + `.data` load-address moved `SRAM_EXEC → QSPIFLASH`).

- `SCB->VTOR = 0x90040000` inject (global ctor) — needed because the startup skips `SystemInit` under `BOOT_APP`; without it SysTick + audio DMA are dead.

- No console in the QSPI build — use audio as the debug signal (a known tone = boot/output OK; a distinct pitch = "compile failed"), or wire USB-CDC / SWO as Csound's notes describe. A SWD probe (attach-don't-flash, hardware breakpoints only — QSPI is read-only to the debugger) is the high-leverage tool for pre-`main` ctor crashes and `chuck->run()` return codes.

---

## Building `libchuck.a` (`scripts/fetch_chuck.sh` — DONE, M0)

`thirdparty/chuck/` is ChucK (pinned default `CHUCK_REF=chuck-1.5.5.8` of [`ccrma/chuck`](https://github.com/ccrma/chuck)), **gitignored**, fetched + cross-built by one script (modelled on `fetch_csound.sh`):

```text
scripts/fetch_chuck.sh             # fetch source + cross-build libchuck.a + self-verify with a link probe
scripts/fetch_chuck.sh --no-build  # just fetch + write the shim sysroot (build later)
```

Unlike Csound, ChucK has **no official Daisy port**, so the script *is* the port. What M0 established:

- **Source subset = WebChucK's** `EMSCRIPTENSRCS` (src/makefile), minus `host-web/chuck_emscripten.cpp` (we bring our own host glue) and minus `util_sndfile.c` (the bundled libsndfile won't build bare-metal; we stub `sf_*` instead). 36 `.cpp` + 3 `.c` TUs.

- **No flex/bison needed.** `-D__USE_CHUCK_YACC__` selects the checked-in pre-generated parser (`core/chuck_yacc.h`) over the bison-generated `chuck.tab.h`. `core/chuck_yacc.c` compiles standalone.

- **Feature defines = WebChucK's emscripten set** (`__DISABLE_MIDI__`, `__DISABLE_HID__`, `__DISABLE_SERIAL__`, `__DISABLE_OTF_SERVER__`, `__DISABLE_NETWORK__`, **`__DISABLE_THREADS__`**, `__DISABLE_SHELL__`, `__OLDSCHOOL_RANDOM__`, …) plus `-D__PLATFORM_LINUX__` (picks a platform branch; also dodges a literal `#elif`-with-no-expression bug in `util_platforms.h` that fires when no platform is defined) and the little-endian / `sf_count_t` macros for the vendored `util_sndfile.h`.

- **The port surface = a shim sysroot, not a code patch.** ChucK's core assumes a hosted POSIX userland with no bare-metal branch (`#ifndef __PLATFORM_WINDOWS__ #include <dirent.h>/<netinet/in.h>/ <unistd.h>`, `va_list` without `<cstdarg>`, `sys/ioctl.h`, `poll.h`, BSD `random`/`usleep`, `getline`, `realpath`, `glob`/`wordexp`). The script writes `Daisy/shim/` (minimal stub headers) + a force-included `ck_prelude.h` (declares the POSIX functions ChucK uses without including their headers) + `chuck_posix_stubs.c` (resolves the disabled-feature symbols at link: chugin `dlopen`, directory scan, tty, BSD random, and libsndfile `sf_*` — none are exercised; `SndBuf`/`LiSa` file loads just fail gracefully, which is fine with no filesystem). The stub TU is folded into `libchuck.a`, so the firmware link needs only the one archive.

- **MCU flags match `lib/libDaisy/core/Makefile`** (`-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard`) so the archive is ABI-compatible (and gets the hardware double FPU — see risk #1). Built at `-Os` with exceptions + RTTI **on** (ChucK uses both; `-fno-exceptions` was not validated).

- **Result:** `Daisy/lib/libchuck.a` (~5.9 MB archive, ~1.1 MB `.text`). The script's link probe (`new ChucK()` → `setParam` → `init` → `compileCode` → `run`) links against libc/libstdc++ + the folded stubs with `--specs=nano.specs --specs=nosys.specs`. >186 KB of code ⇒ confirms the **QSPI** build; ~1.1 MB fits the 8 MB flash easily.

Headers come straight from `thirdparty/chuck/src/core` (no copy step); the firmware `-I`s that dir plus `Daisy/shim`, and force-includes `Daisy/shim/ck_prelude.h`, with the same `-D` set above scoped to the ChucK engine TU (M2 Makefile wiring).

---

## Roadmap (ordered by leverage)

**M0 — Cross-build `libchuck.a`. — DONE (2026-06-19).** `scripts/fetch_chuck.sh` builds the bare-metal core-only archive from WebChucK's source subset + feature flags, generating the shim sysroot/prelude/stubs it needs, and **self-verifies** by compiling + linking a probe TU (`new ChucK()` → `compileCode("SinOsc s => dac;")` → `run()`) for cortex-m7. All 39 core TUs compile; the probe links (text ≈ 1.1 MB). The threading risk (#2) was retired by `-D__DISABLE_THREADS__`. See "Building `libchuck.a`" above for the full recipe and findings. *Gate cleared.*

**M1 — Pod tone from QSPI (retire risk #1 + #2). — DONE, WORKING (2026-06-19).** The Pod vehicle (`pod/harness_chuck.cpp` + `pod/Makefile.chuck`) builds an `EngineContext`, `init()`s the real `ChuckEngine`, drives `process()` from the audio callback, and reads the knobs → `set_param()`. The BOOT_QSPI image (≈1.32 MB) links against `libchuck.a` + the real SDRAM `--wrap` pool (`chuck_alloc.cpp`, so `chuck_heap_arm()` arms the `.sdram_bss` pool — the M2 heap model). **Flashed and confirmed on a Pod: the drone plays, knobs drive it.** Four bugs had to be fixed first (see [`chuck-pod-poc.md`](chuck-pod-poc.md)). **Still open:** `METER=1` CPU headroom — `ck->run(256)` is heavy (~near the block budget at block 256); quantify before adding voices.

**M2 — `IEngine` skeleton on the spotykach. — HARDWARE-VERIFIED (2026-06-21).** `ChuckEngine : IEngine` (`src/engine/chuck/chuck_engine.{h,cpp}`) implements `init`/`prepare`/`process`/ `set_param`→`setGlobalFloat`/`param`/`render`/`capabilities` + the built-in `kProgram` (a `SawOsc => LPF => dac` drone reading `speedA`/`mixA`/`sizeA`). `process()` interleaves the platform's de-interleaved buffers into `ck->run()` (SAMPLE=float) and back; the run() scratch is malloc'd from the SDRAM pool so the SRAM-tight platform pays nothing for it. Wired in: the `ENGINE=chuck` Makefile branch (BOOT_QSPI, the ChucK feature defines matching `libchuck.a`'s ABI, `-fexceptions -frtti` scoped to just the engine TU, `--wrap` malloc), `engine_select.h` (`SPK_ENGINE_CHUCK → ChuckEngine`), `chuck_alloc.cpp` (reuses the engine-agnostic `CsoundPool`), and the shared `spotykach_qspi_vtor.cpp`. `make engine-chuck` links the full firmware. **One platform fix this surfaced:** the QSPI build reserved 186 KB of AXI SRAM for `SRAM_EXEC` that holds no code (it executes in place from QSPIFLASH), so `CoreUI + libDaisy .bss` (~340 KB) overflowed the 326 KB `SRAM` region. Rather than touch the proven Csound QSPI path, ChucK gets its own `alt_qspi_chuck.lds` (identical to `alt_qspi.lds` except it reclaims the unused 186 KB `SRAM_EXEC` into `SRAM`, giving data the full 512 KB AXI SRAM); the `ENGINE=chuck` branch and the Pod harness point at it, and the stock `alt_qspi.lds` (csound) is byte-unchanged. The four Pod fixes are all in the spotykach build (`-u _printf_float` in the `ENGINE=chuck` branch; `start()` + the pool guard via the shared `chuck_engine.cpp`/`chuck_alloc.cpp`; the knob-cadence fix is n/a — the spotykach UI is event-driven). **Flashed the cased unit (2026-06-21): boots, runs the VM, makes sound.** Remaining hardware checks: read the CPU/shred meter for the actual headroom, and test the M3 bank with an SD card. MIDI is M4.

**M3 — SD `.ck` patch bank + live swap. — CODE DONE, BUILD-VERIFIED (2026-06-21).** `chuck_patch.h` (numbered `chuck/<n>.ck` slots, the Alt+PITCH quantizer, read+normalize+BOM/CRLF-strip, built-in fallback) + the bank/selector wiring in `chuck_engine.{h,cpp}` (rescan, boot auto-load of the lowest slot, Alt+PITCH preview/commit, the cyan/white source LED, the gated live recompile). The host selector test (`make -C host test-chuck-patch`) passes and both firmware targets link clean (`pod/Makefile.chuck`, `make engine-chuck`). **Hardware-verified, incl. the memory-leak fix (2026-06-22)** — see the "Patch-swap memory leak — RESOLVED" section above for the full story. Three deliberate deltas from the Csound original:

- **No `chuck_reload.h`.** The `ReloadGate` is engine-agnostic (`void*`, no Csound deps), so the ChucK engine reuses `engine/csound/csound_reload.h` directly - the same reuse pattern `chuck_alloc.cpp` already uses for `CsoundPool`. One gate, one host test (`test-csound-reload` covers it).

- **One persistent VM + compile-once cache (NOT rebuilt, NOT reset).** Csound's `do_reload` destroys + recreates the `CSOUND`; ChucK keeps **one** VM for the whole session and only changes its *shreds*, because ChucK never frees its type system on teardown — so both destroying the VM and `CK_MSG_CLEARVM`- resetting it leaked (see the RESOLVED writeup for the full root cause + the earlier failed attempts). A reload, behind the `ReloadGate`: `removeAllShreds()` + a 1-frame `run()` to flush the *deferred* removal (which detaches the old patch's UGens from `dac`) BEFORE sporking the new patch, then either re-spork the target's **cached** `Chuck_VM_Code` (`vm()->spork(code, NULL, TRUE)` — no recompile, no leak) or, on the patch's first visit, `compileCode()` it once and pin the emitted code in the cache. The gate is required because `compileCode`/`spork` mutate VM state and, under `__DISABLE_THREADS__`, nothing locks them against a concurrent `run()`. A brief audio gap on the swap (and a slightly longer one on a patch's first, compiling visit) is the cost.

- **No structural pre-validation.** A `.csd` must begin with `<CsoundSynthesizer>`; a `.ck` has no required header, so `read_program` only normalizes (BOM/CRLF) and rejects empty/whitespace-only files. The real validation is `compileCode()` returning false -> fall back to the built-in.

**M4 — MIDI in.** Enqueue NoteOn on the SPSC `NoteQueue`; `process()` drains it in the ISR and drives a `global Event noteOn; global int midiNote;` that the program's `MidiNote` shred sporks a finite voice on. NoteOn-only / no velocity / no NoteOff (platform constraint) → finite plucks, same as Csound.

**M5 — Control/UI depth + UGen audit.** Expand the global vocabulary to the full 7 params/deck, draw the Alt+PITCH selector, document the available UGen set, and write the user-facing `docs/engines/chuck.md` + `examples/chuck/` programs (a drone, a bass, a poly MIDI lead — the templates that name the platform globals).

**M6 — sample playback: `SndBuf` via a WAV-over-FatFs bridge (PLANNED).** Make `SndBuf` (and the read path of `WvIn`/`LiSa`) actually load a `.wav` from the SD card, so patches like `examples/chuck/5.ck` play samples instead of compiling-then-exiting on `!buf.ready()`. See the dedicated design section ["Planned (M6): sample playback"](#planned-m6-sample-playback--sndbuf-via-a-wav-over-fatfs-bridge) below for the full scope, the threading argument, and the implementation plan. *Streaming reads (`WvIn` live), recording (`WvOut`), and a general newlib→FatFs virtual FS are explicitly out of scope — see that section.*

**Host-testable off-target (no `libchuck`):** the patch bank scan/quantizer, the `ReloadGate` handoff, and the MIDI note→global mapping + SPSC ring — exactly the units Csound already host-tests (`make -C host test-csound-{patch,reload,midi,alloc}`). Mirror them as `test-chuck-*`. The `compileCode`/`run` calls are QSPI-only.

---

## Planned (M6): sample playback — `SndBuf` via a WAV-over-FatFs bridge

**Goal.** Let `SndBuf` load a `.wav` from the SD card and play it, so sample-based patches (`examples/chuck/5.ck` is the motivating case) work instead of silently exiting. Today the file UGens are present-but-dead: `ugen_stk` is compiled, so `SndBuf` type-checks, but sound-file I/O is stubbed — `chuck_posix_stubs.c` defines `sf_open()`→`NULL`, `sf_readf_float()`→`0`, so `buf.ready()` is false and the patch hits `me.exit()`. Two layers are missing: the **codec** — libsndfile (`util_sndfile.c`) is excluded from the bare-metal source subset (it won't build), and its `sf_*` symbols are stubbed to fail; and the **filesystem** — ChucK/libsndfile reach files through POSIX file syscalls, which under `--specs=nosys.specs` are not wired to the SD's FatFs. (Both were inherited from the WebChucK no-filesystem template the port is modelled on.) This bridge supplies a minimal codec that reads via FatFs directly, sidestepping both.

### The governing constraint: ChucK reads files on the audio thread

`SndBuf buf(filename)` performs its read when the **shred executes the constructor**, which is inside `run()` — i.e. the audio ISR. That collides head-on with the platform's SD contract (`istreamdeck.h`): **FatFs I/O happens on the main loop only; the audio ISR touches only lock-free rings and never blocks.** A blocking, ms-scale SD read from inside the 2 ms audio block would overrun, and would race the platform's own main-loop FatFs use (Storage, the `.ck` bank, the tape/radio decks — FatFs is not safe across the two contexts). So the design must guarantee the actual SD read never happens in the ISR.

**The lever (no new machinery): drive the load on the main loop during the swap.** ChucK time only advances when we call `run()`, and a shred runs all of its non-time-advancing code in a single compute tick (up to the first `... => now`). `do_reload` already calls `_ck->run(_inbuf,_outbuf,1)` **on the main loop, gate held**, to flush `removeAllShreds`. If — after `load_program()` sporks the new patch — we drive one more short `run()` there, the new shred executes its top-level code (including the `SndBuf` constructor, hence the FatFs read) **on the main loop**, where FatFs is legal and serialized against Storage. Playback thereafter reads from the in-RAM `SndBuf` entirely in the ISR, with zero file I/O. This makes whole-file `SndBuf` loads fit the platform contract with no async handshake.

### Architecture

1. **Real `sf_*` in a firmware TU (overrides the archive stubs, no `libchuck` rebuild).** The `sf_*` stubs live in `chuck_posix_stubs.c`, folded into `libchuck.a`. A new firmware-side TU `src/engine/chuck/chuck_sndfile.cpp` that *defines* the real `sf_open`/`sf_seek`/`sf_readf_float`/ `sf_close`/`sf_strerror`/… resolves those symbols from the firmware object, so the linker never pulls the archive stubs (standard archive resolution — strong firmware symbol wins; no weak symbols, no rebuild). Being firmware-side C++, it can call FatFs / the platform freely. Compiles only into the `ENGINE=chuck` target.

2. **Read via FatFs, not POSIX.** Implement the `sf_*` subset against FatFs (`f_open`/`f_read`/`f_lseek`/ `f_close`) directly — the same layer `chuck_patch.h`/`IStreamDeck` already use — so we never touch the un-wired newlib file syscalls. Paths are relative (no leading slash), same FatFs gotcha as the bank.

3. **Main-loop preload tick in `do_reload`.** After `load_program()`, drive a short `run()` on the main loop (gate held) so the new shred's `SndBuf` constructor executes there. Add a one-shot guard so a patch whose top-level code never blocks on time can't spin (cap the preload to a small frame budget).

### `sf_*` subset to implement (read side only)

ChucK's `SndBuf` uses the libsndfile read API. The minimum to satisfy it:

- `SNDFILE* sf_open(const char* path, int mode, SF_INFO* info)` — `mode == SFM_READ` only; `f_open` + parse the WAV header; fill `info->frames/samplerate/channels/format`; return an opaque handle (a small heap struct holding the `FIL`, data-chunk offset/length, format, channel count) or `NULL` on any error.

- `sf_count_t sf_seek(SNDFILE*, sf_count_t frames, int whence)` — `f_lseek` into the data chunk (`data_off + frames*frame_bytes`); SEEK_SET/CUR/END.

- `sf_count_t sf_readf_float(SNDFILE*, float*, sf_count_t frames)` — `f_read` raw frames, convert to interleaved float in [-1,1], return frames read. (`sf_readf_double` similar if the build needs it.)

- `int sf_close(SNDFILE*)` — `f_close` + free the handle.

- `const char* sf_strerror(SNDFILE*)`, and whatever else `SndBuf`/`util_sndfile` reference at link (audit the undefined-symbol set after dropping the stubs; e.g. `sf_open_fd`, `sf_command` may need benign stubs that still fail).

**WAV parsing (minimal, robust).** RIFF/`WAVE`; walk chunks for `fmt` and `data` (don't assume order or adjacency; skip unknown chunks via their size; handle the pad byte on odd sizes). Support `WAVE_FORMAT_PCM` 16-bit and 24-bit and `WAVE_FORMAT_IEEE_FLOAT` 32-bit, mono and stereo. Convert PCM→ float (`/32768`, sign-extend 24-bit). Reject (return `NULL`) anything else (compressed, >2ch, exotic bit-depths) — the patch then sees `!buf.ready()` and exits gracefully, same as today.

### Memory & limits

- `SndBuf` loads the **whole file into the SDRAM pool** (the 40 MB `--wrap` pool, shared with the VM and the compile-once cache). Sample length is therefore bounded by pool headroom — a multi-MB sample is fine; a multi-minute stereo file is not. Document the practical cap; consider a hard size guard in `sf_open` that fails large files rather than exhausting the pool mid-load.

- The bridge is read-only and one-shot-per-shred. It does **not** make ChucK's heap or file access general — only the `SndBuf`/`WvIn`/`LiSa` *read* path.

### Explicitly out of scope (and why)

- **Streaming reads (`WvIn` reading continuously during playback).** Its reads land on arbitrary later ISR ticks, not just at shred start, so the main-loop-preload trick doesn't cover it. The correct way to stream from SD is through `IStreamDeck`'s deck rings (what tape/radio use), which is a much larger restructuring — defer.

- **Recording (`WvOut`).** Write side + continuous ISR I/O; same reason. Defer.

- **A general newlib→FatFs virtual FS.** It would make any libc file call hit the SD, but (a) it doesn't decode audio (SndBuf still needs a codec) and (b) it *invites* ISR file I/O, the exact thing the platform forbids. Skip it; the targeted `sf_*` bridge is safer and sufficient.

### Validation

- **Host test (`host/test_chuck_sndfile.cpp`, `make -C host test-chuck-sndfile`).** The WAV parser + PCM→float conversion are pure and `libchuck`-free: feed crafted byte buffers (PCM16/24, float32, mono/stereo, chunks in odd order, a truncated/garbage header, an unsupported format) through the parse

  - decode and assert frame counts, sample values, and graceful rejection. Factor the parser so the FatFs `f_read` is behind a tiny "read N bytes" seam the host test fills from memory.

- **On target:** flash with a card holding `chuck/5.ck` + `samples/snare.wav`; confirm it plays, confirm the load happens without a dropout/overrun (it runs on the main loop), and confirm an unsupported/missing file falls back to silence cleanly (not a hang).

### Files

- `src/engine/chuck/chuck_sndfile.cpp` — **new (M6).** Real `sf_*` over FatFs (overrides the `libchuck.a` stubs); minimal WAV parse + PCM→float; the "read N bytes" seam for host testing.

- `src/engine/chuck/chuck_engine.cpp` — `do_reload` gains the main-loop preload tick after `load_program()`.

- `host/test_chuck_sndfile.cpp` + the `test-chuck-sndfile` target — the parser/decoder unit test.

---

## Anticipated gotchas (carried from Csound; verify for ChucK)

- **FatFs paths must be relative** (no leading slash) — `"chuck/%d.ck"`, not `"/chuck/…"`.

- **SD text vs a compiled-in string**: strip a UTF-8 BOM and normalize CRLF→LF before `compileCode` (the Csound parser choked on both; ChucK's lexer likely tolerates more, but normalize anyway).

- **The SDRAM-heap-vs-ctor crash**: keep the platform heap in SRAM; arm the ChucK SDRAM pool *after* `_hw.Init()`. This is the central reusable lesson.

- **`-DSPK_USE_STREAM`** must be in the Makefile chuck branch or `ctx.stream` is null and SD loading silently no-ops (the bank shows only the built-in).

- **Large block / shred budget**: keep the block at 256; watch dense per-sample shreds (risk #5).

---

## Files (mirrors the csound layout)

- `src/engine/chuck/chuck_engine.{h,cpp}` — **DONE (M2).** `ChuckEngine : IEngine` (init / compile / process / set_param→`setGlobalFloat` / render + the built-in `kProgram` + the global-name param map). Compiles only for `ENGINE=chuck`.

- `src/engine/chuck/chuck_alloc.cpp` — **DONE (M2).** The `--wrap` malloc shim over a 12 MB `.sdram_bss` pool. Reuses the engine-agnostic `CsoundPool` (`engine/csound/csound_pool.h`) rather than duplicating it (roadmap: promote that to a shared `SdramPool`; reused as-is to keep csound byte-identical).

- `pod/harness_chuck.cpp` + `pod/Makefile.chuck` — **DONE (M1).** The standalone Pod proof-of-life.

- VTOR inject — **shared:** the `ENGINE=chuck` branch links `src/engine/csound/spotykach_qspi_vtor.cpp` directly (the BOOT_QSPI vector-table fix is engine-agnostic); no chuck copy.

- `src/engine/chuck/chuck_patch.h` — **DONE (M3).** numbered `chuck/<n>.ck` bank: path / scan / Alt+PITCH quantizer / read+normalize with built-in fallback (header-only, host-tested via `test_chuck_patch.cpp`).

- `chuck_reload.h` — **not created (M3).** The `ReloadGate` is engine-agnostic; `chuck_engine.h` reuses `engine/csound/csound_reload.h` directly (like `chuck_alloc.cpp` reuses `CsoundPool`). No duplicate file or test. (Roadmap: promote both out of `csound/` into a shared location.)

- `src/engine/chuck/chuck_midi.h` — *M4.* note→global mapping + the lock-free SPSC note ring.

---

## See also

- [`docs/dev/csound-impl.md`](csound-impl.md) — the shipped sibling engine; the primary reference for every reused mechanism (heap, QSPI, reload gate, SD bank, MIDI ring, debugging).

- [`docs/engines/csound.md`](../engines/csound.md) — the user-facing model `docs/engines/chuck.md` should follow.

- [ccrma/chuck](https://github.com/ccrma/chuck) · [WebChucK](https://github.com/ccrma/webchuck) (the bare-metal-shaped build template) · [ChucK docs](https://chuck.stanford.edu).
