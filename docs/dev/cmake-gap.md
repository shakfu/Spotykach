# Dev notes — the CMake-vs-Makefile `SRAM_EXEC` size gap

The repo has two firmware build systems: the canonical, hardware-proven **Makefile** (output in `build/`) and an opt-in **CMake** path (`CMakeLists.txt` + the thin `Makefile.cmake` frontend, output in `build-cmake/<engine>/`). They are kept at parity. This note records why the CMake binaries were once ~5 % larger in `SRAM_EXEC`, how that was traced and mostly fixed, and the one residual difference that remains by choice.

## Decision (2026-08-08): keep both — Makefile canonical, CMake supported

TODO P5 framed this as binary: *finish the CMake adoption or back it out*, on the grounds that three
build files on `main` with a hand-duplicated engine list is a standing liability. That framing was
right at the time, because nothing detected the duplication going wrong. Seven divergences had
accumulated silently, two of them hard build failures and one — the missing `USB_MIDI` equivalent —
producing an image that built, booted, ran, and ignored MIDI.

**What changed is that the drift is now mechanically caught.** `.github/workflows/ci.yml` builds six
flag-sensitive engines through CMake alongside the full Makefile matrix, chosen for what they exercise
rather than for coverage: `pstretch` (own linker script), `reverb` and `graincloud` (`-Os`), `softcut`
(extra sources), `mosc` (QSPI boot), `granular` (the default). Every past divergence would have been
caught by one of those six. The liability was never *having* two build systems; it was having no
forcing function, and that is the part that was missing.

So: **both stay.** The Makefile is canonical — it is what `make`, the README, the release script and
every hardware-verified image use. CMake is *supported*, at parity, and CI keeps it there.

**This decision was conditional on CI running unasked, and that condition was met 2026-08-11.**
`ci.yml` now fires on push and pull request, so the CMake parity matrix catches divergence
mechanically rather than resting on someone remembering to press a button. (`qspi-libs.yml` is still
dispatch-plus-weekly by design; it builds no CMake target.) Caveat: neither workflow has yet completed
a run on a GitHub runner, so the first push is also the first proof — if it turns out misconfigured,
this decision is back to resting on a button until it is fixed.

**What is being given up.** TODO P5's headline justification for adopting CMake was item 4: per-target
`target_include_directories(... PRIVATE)` making a platform→engine include a *compile error* instead of
a grep hit. That is not built and is not planned here. Without it, CMake's remaining advantages over
the Makefile are real but modest:

- **Per-engine cached build dirs** (`build-cmake/<engine>/`), so switching engines never rebuilds. The
  Makefile shares one `build/`, which is exactly why it needs the `.engine-stamp` / `.grainflavor-stamp`
  / `.usbmidi-stamp` machinery — three stamps that exist purely to work around the shared directory.
- **Flag changes are tracked properly.** `SPK_TERMINAL`, `TERM_USBDIAG` and `SPK_GRAIN_GF` change *type
  layout*, so a partial rebuild puts members at the wrong offsets — a frozen panel with a working
  terminal, which cost a hardware session to diagnose. CMake records definitions in `flags.make`, which
  every object already depends on; the Makefile has to delete objects and defend against GNU Make
  3.81's whole-second mtime resolution.
- `compile_commands.json` falls out natively, with no `bear`.

**What would reopen this:** CI staying unarmed; a third build file appearing; or someone wanting the
compiler-enforced boundary badly enough to build item 4, at which point CMake becomes canonical and the
Makefile goes. Backing CMake out remains a clean revert — nothing depends on it.

**Still not established, either way:** no CMake-built `.bin` has been flashed. P5's acceptance gate 5
stands unchanged and belongs to the P2 bench session. CI proves the two agree on the host; only a
device proves the CMake image boots.

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

## Feature-parity re-sync (2026-08-03)

The size analysis above was done when the two builds were feature-equivalent. They then drifted: the Makefile gained switches and per-engine settings that were never mirrored, because nothing forces them to be — the CMake path is opt-in, so a Makefile-only change breaks nothing anyone notices until someone builds that engine the other way.

Seven divergences had accumulated. Two were hard build failures, not differences of degree:

