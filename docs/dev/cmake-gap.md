# Dev notes — the CMake-vs-Makefile `SRAM_EXEC` size gap

The repo has two firmware build systems: the canonical, hardware-proven **Makefile** (output in `build/`) and an opt-in **CMake** path (`CMakeLists.txt` + the thin `Makefile.cmake` frontend, output in `build-cmake/<engine>/`). They are kept at parity. This note records why the CMake binaries were once ~5 % larger in `SRAM_EXEC`, how that was traced and mostly fixed, and the one residual difference that remains by choice.

## Symptom

After the CMake build was brought to full engine parity, every CMake binary linked ~5 % larger in `SRAM_EXEC` than the Makefile's. For `glitch`: Make **78.74 %** vs CMake **84.35 %** (~14 KB). The app TUs compile at the same opt level (`-O2`/`-Os` via `APP_OPT`), libDaisy and DaisySP are `-O3` in both, and neither uses LTO, so the opt levels were not the cause.

## Method

Binary diffing, not guessing:

1. `arm-none-eabi-size -A` on both ELFs to localize the delta to sections (`.text` / `.data`).

2. `arm-none-eabi-nm --print-size --size-sort` on both, joined by symbol name, to find symbols **only in** one build or **larger in** one build.

3. The GNU `ld` map's "Archive member included to satisfy reference by file (symbol)" section to trace **why** an object was pulled into the link.

This isolated three independent causes.

## Cause 1 (fixed): USB-host fork gap — ~8 KB

The symbol diff showed the CMake build linked the entire USB-host MSC stack plus the USB-device CDC stack and the Logger (`hhcd_USB_OTG_HS`, `hpcd_USB_OTG_FS`, `USBH_MSC_*`, and a tell-tale `_GLOBAL__sub_I_hUsbHostHS` static initializer) — none of which the Make build links, and none of which the platform uses.

The map trace: `sys/fatfs.cpp` references **both** `SD_Driver` and `USBH_Driver`; `USBH_Driver` pulls `usbh_diskio.c` → `hUsbHostHS` → `usb_host.cpp`, whose global USB-host-handle static initializer is a GC root (`.init_array`) that `--gc-sections` cannot drop, and which in turn drags in the Logger and the USB-device CDC middleware.

`fatfs.cpp` guards the USB driver with `#ifndef DSY_DISABLE_USB_HOST`. The bleeptools fork defines `-DDSY_DISABLE_USB_HOST` in libDaisy's **Makefile** (so the Make-built `libdaisy.a` omits the `USBH_Driver` reference) but never in libDaisy's **CMake** build — the same class of fork gap already documented for `midi_util.cpp`.

**Fix** (in `CMakeLists.txt`, alongside the other fork-gap patches):

```cmake
target_compile_definitions(daisy PRIVATE DSY_DISABLE_USB_HOST)
```

This removes only USB-**MSC-host** support (a USB stick), not SD-card FatFs, so the streaming engines (`tape`/`radio`/`shuttle`/`softcut`) are unaffected — verified by rebuilding `radio` (heavy FatFs+SD user), which still links and runs.

## Cause 2 (fixed): FatFs opt level — ~2.4 KB

After Cause 1, the residual `.text` symbols larger in CMake were all FatFs (`f_write`, `f_mkdir`, `dir_register`, `create_chain`, …). The Make build compiles `ff.c`/`diskio.c` as ordinary **app** sources at the app opt (`-O2`, or `-Os` for `reso`/`mosc`/`graincloud`); CMake builds FatFs as a sub-library that inherits the directory `-O3`, so its functions came out larger. FatFs is the only libDaisy component the Make build compiles at app opt; everything else in `libdaisy.a` is `-O3` in both.

**Fix** (in `CMakeLists.txt`):

```cmake
target_compile_options(FatFs PRIVATE ${APP_OPT})
```

The `FatFs` target is global (defined in `lib/libDaisy/Middlewares/Third_Party/FatFs/CMakeLists.txt`), so it can be overridden from the top-level lists; the target-level option wins over the directory `-O3`. It correctly tracks `-Os` for the size-tight engines (verified on `reso`).

## Result of Causes 1 + 2

| engine | Make | CMake before | CMake after |
|---|---|---|---|
| glitch | 78.74 % | 84.35 % | 79.38 % |
| radio (FatFs+SD) | 79.38 % | — | 80.04 % |
| reso (`-Os`) | 92.45 % | — | 93.12 % |

The gap dropped from ~5.6 % to a consistent **~0.65 %** across SRAM engines.

## Cause 3 (residual, ~0.65 %): full-`libc` vs nano `impure_data`

The last ~1 KB is newlib's `_impure_data` — the full-size (~968 B) C-library reentrancy struct — present in `.data` only in the CMake build. (`.data`'s load image sits in `SRAM_EXEC`, so it counts.)

### Root cause

