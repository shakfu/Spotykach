# graincloud - implementation notes

Developer notes for the `graincloud` engine (user doc: [`docs/engines/graincloud.md`](../engines/graincloud.md)).

## Architecture: a granular variant, not a standalone engine

`graincloud` is **the granular engine compiled with `SPK_GRAIN_GF`**, which swaps granular's grain DSP (`Generator`/`Vox`/`Window`) for a GrainflowLib cloud. The build compiles `src/engine/granular/*.cpp` with its own `SPK_ENGINE_GRAINCLOUD` define and its own IEngine wrapper (`graincloud_engine.cpp` substitutes for `granular_engine.cpp`); everything else - recording, SD storage, dual-deck, crossfade, FX, UI - is the same source, inherited unchanged. This replaced an earlier *standalone* graincloud engine that reimplemented all that plumbing from scratch and kept hitting platform-integration bugs (record gesture, storage crash, crossfade traps); inheriting granular's proven plumbing removed that entire class of problems.

> **History (2026-08-08).** This architecture was silently lost at some point: `src/engine/graincloud/` became a byte-for-byte **copy** of the granular tree - 35 of 42 files identical, ~3,400 duplicated lines - so fixes to `granular/` stopped reaching the published engine, and the two had begun to drift. The copy has been deleted and the `SPK_GRAIN_GF` design restored, with the guarded blocks living in the three files that actually differ (`generator.h`, `generator.cpp`, and two lines of `deck.cpp`).
>
> **Why the guards are in `granular/` rather than a shared `graincore/` directory.** `src/engine/granular/` is a deliberately *frozen* copy of the upstream Synthux Academy Core, kept diffable against it (see [`1.2.0-upstream-sync.md`](1.2.0-upstream-sync.md)). Relocating it to a neutral directory would have made every future upstream diff a rename-plus-edit. Keeping it in place and marking the three divergent regions with `#if SPK_GRAIN_GF` costs ~30 lines of clearly-labelled diff noise in files that were *already* divergent copies - strictly less upstream burden than the duplication it replaces.
>
> Because both engines now compile the same sources to the same object basenames with different flags - and `SPK_GRAIN_GF` **adds members to `Generator`** - a `granular`↔`graincloud` switch must invalidate every object. `build/.grainflavor-stamp` in the Makefile does that, in the same idiom (and for the same reason) as the `TERMINAL`/`USBDIAG` stamps.

The single seam: `Deck::process_out()` calls `_generator.process(bus[0], bus[1])` once per sample. Under `SPK_GRAIN_GF`, `Generator::process()` (generator.cpp) sums a `GfCloud` instead of the `Vox` array; `Generator` keeps its whole interface/state so `Deck`/`Drifter`/`granular_engine` compile and drive it unchanged.

## GfCloud (`src/engine/graincloud/gf_cloud.{h,cpp}`)

The GrainflowLib cloud core, reading granular's `Buffer`:
- **Per-block/per-sample bridge.** Granular is per-sample; GrainflowLib is per-block (96). `GfCloud::process(out0,out1)` serves one sample, recomputing a 96-sample stereo block at each boundary (`compute_block`).

- **Buffer-reader seam.** `gf_i_buffer_reader<Buffer,float>` callbacks read granular's `Buffer` via `read_linear` (one channel each), report `rec_size()` as the buffer length, and use a Hann LUT for the envelope. A non-finite-position guard prevents `(int)NaN` OOB reads.

- **Memory.** Grain scratch (grain array + io_config arrays) are members of two **static** `GfCloud` instances in regular RAM (zero-initialized BSS, ctors run at startup when RAM is ready) - acquired per deck via `gf_cloud_acquire(ref)`. Only the audio `Buffer` is SDRAM. No arena threading (Generator::init has no arena), no SDRAM-static-ctor hazard, no platform-boundary breach.

- **Whole TU guarded** by `#ifdef SPK_GRAIN_GF`, and it lives under `src/engine/graincloud/` rather than in granular's source wildcard - so a plain `ENGINE=granular` build never compiles it *and* never sees the GrainflowLib headers. Granular is unaffected either way; keeping the vendored library out of `src/engine/granular/` is what lets that directory stay a clean diff against upstream.

The de-STL'd GrainflowLib itself is vendored under `src/engine/graincloud/thirdparty/grainflow/` (the de-STL changes - heap/throw/RTTI removed - are marked `// sk:` in those headers).

## Knob -> cloud param mapping

`Generator::process` (under the flag) reads granular's stored params and calls `GfCloud::set_params`:
- start (POS) -> cloud centre; size (SIZE) -> grain duration; shape (ENV) -> density; smoothed increment (Speed) -> transpose; spread (Drift) -> position spray.

`GfCloud` derives overlap = `round(onset(density) * duration)` clamped to `[2, kMaxGrains]` (min 2 avoids a single-grain tremolo), sets the grain-clock period to the duration, and normalizes the mixdown gain by the active grain count.

## Build

`ENGINE=graincloud` -> the granular source wildcard *minus* `granular_engine.cpp`, plus `src/engine/graincloud/*.cpp`; includes `-Isrc/engine/graincloud -I<grainflow> -Isrc/engine/granular` **in that order** (so generator.cpp's guarded `#include "gf_cloud.h"` falls through to the graincloud dir); `-DSPK_ENGINE_GRAINCLOUD -DSPK_GRAIN_GF=1 -DM_PI=...`; **`-Os`** (granular + GrainflowLib templates overflow the execution SRAM at `-O2`). Wired identically in `Makefile`, `CMakeLists.txt` and `Makefile.cmake`, and both paths are built in CI.

Host tests: `test-graincloud-kernel` exercises the GrainflowLib DSP standalone, and `test-graincloud` drives the **whole assembled engine** (writes a known DC value into each deck's buffer through the storage port, then asserts the cloud output scales with it) - the test that proves the shared-source build is wired up correctly. Plain granular's `test-granular` covers the inherited plumbing.

## Risks / watch-items

- **On-device scattered-read cost unconfirmed** (no hardware profile yet). If high-density patches glitch, lower `kMaxGrains` in `gf_cloud.h` or run `METER=1`.

- **kMaxGrains=8/deck** is conservative for CPU; the per-sample `exp2f` in GrainflowLib's `increment` (fm is always 0 here) is the next optimization if more grains are wanted.

- **SDRAM is not zero-initialized**; the granular `Buffer` is memset by granular's own init, and GfCloud's scratch is regular zeroed BSS - so the uninitialized-memory class of bug does not apply here.
