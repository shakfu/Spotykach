# Dev notes — dual resonator / pluck (`reso` engine)

Implementation/bring-up notes for `ENGINE=reso`. The user-facing reference (concept, modes, control map, knob defaults, build commands) is [`docs/engines/reso.md`](../engines/reso.md); this file holds the internals, the build system, the file map, and the bug writeups.

## Status at a glance

- Implemented and integrated end to end (engine, build system, host test).

- Host test suite passes: `make -C host test` -> `OK: all reso checks passed`.

- Firmware links and fits: `make ENGINE=reso` -> SRAM_EXEC **69.1%** (184,060 B / 260 KB), no overflow. (Re-measured 2026-08-31; this read "~96% (183 KB / 186 KB)" while the region was still 186 KB.)

- ARM compile of all Rings/stmlib sources is clean.

- PITCH tracks on hardware (the original "knob does nothing" bug is fixed - see the end).

- One open tuning item: the pluck attack is bright/percussive ("thomp"); the pitched fundamental is clear but the broadband attack could be softened. Not a defect, just voicing.

---

## Architecture (PIMPL)

`ResoEngine` (`src/engine/reso/reso_engine.h`) exposes only the `IEngine` interface plus an opaque `struct Impl* _p`. All Rings/stmlib types live in `reso_engine.cpp` inside `struct ResoEngine::Impl`.

This is mandatory, not stylistic: `stmlib/stmlib.h` declares a global `namespace impl` (for its `STATIC_ASSERT` helper). The composition root `src/app.cpp` has a `static AppImpl impl;` instance, and `app.cpp` includes the engine header via `engine_select.h`. If the header pulls Rings/stmlib, the `namespace impl` collides with `app.cpp`'s `impl` and `app.cpp` fails to compile ("expected primary-expression before '.' token"). Keeping Rings out of the header avoids it.

`Impl` (and the two ~108 KB Parts + two 64 KB reverb buffers it owns) is placement-new'd into the injected SDRAM arena at `init()`. Rings runs at 48 kHz with `kMaxBlockSize = 24`, so the 96-sample platform block is processed in four chunks of <=24.

---

## Build system

Dependencies vendored under `src/engine/reso/thirdparty/` (colocated with the engine that owns them; the `RESO_TP`/`RESO_INC` make vars scope the sources and `-I` to the reso build only):

- `rings` - Mutable Instruments Rings (only `dsp/` is used).

- `stmlib` - Mutable support library (hard dependency of Rings).

The vendored tree is **trimmed to the exact compile closure** - only the `.cc`/`.h` files reso actually pulls in (computed from the compiler's own `.d` dependency output), plus `stmlib/LICENSE`/`README.md`. This is ~34 files / ~320 KB, down from the ~50 MB full checkouts (the bulk was `stmlib/third_party` and Rings' `hardware_design`/`drivers`/`ui`/`bootloader`, none of which the DSP needs). If you later add a Rings model or stmlib helper that includes a header that was pruned, the build will fail with a "file not found"; re-fetch that file from upstream. Rings' MIT notice lives in each source-file header (retained by keeping the used files); stmlib keeps its top-level `LICENSE`.

Compiled sources for reso (engine + Rings DSP + stmlib): `reso_engine.cpp`, `rings/dsp/{part,string,resonator,fm_voice}.cc`, `rings/resources.cc`, `stmlib/dsp/{units,atan}.cc`, `stmlib/utils/random.cc`.

Three build gotchas, all handled:

1. **`-DTEST` on host only.** stmlib gates its ARM `ssat`/`usat`/`vsqrt` asm behind `#ifndef TEST`; the desktop build must define `TEST` to use the portable C paths. The firmware build must NOT define it (uses the asm). `host/Makefile` adds `-DTEST -I$(RESO_TP)`; the root `Makefile` adds `$(RESO_INC)` (`-I src/engine/reso/thirdparty`) without `-DTEST`.

2. **`-DM_PI` on the ARM build.** stmlib's filters use `M_PI`, which strict `-std=c++17` does not expose from `<cmath>` on `arm-none-eabi`. The reso branch of the root `Makefile` adds `-DM_PI=3.14159265358979323846`.

3. **`OPT = -Os` for reso only.** Rings (~30 KB of code+tables) overflowed the **then-186 KB** execution SRAM at `-O2` (overflow ~5.4 KB). **Worth revisiting:** the region is 260 KB since `993210f`, and reso now links at 69.1% *with* `-Os`, so the `-O2` overflow this works around almost certainly no longer exists. Re-measure before removing the override — `-Os` may still be wanted on CPU-headroom grounds, which is a separate question this note never answered. The reso branch sets `OPT = -Os`, which fits (~96%) and leaves other engines at `-O2`. (`-Os` was briefly suspected of miscompiling the pitch path; it does not - the pitch bug was elsewhere. A Rings-only `-O2` override was tried and reverted.)

Registered in: `src/engine/engine_select.h`, root `Makefile` (`ENGINE=reso`, `engine-reso` flash target), `CMakeLists.txt`, `host/Makefile` (`test-reso`).

The Daisy core Makefile already compiles `.cc` (not just `.cpp`), and object names are flat-by-basename with no collisions among the Rings/stmlib files.

### A dead end to not repeat: QSPI table relocation

To recover SRAM at `-O2`, an attempt routed the Rings lookup tables (`resources.o`/`units.o` rodata, ~24 KB) into the `QSPIFLASH` region via `alt_sram.lds`. It linked (SRAM_EXEC dropped to 89%) but produced a **1.7 GB `.bin`**: `BOOT_SRAM` builds one contiguous SRAM image, and `objcopy -O binary` zero-filled the address gap from SRAM (0x24000000) to QSPI (0x90000000). Reverted. Any future table-relocation needs proper LMA handling / a separate QSPI programming step, not a naive `> QSPIFLASH`.