Both builds pass `--specs=nano.specs --specs=nosys.specs`, both link `libstdc++_nano.a`, and the real `ld` line (via `g++ … -v`) shows `-lc_nano` and no `-lc`/`-lg`/`-g`. Yet the CMake map resolves ~146 newlib symbols — including the `exit` / `atexit` / `_global_impure_ptr` / `impure_data` family — from **full `libc.a`** rather than `libc_nano.a` (the Make build resolves all but 2 from `libc_nano.a`).

The trigger is `crt0.o`, which references `exit`. Both builds link the same `crt0.o`, and `crt0.o` is processed **before** any `-l` library, so its `exit` (and the reentrancy pointer it needs) bind to whichever `libc` the linker reaches first:

- **Make** lists the standard libraries explicitly — `LIBS += -ldaisy -lc -lm -lnosys` (libDaisy `core/Makefile`) — and with `nano.specs` rewriting the *implicit* `-lc` to `-lc_nano`, the ordering makes the nano library the primary resolver. Full `exit` is never pulled; `exit` resolves to nano's 40-byte stub and the small reent.

- **CMake** has `CMAKE_CXX_IMPLICIT_LINK_LIBRARIES` set empty by the toolchain file, so the standard libraries come only from the spec-injected group. `crt0.o`'s pending `exit` falls through to the linker's default-search **full `libc.a`**, which pulls full `exit` → `__call_exitprocs` → `__call_atexit` → the 968 B `impure_data`.

This is purely a link-time symbol-resolution-order difference; the compiled objects are identical.

### What was tried (and why each failed)

- **Append `-lc_nano`** after the groups — no effect. Once `ld` has bound `_global_impure_ptr` to `libc.a`, a later library cannot rebind it.

- **Insert `-lc_nano -lm -lnosys` right after `libdaisy.a`** (mirroring Make's order) — no effect. `crt0.o` is upstream of *every* `-l` flag, so its references are resolved before any added library is reached.

- **`-nostartfiles`** to drop the redundant `crt0.o` — breaks the link (`__dso_handle`, provided by `crtbegin.o`, goes undefined; the firmware needs the crt files, and the Make build keeps them too).

- Ruled out as suspects: implicit C++ libs (empty in both), the `libstdc++` variant (nano in both), and `-lg`/debug-libc (no `-g` on either link line).

A robust build-system fix would require replicating the Makefile's exact newlib library ordering inside CMake (or excluding full `libc.a` from the default search) — fragile, with real risk of breaking the link or boot, for 0.5 % of a struct that is never used on bare metal.

### Decision

Left as-is. The residual is **harmless**: `_impure_data` is the C-library reentrancy/`errno`/stdio state; the heavy parts (`exit`/`atexit`/`__call_exitprocs`) are dead because `main()` never returns on bare metal, so the firmware never executes any of it. The cost is ~968 B of `SRAM_EXEC` on the opt-in CMake build only; the canonical Makefile build is unaffected.

## Alternative fix: a firmware `exit` stub

The one change that **would** zero out the residual is to give the firmware its own `exit` (and, if needed, `atexit`/`__cxa_atexit`) so that `crt0.o`'s `exit` reference binds to the firmware symbol instead of full `libc`'s, breaking the `exit → __call_exitprocs → impure_data` chain. After that, `_impure_data` is pulled only by the remaining `errno`/stdio references, which resolve to nano's small reent (exactly what the Make build already gets).

Sketch (a freestanding-firmware idiom — the app never exits):

```c
// e.g. src/hw/ or a small newlib-stubs TU, compiled into every build.
extern "C" __attribute__((noreturn)) void exit(int) { while (1) {} }
// optionally, to also short-circuit the C++ static-dtor registration:
extern "C" int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }
```

### Why it is not done

- It is a **firmware source change to compensate for a build-system difference**. The firmware is correct as written; the discrepancy lives entirely in how CMake orders the newlib libraries at link time. Patching the program to hide a linker-ordering quirk is the wrong layer.

- It affects **both** build systems. The Make build already avoids the full `impure_data`, so the stub buys it nothing; it only adds firmware that exists to satisfy the opt-in build.

- The payoff is ~968 B of `SRAM_EXEC`, and only on the CMake build — below the bar for changing shipping firmware behaviour (overriding `exit`/`__cxa_atexit` is a real semantic change, even if dead on this target).

If the CMake build is ever promoted to canonical (the deferred option noted in `Makefile.cmake`), revisit this: at that point a firmware `exit` stub — or, better, aligning the CMake link's library list/order with the Makefile so nano resolves first — becomes worth doing, and the stub above is the quick lever.

## Files

- `CMakeLists.txt` — the two applied fixes (`DSY_DISABLE_USB_HOST` on the `daisy` target; `${APP_OPT}` on the `FatFs` target), in the fork-gap / size-parity block after `include(DaisyProject)`.

- `lib/libDaisy/Makefile` (line ~307) — where the fork sets `-DDSY_DISABLE_USB_HOST` for the Make build; the CMake fix mirrors it.

- `lib/libDaisy/src/sys/fatfs.cpp` — the `#ifndef DSY_DISABLE_USB_HOST` guard around `USBH_Driver`.
