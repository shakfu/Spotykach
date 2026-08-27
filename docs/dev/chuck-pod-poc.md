# ChucK on the Daisy Pod — RESOLVED (working synth + knobs)

> **Status (2026-06-19): WORKING.** The `ENGINE=chuck` runtime now boots, compiles, and runs on a bare > Daisy Pod from QSPI: it creates the VM, compiles the built-in `.ck` program, runs the synthesis VM in > the audio callback, produces audio (the `SawOsc -> LPF -> dac` drone), and applies live knob input via > the global-variable queue. Confirmed over SWD (running in `Chuck_UGen::system_tick`, > `Chuck_VM_Shreduler::advance`, `Chuck_Globals_Manager::handle_global_queue_messages`) and by ear. > > This supersedes the earlier "debugging the boot crash" handoff. **Every premise of that doc was wrong:** > the platform, bootloader, linker script, and SDRAM were fine the whole time; the "solid-white panel" / > "no LED" was never a crash signature — it was an *instrumentation gap*. The real faults were four > independent, ordinary bugs, found by attaching a debugger. Background: [`chuck-impl.md`](chuck-impl.md) > (roadmap), [`csound-impl.md`](csound-impl.md) (the sibling QSPI engine).

## TL;DR — the four root causes (in the order we hit them)

| # | Symptom | Root cause | Fix |
|---|---------|-----------|-----|
| 1 | `abort()` during `compileCode()` (dark LED, **no** CPU fault) | `--specs=nano.specs` ships libstdc++ **without exceptions**: every `std::__throw_*` is a bare `bl abort`. ChucK stringifies float literals via `std::to_string` -> `vsnprintf("%f")`, which with no float-printf returns a negative length -> `std::string(buf, buf-1)` -> `__throw_length_error` -> `abort`. | Link `-u _printf_float`. |
| 2 | HardFault right after init; type confusion (a `std::string` used as a ring buffer) | The VM was **never `start()`ed**. `ChucK::globals()` returns `NULL` until `vm->running()`, and `run()` lazily auto-`start()`s — *inside the audio ISR*, racing the main loop's `setGlobalFloat` through the non-reentrant pool. | Call `_ck->start()` in `init()` (single-threaded, before audio); null-guard `globals()`. |
| 3 | Imprecise BusFault in `CsoundPool::release` (free-list following a `0xaa..` pointer) | The SDRAM pool is shared by the main loop (`setGlobalFloat` -> `new`) and the audio ISR (`ck->run()` -> VM allocs). `CsoundPool` is **not reentrant**; concurrent alloc/free corrupts the free list. | Wrap every pool op in a PRIMASK critical section (`chuck_alloc.cpp`). |
| 4 | Knobs dead; later, audio noise once they worked | `set_param` was called in a **tight, unthrottled main loop** -> floods ChucK's 16384-slot global queue -> the VM chokes draining it in `run()` -> audio ISR eats ~100% CPU, starving the main loop (so `_cache` froze). Reading every block then exposed **ADC jitter** -> constant param reassignment -> zipper noise. | Read knobs **once per audio block in the callback**; **deadband + one-pole smooth** the values. |

The headline: **ChucK runs fine on bare-metal Daisy.** None of the failures were ChucK-on-Cortex-M incompatibilities — they were a libc config flag, an init-ordering mistake, a missing lock, and a host-integration cadence bug.

## How we found them (methodology — reusable)

The breakthrough was getting a debugger on the part. The cased spotykach hides the Daisy LED and SWD pads; the **bare Pod** exposes both, and an **ST-Link V3 mini** on SWD turned "guess from a white panel" into "read the program counter."

Two pieces of tooling, both kept in the tree:

- **`pod/daisy_qspi.cfg`** — OpenOCD config to flash *and* debug the QSPI app over SWD. The stock `stm32h7x.cfg` only knows internal flash (`0x08000000`); a `BOOT_QSPI` app lives in QSPI XIP (`0x90040000`). The config adds a `stmqspi` flash bank for the Daisy's IS25LP064A and **attaches without resetting** so the bootloader-configured QUADSPI controller is live (a `reset halt` would drop QUADSPI to its unconfigured power-on state). It also neutralizes `stm32h7x.cfg`'s DBGMCU reset/examine events, which fail on this hla+QSPI setup. Targets: `make -f Makefile.chuck program-swd` (flash over SWD, no DFU buttons) and `make -f Makefile.chuck openocd-attach` (inspect-only).

