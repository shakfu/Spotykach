# Dev notes — dual macro-oscillator (`mosc` engine)

Implementation/bring-up notes for `ENGINE=mosc`. The user-facing reference (concept, modes, control map, CV map, build commands) is [`docs/engines/mosc.md`](../engines/mosc.md); this file holds the internals, the build system, the file map, and the design decisions.

## Status at a glance

- Implemented and integrated end to end (engine, build system, QSPI flash target).

- Builds and links: `make engine-mosc` → `.text` 289,952 B at LMA `0x90040298` (QSPIFLASH, ~3.5% of 8 MB); the 48 MB engine arena is present (`.sdram_bss` ≈ 50 MB bss).

- ARM compile of all 24 Plaits engines + `voice.cc` + `resources.cc` (~378 KB of LUTs) + `stmlib` is clean.

- **Hardware-verified on the cased unit**: both decks sound, engine select (Alt+PITCH) scrolls all 24, the spare CV jacks modulate harmonics/timbre, and the routing switch (Stereo / DoubleMono / GenerativeStereo) works.

- Open item: dual-voice CPU load not yet measured (`METER=1`). Two full Plaits voices from QSPI; the worst case is both decks on a heavy engine (speech / additive / 6-op FM / grain).

---

## Architecture (PIMPL, one voice per deck)

`MoscEngine` (`src/engine/mosc/mosc_engine.h`) exposes only the `IEngine` interface plus an opaque `struct Impl* _p`. All Plaits/stmlib types live in `mosc_engine.cpp` inside `struct MoscEngine::Impl`.

This is mandatory, not stylistic — the same trap as reso: `stmlib/stmlib.h` declares a global `namespace impl` (its `STATIC_ASSERT` helper). The composition root `src/app.cpp` has a `static AppImpl impl;` and includes the engine header via `engine_select.h`; if the header pulled Plaits/stmlib, that `namespace impl` would collide with `app.cpp`'s `impl` instance and `app.cpp` would fail to compile. Keeping Plaits out of the header avoids it.

`Impl` owns one `Deck` per deck; each `Deck` holds a `plaits::Voice` (`sizeof ≈ 8.9 KB`), its `Patch`/`Modulations`, and the normalized knob/CV state. `Impl` (both voices) is placement-new'd into the injected SDRAM arena at `init()`. Each voice also gets its own **16 KB `stmlib::BufferAllocator` scratch** carved from the arena — Plaits resets the allocator between engines in `Voice::Init`, so all 24 engines for a deck share that block (only the active one is live), exactly as on the original hardware (`shared_buffer`, 16384 B in `plaits.cc`).

Plaits renders at 48 kHz in **`kBlockSize = 12`**-sample chunks; the platform's 96-frame block is processed 12 at a time. The chunk size matters: Plaits' decay/LPG time constants are written in terms of `kBlockSize`, so rendering must be called with `size == kBlockSize` (not the full 96, and not `kMaxBlockSize = 24`) to keep envelope timing correct.

**Engine select.** `Voice::Render` feeds `patch.engine` to a `HysteresisQuantizer2` as the **integer base** (the continuous `modulations.engine` CV adds an offset, which mosc leaves at 0). So an int-valued `patch.engine` selects a model directly; `set_param(Aux)` maps the knob 0..1 → `round(v * 23)`.

**Dual-deck routing.** `process()` renders **both** voices for each 12-sample chunk *before* mixing, so the cross-deck routes have both signals on hand:

- `Route::Stereo` — `L = A.out`, `R = B.out`.

- `Route::DoubleMono` — `L = R = (A.out + B.out) * 0.5`.

- `Route::GenerativeStereo` — `L = (A.out + B.aux) * 0.5`, `R = (B.out + A.aux) * 0.5`. This is the only place Plaits' **aux** output is used (otherwise discarded); summed modes are ×0.5 to avoid clipping. Per-deck level metering is taken from each voice's own `out` (not the mixed channel) so the rings stay per-deck.