| Gap | Effect before the fix |
|---|---|
| `src/terminal/*.cpp` missing from the source list | any `TERMINAL=1` build failed to link on the whole Terminal/dispatch surface |
| pstretch not on `linker/alt_sram_pstretch.lds` | `region SRAM overflowed by 80584 bytes` — pstretch did not build at all |
| reverb missing `OPT = -Os` | linked, but ~6 KB larger than the canonical build |
| csound missing `--wrap=aligned_alloc` | the wrap that keeps nano-libc's `aligned_alloc` from pulling `posix_memalign` (absent under nosys) |
| `USB_MIDI` default | never defined, so the QSPI engines silently lost device MIDI |
| `TERMINAL` / `USBDIAG` / `TERMPORT` / `METER` / `WINDOW` / `BRINGUP` / `NOCHUCK` / `CHUCKLVL` | not implemented; passing them did nothing |
| `SOFTCUT_EXTRA` | not implemented |

`USB_MIDI` needed a stand-in for the Makefile's `APP_TYPE`: the Makefile defaults it on for `BOOT_QSPI` and off otherwise, but `BOOT_SRAM`/`BOOT_QSPI` differ only by linker script here, so CMake carries an explicit `ENGINE_BOOT_QSPI` flag set by the three QSPI engines.

### The one place the frontend had to change shape

Toggles could not simply be forwarded when set. A CMake cache entry is **sticky**: configure once with `-DTERMINAL=1` and every later build in that directory stays a terminal build, even when the caller drops the flag. The old `Makefile.cmake` configured only when `CMakeCache.txt` was absent, which made this worse — a changed toggle was ignored outright.

So `Makefile.cmake` now configures on **every** invocation and passes **every** toggle with its current value, including the empty one. An empty value is falsy to CMake's `if()`, so absent means off, exactly as in make. A warm reconfigure costs ~0.1 s.

This is also the one place CMake is structurally *better* than the canonical build. `SPK_TERMINAL`, `TERM_USBDIAG` and `SPK_TERMINAL_PORT_EXTERNAL` change **type layout** (virtuals on `IEngine`, members on `CoreUI`/`AppImpl`), so a partial rebuild produces a binary whose members sit at the wrong offsets — a frozen panel with a working terminal, which took a hardware session to diagnose. The Makefile defends against it with three stamp files and an object wipe, because GNU Make 3.81 compares mtimes at whole-second resolution. CMake records the definitions in `flags.make`, which every object already depends on, so a toggle change rebuilds everything on its own.

### Verified

All 22 engines build under both systems.

Sizes must be compared on **clean trees**. A Makefile build of engine B on top of engine A's `build/` is not a valid baseline: the stamps force only `app.o`, `version.o` and the three stream TUs to rebuild, so TUs like `card.o` and `storage.o` keep the previous engine's `SPK_USE_STREAM` setting. That contamination read as a 6 KB difference until the tree was cleaned. `make dist` / `make dist-cmake` avoid the trap by construction — the canonical path cleans per engine, and the CMake path gives each engine its own directory.

Full published set, `.bin` sizes at `0.6.1-14-g3327c33`, produced by those two targets:

| engine | make | cmake | delta | | engine | make | cmake | delta |
|---|---:|---:|---:|---|---|---:|---:|---:|
| bard | 167184 | 168448 | +1264 | | pstretch | 164448 | 165704 | +1256 |
| chuck | 1415352 | 1416900 | +1548 | | qdelay | 155376 | 156632 | +1256 |
| csound | 2345828 | 2346992 | +1164 | | radio | 159816 | 161072 | +1256 |
| delay | 154440 | 155696 | +1256 | | reso | 184140 | 185404 | +1264 |
| edrums | 163388 | 164644 | +1256 | | reverb | 185796 | 187060 | +1264 |
| filter | 158424 | 159680 | +1256 | | shuttle | 173864 | 175120 | +1256 |
| glitch | 158056 | 159312 | +1256 | | softcut | 179340 | 180784 | +1444 |
| graincloud | 184492 | 185856 | +1364 | | tape | 173376 | 174632 | +1256 |
| mosc | 304820 | 306188 | +1368 | | voice | 160256 | 161504 | +1248 |

CMake is larger on **18 of 18**, range 1164–1548 B, mean 1290 B, mode **1256 B** (nine engines). That mode is Cause 3 measured exactly: `.text` +272, `.data` +972, `.init_array` +4 = 1248 B, plus alignment. Thirteen of eighteen sit in a 1248–1264 B band. `.bss` matches exactly everywhere.

The point of the table is the **absence of an outlier**. No engine diverges by a different order of magnitude, which is the failure a whole-set comparison exists to rule out.

Five engines sit outside the band: `csound` +1164 (*below* it), `graincloud` +1364, `mosc` +1368, `softcut` +1444, `chuck` +1548. The four highest are the largest or most libc-hungry engines, and `csound` coming in low is consistent with `libcsound.a` already pulling `atexit`/`__register_exitproc` in **both** builds, leaving only the `impure_data` delta. That reading is **inference from the shape of the data** — the symbol-level breakdown was verified on `chorus` only. Pinning it down is one `nm` diff per engine if it ever matters.