---

## Files

- `src/engine/reso/reso_engine.h` - slim `IEngine` + opaque `Impl*` (no Rings includes).

- `src/engine/reso/reso_engine.cpp` - `Impl` with the Rings wiring, modes, schedulers, control map.

- `host/test_reso.cpp` - host tests (param round-trip, mode switch, Slice decay, Reel quiet-idle + sounds-on-trigger, Drift scatter, MIDI note, pitch-tracking, brightness control).

- Build edits: root `Makefile`, `CMakeLists.txt`, `host/Makefile`, `src/engine/engine_select.h`.

- Platform edit (affects all engines): `src/ui/core.ui.cpp` - see "absolute pitch" below.

Nothing is committed. `src/engine/reso/thirdparty/{rings,stmlib}` are vendored copies (MIT); consider submodules or retaining their licenses.

---

## Resolved: the PITCH-knob bug

**Symptom.** On the device, turning the PITCH knob did not change the audible pitch, even though an on-ring pitch dot tracked the knob. On the desktop host the full pitch path worked.

**Root cause.** `cv_voct()` (the V/Oct CV input handler) *overwrote* the knob's pitch every block. `read_cv()` in `src/ui/core.ui.cpp` calls `_engine.cv_voct(...)` for both decks at ~500 Hz, and the old handler did `deck[d].pitch_n = clampf(value, 0, 1)`. So `set_param(Speed)` would write `pitch_n` from the knob and microseconds later the next `read_cv()` clobbered it with the V/Oct reading. With nothing patched into the V/Oct jack, that reading is a constant, so `pitch_n` was pinned and the knob was inert. Worse, `CalibratedVOct::Process()` returns a *semitone offset* (0 at 0V), which the old code wrongly `clampf`-ed into `[0,1]` and treated as an absolute pitch - pinning the unpatched case to `pitch_to_note(0)` = C1.

**Why the dot lied.** The engine's own `render()` draws the dot from `pitch_n`, and `render()` runs in the same UI tick immediately after `set_param(Speed)` - before the next `read_cv()` clobber. So the dot captured the knob value while the audio path (one clobber later) saw the pinned value. The moving dot was the single most misleading signal in the whole hunt.

**Fix.** V/Oct CV is now an **additive transposition**, not an override. `cv_voct()` stores a per-deck `cv_semis` and touches nothing else; the pluck note is `base_note(dk) = pitch_to_note(pitch_n) + cv_semis`, routed through every pluck site (knob, gate/pad via `trigger_here`, Slice arp, Drift, MIDI). Unpatched -> `cv_semis == 0` -> the knob alone controls pitch; patched -> CV transposes on top. It does not rewrite `dk.note` per block, so it never clobbers a trigger's arp/drift offset.

**Regression test.** `host/test_reso.cpp` now calls `cv_voct()` around the pitch checks (the prior tests never did, which is exactly why the bug was invisible on host): it asserts the knob still spans pitch while `cv_voct(0)` is hammered in, and that `cv_voct(+12)` transposes up ~1 octave.

### Red herrings and secondary fixes found along the way

The hunt started by (wrongly) suspecting a gcc `-Os` miscompile of Rings and a frozen note->frequency path. Disproving those turned up three *real* but separate issues, all fixed:

- **`-Os` miscompile - disproven.** A diagnostic that drove `dk.note` directly in `process()` produced a clean pitched sweep on device, so the note->frequency path was never frozen. The Rings-only `-O2` override tried for this is reverted; reso is back to plain `-Os`.

- **Input feed into Slice/Drift (real fix).** The internal-exciter modes were fed the live audio input, which Rings sums into the resonator on top of the pluck - layering a continuous unpitched drone over every note (audible on the live ADC, silent on the host's zero input). Now they feed silence; only Reel takes external input. This is the "much clearer pitch differentiation" improvement.

- **Cycle default (real fix).** `MODFREQ`/`ModSpeed` ("cycle") was seeded to 0.3 by a shared literal in `core.ui.cpp`, above the Slice arp threshold, so the arp free-ran from boot (the "replucking"). The seed is now engine-derived (like Pos/ModAmp/Aux): reso seeds 0 (arp off), granular carries its own 0.3 so it is unchanged, edrums keeps its 0.0.

### Still open (voicing, not a bug)

The pluck attack is bright/percussive - a residual "thomp" over the clear fundamental. It is the Rings exciter transient (the excitation filter cutoff is `8 * filter_cutoff`, very bright). Tunable by lowering the exciter brightness coupling or level if a softer attack is wanted.

---

## Other notes / TODO

- The pitch dot in `render()` is drawn from `pitch_n`; note it reflects the *knob*, not the V/Oct CV transposition. It misled the pitch hunt (it tracks the knob even when the audio pitch was pinned); keep or restyle.

- Reel "drone": for a sustained drone, set `SIZE` high and trigger - it rings for a long time (there is no longer an unconditional drone).

- Drift randomization uses the engine's own LCG; Slice arp reads `ITransport::tempo()`.

- Two full `rings::Part` instances run every block; at `-Os` on the 480 MHz M7 there should be ample headroom, but CPU has not been measured on hardware (`Meter::cpu`).

- Models 4/5 (FM, string+reverb) are wired via `Aux`; reso stays at `-Os` (fits ~96%), so there is no pressure to drop them for SRAM.