**Triggers.** A trigger (Play pad / gate-in / MIDI note / Seq) holds `modulations.trigger` high for a few render blocks so `Voice` sees a clean rising-then-falling edge (its `trigger_delay_` is a per-block shift register). Gate mode sets `trigger_patched = true` (the LPG/decay envelope fires on trigger); Drone mode sets it `false` (LPG bypassed, engine runs open). V/Oct CV is **additive** (`cv_semis`, summed into the note via `base_note()`), never an override — the same fix reso applies so the PITCH knob is not clobbered by an unpatched jack.

---

## Build system

mosc is a **QSPI-execute target**. The full 24-engine voice is ~292 KB of `.text`, which overflows the 186 KB `SRAM_EXEC`, so it builds `BOOT_QSPI` with the QSPI linker script and executes from the 8 MB QSPI flash:

```text
make ENGINE=mosc APP_TYPE=BOOT_QSPI LDSCRIPT=alt_qspi.lds   # (== make engine-mosc)
```

This is the same path [csound](../engines/csound.md)/[chuck](../engines/chuck.md) use, with two differences:

1. **mosc keeps the engine arena.** It synthesises from `ctx.arena` (placement-new'd voices + scratch), so it does **not** set `SPK_NO_ENGINE_ARENA` — the 48 MB arena stays (csound/chuck use their own pool and shrink it to a token block).

2. It reuses the csound engine's **VTOR inject** (`src/engine/csound/spotykach_qspi_vtor.cpp`, engine-agnostic): a high-priority global constructor points `SCB->VTOR` at the QSPI app base so SysTick + the audio DMA IRQ reach the app under `BOOT_APP`.

Build defines on the `ENGINE=mosc` branch: `-DSPK_ENGINE_MOSC`, `OPT = -Os` (density / I-cache), `-DM_PI=…` (stmlib filters use `M_PI`, which strict `-std=c++17` does not expose on `arm-none-eabi`), and `-DPLAITS_USER_DATA_STUB` (see below).

### Vendored DSP + shared stmlib

- **Plaits** is vendored under `src/engine/mosc/thirdparty/plaits` (the whole `dsp/` tree + `resources.{h,cc}` + the stubbed `user_data.h`; the firmware-level `plaits.cc`/`ui.*`/`settings.*`/`user_data_receiver.*` are not used). `MOSC_TP`/`MOSC_INC` scope `-I` to the mosc build.

- **stmlib** is **shared** with reso. The Mutable support library is identical across the two engines' closures (same upstream, unchanged for years — verified byte-for-byte against reso's old copy), so it is vendored **once** at `src/engine/common/thirdparty/stmlib` (a `common` engine-slot, because `.gitignore` ignores bare `thirdparty/` but un-ignores `src/engine/*/thirdparty/**`). Two make vars at the top of the `Makefile` — `STMLIB_TP` and `STMLIB_INC` (`-Isrc/engine/common/thirdparty`) — are added to **both** the reso and mosc branches; both pull the three `.cc` (`dsp/units.cc`, `dsp/atan.cc`, `utils/random.cc`) from `$(STMLIB_TP)`. The shared copy is trimmed to the **union** of the reso (Rings) and mosc (Plaits) needs (19 files). reso's former `src/engine/reso/thirdparty/stmlib` was deleted; reso's tree is now `rings` only.

Compiled sources for mosc: `mosc_engine.cpp`, the VTOR inject, `plaits/dsp/voice.cc`, every `plaits/dsp/engine/*.cc` + `engine2/*.cc` + `physical_modelling/*.cc` + `speech/*.cc`, `chords/chord_bank.cc`, `fm/{algorithms,dx_units}.cc`, `plaits/resources.cc`, and the three shared `stmlib` `.cc`.

### The `user_data.h` stub