In proportion the delta is negligible: 0.05% on `csound`, ~0.8% on the small SRAM engines. Nor is any budget threatened. Since the `SRAM_EXEC` rebalance to 300K the loaded image sits near 60% on every SRAM engine, so 1.25 KB is ~0.4 percentage points:

```
reso        SRAM_EXEC 60.35%   SRAM 23.97%
graincloud            60.49%        74.91%
reverb                60.89%        27.79%
bard                  54.83%        62.60%
```

The one engine where it could have mattered — `pstretch`, at 93.17% of its data region — takes the delta in `SRAM_EXEC` (80.91% vs 80.30%), not in the region that is nearly full.

**Regenerating this table:** `make dist VERSION=<v>` then `make dist-cmake VERSION=<v>`, and diff the two `MANIFEST.txt` files. The comparison is only valid when the firmware source is identical and the two version strings are the **same length** — the banner is baked into the image, so a shorter or longer version string shifts every size by that difference.

### Building a full release through CMake

`make dist-cmake` runs the same `scripts/build_release.py` as `make dist`, with `--cmake`. The manifest, `SHA256SUMS`, release notes and the in-binary banner check are shared code; only the compiler driver differs. This exists to compare the two build systems across the whole engine set, not to produce artifacts for users.

Three things had to change to make it possible:

- **`SPK_VERSION` was not overridable in CMake.** `execute_process(git describe)` overwrote whatever was passed, so a pinned release version could never reach the binary — and `build_release.py` *verifies* the banner it expects is present, so every artifact would have failed that check. It is now guarded, matching the Makefile's `?=`.
- **No `clean` step, and no `ENGINE_MAKE_FLAGS`.** The canonical build cleans between engines because `build/` is shared and its stamps only force a subset of TUs to rebuild; CMake's per-engine `build-cmake/<engine>` has no cross-engine contamination to wipe, and cleaning would rebuild libDaisy once per engine. `APP_TYPE`/`LDSCRIPT` are Makefile concepts — `CMakeLists.txt` derives the linker script from `ENGINE` alone.
- **Output goes to `dist-cmake/<version>/`, never `dist/`.** `make gh-release` globs `dist/<version>/*` and uploads it, so a shared output directory would let someone publish CMake-built binaries as an official release by running two targets in the wrong order. `dist-cmake/` needed its own `.gitignore` entry: `dist/` matches that name exactly and `build-*/` does not apply.

Measured on `delay` and `chorus` at the same pinned version, the CMake artifacts are **+1256 B** and **+1248 B** — the newlib residual above, consistent per engine and independent of engine size.

There is deliberately **one** entry point. Adding a `dist` target to `Makefile.cmake` as well would give two ways to invoke the same script, which is the duplication that produced every drift documented on this page.

### The stale engine list, and why it drifts

Found on the way: the Makefile's own `$(error Unknown ENGINE ...)` message named only 18 of the 22 engines — `glitch`, `pstretch`, `softcut` and `gigaverb` had never been added. Now fixed, and both messages name the same 22.

The list drifts because it is duplicated and hand-maintained, and the obvious repair — a shared `ENGINES` variable — does not actually fix it: `scripts/gen_engine.py` and `scripts/gen_faust_engine.py` locate the engine switch by the literal string `"else\n$(error Unknown ENGINE"` and insert generated blocks immediately before it. They add a branch without touching any list, so a generated engine would still go unnamed. Closing the gap properly means teaching both generators to update the list too; until then it stays a manual step, flagged by a comment beside the switch.

That sentinel also constrains edits here: nothing may be inserted between the `else` and the `$(error ...)` line, or both generators break.

## Files

- `CMakeLists.txt` — the two size fixes (`DSY_DISABLE_USB_HOST` on the `daisy` target; `${APP_OPT}` on the `FatFs` target) in the fork-gap block after `include(DaisyProject)`, plus the toggle block and the per-engine settings covered by the re-sync above.

- `Makefile.cmake` — the unconditional configure and the full toggle forwarding that give the switches make's absent-means-off semantics.

- `lib/libDaisy/Makefile` (line ~307) — where the fork sets `-DDSY_DISABLE_USB_HOST` for the Make build; the CMake fix mirrors it.

- `lib/libDaisy/src/sys/fatfs.cpp` — the `#ifndef DSY_DISABLE_USB_HOST` guard around `USBH_Driver`.