- **In-firmware crash capture** (`chuck_engine.cpp`) — because the nano libstdc++ turns every C++ throw into a bare `abort()` that spins in `_exit` (no fault, no message), we added: a `try/catch` recording the exception type + `what()`; **overrides of `abort()` and `__assert_func`** that record the cause (assert `file:line:expr`, or a stack-scan mini-backtrace of QSPI return addresses) instead of spinning silently; and two fixed buffers readable over SWD:

  - `g_chuck_init_stage` (`@0x24000168` in this build): `1`=new ChucK, `2`=init, `3`=compileCode, `9`=ok.

  - `g_chuck_init_error[192]` (`@0x240000a8`): `"<type>: <what>"`, `"assert ..."`, or `"abort bt: <addrs>"`.

Typical debug loop: flash (DFU or SWD), boot, then `openocd-attach` + `mdw`/`mdb` the buffers, or read fault registers (`CFSR @0xE000ED28`, `HFSR @0xE000ED2C`, `BFAR @0xE000ED38`) and the stacked exception frame at MSP to get the faulting PC, then `arm-none-eabi-addr2line` it. The single most useful trick was sampling PC repeatedly (resume/halt) to see where the CPU actually spends time — that exposed both the `_exit` spin (bug 1) and the audio-ISR starvation (bug 4).