Plaits' `user_data.h` is its on-device patch storage for the original STM32F37x — its non-`TEST` branch pulls `<stm32f37x_conf.h>` + `stmlib/system/flash_programming.h`, neither of which exists here. The vendored copy adds a third branch guarded by **`PLAITS_USER_DATA_STUB`** (set on the mosc Makefile branch): `UserData::ptr()` returns `NULL` and the FLASH ops are no-ops. `Voice::Render` already falls back to the built-in `fm_patches_table` when `ptr()` is `NULL`, so the 6-op FM engines keep their factory patches; only user-saved patches (which this firmware can't store anyway) are unavailable. `user_data_receiver.cc` is not compiled, so `Save()` is never ODR-used.

Registered in: `src/engine/engine_select.h` (`SPK_ENGINE_MOSC` → `MoscEngine`), root `Makefile` (`ENGINE=mosc` branch + `engine-mosc`/`program-mosc` targets + the shared `STMLIB_*` vars).

---

## Engine order

`set_param(Aux)` → `patch.engine` (0..23). The index is Plaits' registration order in `Voice::Init` (newer "engine2" bank first, then the classic 16). Boot default = **8 (Virtual Analog)**.

| # | Engine | # | Engine |
|---|---|---|---|
| 0 | Virtual Analog VCF | 12 | Additive |
| 1 | Phase Distortion | 13 | Wavetable |
| 2 | 6-op FM (A) | 14 | Chord |
| 3 | 6-op FM (B) | 15 | Speech |
| 4 | 6-op FM (C) | 16 | Swarm |
| 5 | Wave Terrain | 17 | Noise |
| 6 | String Machine | 18 | Particle |
| 7 | Chiptune | 19 | String (plucked) |
| 8 | Virtual Analog (default) | 20 | Modal |
| 9 | Waveshaping | 21 | Bass Drum |
| 10 | 2-op FM | 22 | Snare Drum |
| 11 | Grain | 23 | Hi-Hat |

---

## Files

- `src/engine/mosc/mosc_engine.h` — slim `IEngine` + opaque `Impl*` (no Plaits includes).

- `src/engine/mosc/mosc_engine.cpp` — `Impl` with the Plaits wiring, per-deck Voice, param/CV map, routing, display.

- `src/engine/mosc/thirdparty/plaits/` — vendored Plaits DSP (114 files: `dsp/` tree, `resources.{h,cc}`, stubbed `user_data.h`).

- `src/engine/common/thirdparty/stmlib/` — shared trimmed stmlib (19 files), used by reso **and** mosc.

- Build edits: root `Makefile` (`ENGINE=mosc` branch, `engine-mosc`/`program-mosc`, shared `STMLIB_*` vars, reso branch repointed at the shared stmlib), `src/engine/engine_select.h`.

- Docs: `docs/engines/mosc.md`, `docs/diagrams/controls/mosc.json`.

---

## Open items / TODO

- **CPU not measured.** Build `make engine-mosc METER=1` and read the on-panel meter; worst case = both decks on a heavy engine. M7 @ 480 MHz has ~10× the headroom of Plaits' original 72 MHz M4F running one voice, but QSPI execution (cache misses on cold code) is the variable. If tight: pin the hot inner loops to ITCM, or run a single shared voice.

- **No host test yet.** A `host/test_mosc.cpp` (param round-trip, engine select, route mixing, trigger) would mirror `test_reso`. Note the host build would need `-DTEST` so stmlib uses its portable C paths (the firmware build must not define it).

- **CV depth tuning.** `kHarmCvDepth` / `kTimbreModAmt` are first-guess (1.0); the true full-scale CV magnitude wasn't measurable from source, so these may need a nudge on hardware.

- **Mod coverage is thin by design.** Knob coverage is 100% of Plaits' panel; CV coverage is pitch + harmonics + timbre (3 of ~7). The Spotykach simply has fewer jacks than Plaits.