> SWD note: the ST-Link/hla connect to the H7 at full clock is **flaky** (intermittent "init mode failed > / unable to connect"). Retry, and power-cycle the probe + target if it wedges. QSPI XIP rules still > apply: attach (don't `reset halt`), and the half-written-QSPI risk means a crashed `program-swd` can > need a DFU re-flash to recover.

## The four fixes in detail

### 1. Float printf (`-u _printf_float`)

`pod/Makefile.chuck` appends `LDFLAGS += -u _printf_float` after the libDaisy include. Without it, `std::to_string(double)` (reached from ChucK's parser stringifying the float literals in the `.ck` program) computes a negative length and aborts in `__throw_length_error`. This is a generic `nano.specs` gotcha, not ChucK-specific — any `%f`/`std::to_string(float)` in the firmware needs it.

### 2. Start the VM (`_ck->start()`)

`ChuckEngine::init()` now calls `_ck->start()` right after `_ck->init()` and before `compileCode()` — single-threaded, before `StartAudio`. This matches the proven embedding in `chuck-max` (`chuck_tilde.cpp`: `init()` -> `start()` before audio). `set_param` also null-guards `globals()` (it returns `NULL` pre-start). Without this, the VM only started lazily from inside the audio ISR, concurrently with the main loop — the source of the bug-3 race.

### 3. Reentrant pool (`CritSec`)

`chuck_alloc.cpp` wraps every `g_alloc` op (`alloc`/`release`/`grow`/`payload`) in a short PRIMASK-masked critical section (RAII save/restore, so it nests). `CsoundPool` (the engine-agnostic SDRAM coalescing allocator under `src/engine/csound/`, reused by ChucK via `--wrap`) has no internal locking, so this serializes the main loop and the audio ISR. Cost: interrupts are masked for the duration of a pool op; acceptable given how infrequent allocations are after startup.

### 4. Knob cadence + deadband (`harness_chuck.cpp`)

Knob reads + `set_param` moved from the main loop **into the audio callback** (one read per block, ~187 Hz) — the correct bare-metal pattern with no host thread to schedule global updates. The main loop now only does off-ISR housekeeping (`prepare()`). Each knob is **deadbanded** (only re-sent when it moves > 0.004 from the last sent value) so ADC jitter on a still pot sends nothing (clean audio at rest), and **one-pole smoothed** so turning glides instead of stepping.

## Build & flash

```text
scripts/fetch_chuck.sh                       # once: fetch + cross-build libchuck.a (gitignored)
cd pod && make -f Makefile.chuck             # build the harness (full ChucK by default)

# Flash over USB DFU (proven):
while ! make -f Makefile.chuck program-dfu; do sleep 0.3; done   # then tap RESET to catch the window
#   then tap RESET once more to boot

# OR flash over SWD (ST-Link wired, board booted once so QUADSPI is configured):
make -f Makefile.chuck program-swd

make -f Makefile.chuck openocd-attach        # attach-only, for inspection
```

Bring-up knobs still available: `NOCHUCK=1` (skip the whole ChucK runtime) and `CHUCKLVL=1|2|3`
(new ChucK / +init / +compile) — see the `CHUCK_RUNTIME_LEVEL` gates. `g_chuck_init_stage`/`_error` double as the bisection readout.

## Files changed this effort

- `src/engine/chuck/chuck_engine.cpp` — `_ck->start()` in init; null-guarded `globals()`; `try/catch`

  - `abort`/`__assert_func` overrides + `g_chuck_init_stage`/`g_chuck_init_error` capture.

- `src/engine/chuck/chuck_alloc.cpp` — `CritSec` (PRIMASK) around every pool op.

- `pod/harness_chuck.cpp` — knob handling in the audio callback, deadband + one-pole smoothing; main loop reduced to housekeeping.

- `pod/Makefile.chuck` — `LDFLAGS += -u _printf_float`; `program-swd` / `openocd-attach` targets.

- `pod/daisy_qspi.cfg` — **new**: SWD flash (`stmqspi` IS25LP064A) + attach config.

## Open items / next steps

- **CPU headroom.** `ck->run(256)` dominates the audio budget (~all of it at block 256). The drone fits real-time, but there is little margin. Profile before adding voices; consider a larger block, a coarser `.ck` control rate, or trimming the program. **Now measurable on the spotykach panel, no probe:** build `make engine-chuck METER=1` and the ChucK engine's `render()` shows ring A = whole-callback CPU load (arc=avg, dot=peak, colour=severity; red >= 85% = at/over budget) and ring B = live concurrent-shred count (one cyan dot per shred, from `vm()->shreduler()->get_all_shred_ids`, sampled in `process()` after `run()` so it can't race the shreduler). Same flag also streams max/avg/min load over USB CDC (app.cpp's `CpuLoadMeter`). The production build's rings are a stereo output meter (per-channel RMS arc

  - peak dot, dB-scaled). This is the answer to "how many concurrent shreds fit" - read CPU vs shred count together as you add voices.

- **Decide what debug instrumentation to keep.** The `abort`/`__assert_func` capture + `g_chuck_init_*` buffers are cheap and genuinely useful for future bring-up — recommend keeping. The stack-scan backtrace in `abort` and any leftover `blink()` checkpoints are optional.

- **Promote `CsoundPool` -> a shared `SdramPool`** (rename out of `csound/`), now that two engines depend on it and it needs the reentrancy guard. Keep both engines on the one instance or give each its own.

- **Spotykach build: HARDWARE-VERIFIED (2026-06-21)** — first DFU flash of the cased unit booted, ran the VM, and made sound; the four Pod fixes carried over via the shared TUs with no new bugs. `-u _printf_float` added to the `Makefile` `ENGINE=chuck` branch; `start()` + pool `CritSec` come free from the shared `chuck_engine.cpp`/`chuck_alloc.cpp` TUs. Fix #4 (param cadence) is **n/a** — the spotykach UI is event-driven (`core.ui.cpp`: `PotMoved` -> `_apply` -> `set_param`), so it does not flood ChucK's global queue the way the bare Pod harness's per-block loop did. `make engine-chuck` links clean (SDRAM 93.8%: the 48 MB granular arena + 12 MB ChucK pool coexist; the pool is reserved separately, not starved). Flash + verify on hardware. (The earlier `CHUCKLVL=2` crash was almost certainly these same four bugs, not memory.)

- **M3 — DONE (code/build, 2026-06-21):** SD `chuck/<n>.ck` patch bank + Alt+PITCH live recompile (`chuck_patch.h` + the gated single-VM `CK_MSG_CLEARVM` reset + `compileCode` swap). Host selector test passes; both firmware targets link clean; hardware test pending. **M4** (MIDI-in) is next — unchanged from `chuck-impl.md`.

## Toolchain / facts

- `/Library/DaisyToolchain/0.2.0` arm-none-eabi-gcc **10.3.1** (this machine; macOS). `libchuck.a` built by `scripts/fetch_chuck.sh` (CHUCK_REF `chuck-1.5.5.8`, exceptions+RTTI ON; `chuck_engine.o` matches with `-fexceptions -frtti`). `thirdparty/chuck` is gitignored — reproduce with the fetch script.

- ChucK feature defines in the engine TU **must** match `fetch_chuck.sh` (ABI). Verified matching this effort — the type confusion (bug 2) was init-ordering, not an ABI mismatch.

- STM32H750: 512 K AXI SRAM @`0x24000000`, 128 K DTCMRAM @`0x20000000`, 64 M SDRAM @`0xc0000000`, QSPI app @`0x90040000`. QUADSPI peripheral @`0x52005000`; QSPI chip IS25LP064A (8 MB). Daisy Pod pots: knob1 = `D21`, knob2 = `D15` (matches libDaisy `DaisyPod`). User LED = PC7. Audio user LED ~0.7 Hz blink = ISR alive.

- `alt_qspi_chuck.lds` (ChucK-only linker script) reclaims the unused 186 K `SRAM_EXEC` into `SRAM` and puts the stack at the top of AXI SRAM (`_estack=0x24080000`). Unchanged this effort and not implicated.

- DFU (Linux): `…0483/df11` needs a `plugdev` udev rule. On macOS DFU works directly (no udev). The bootloader is a Daisy QSPI bootloader (2 s breathing window); apps load at `0x90040000`.
