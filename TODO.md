# TODO

Deferred work, in priority order (highest first). See `docs/` for the platform/engine design and `CHANGELOG.md` for done work.

> **Update 2026-08-01 - the bench session is now instrumented.** This file was written as if P2 could
> only be a manual listening pass. That predates the USB-C terminal channel, which was fixed and verified
> on hardware 2026-07-31 (root cause: the panel jack is on OTG_HS, not OTG_FS - see
> [`terminal-impl.md`](docs/dev/terminal-impl.md)). Two things landed since, both host-verified:
>
> - **`query cpu` / `cpumin` / `cpumax` + `reset cpu`** - the platform's `CpuLoadMeter` is now readable
>   over the channel, so P2's headroom numbers are `reset cpu` -> drive the engine -> `query cpumax`,
>   scripted per engine. Previously the meter needed `METER=1`, which brings up a second USB device on
>   the same OTG core the terminal uses - the numbers and the channel that would collect them were
>   mutually exclusive.
> - **`live_params()`/`live_configs()` on every engine** - the stated blocker on `make test-hw`. The
>   generic sweep no longer sets params engines ignore, so the mechanical half of P2 can run unattended.
>
> Neither is hardware-verified yet; both fold into the P2 session.
>
> **And one regression found on the way - now FIXED and hardware-verified.** `pstretch` had stopped
> building entirely: not "cannot host the terminal" but no link at all, terminal or not, at either
> window (`region SRAM overflowed by 80576 bytes` on the committed `v0.6.1` tree). Commit `993210f`
> moved 114K from the data `SRAM` region into `SRAM_EXEC` (186K -> 300K) so the channel would fit
> everywhere; pstretch's FFT working set needed 297664 B of exactly that region.
>
> Fixed with a per-engine linker script, `linker/alt_sram_pstretch.lds` (200K/312K instead of
> 300K/212K), selected automatically on `ENGINE=pstretch` so a plain `make ENGINE=pstretch` is correct
> too. Same approach and precedent as `linker/alt_qspi_chuck.lds`, and it leaves the other 20 engines on
> the 300K split untouched. **All three pstretch images flashed and confirmed working on hardware
> 2026-08-01** (8192 with and without the terminal, and 4096 with it) - so the moved code/data boundary
> boots, which a link check could not have established.
>
> **P2 is therefore no longer purely pending - its first engine is measured.** `make test-hw` ran
> against real hardware for the first time (`30 passed, 3 skipped`), and pstretch has CPU numbers:
>
> | | WINDOW=8192 | WINDOW=4096 |
> |---|---|---|
> | CPU avg / max | 41.5% / **87.6 -> 91.3% (still climbing)** | 33.3% / **63.6% (converged)** |
> | `SRAM` (312K) | 97.40% | 82.01% |
>
> **Open decision - pstretch's default window is now a voicing call with evidence attached.** 4096 is
> the only config with real margin on both axes at once (~36% CPU, ~50 KB data, vs ~9% and ~8 KB), and
> its peak has converged where 8192's had not - so 8192's true worst case is unknown and worse than
> 91%. But 4096 is a shorter smear (~85 ms vs ~171 ms) and the long wash may be the point of the
> engine. **Default stays 8192 until someone listens to both.** Numbers in
> [`docs/engines/pstretch.md`](docs/engines/pstretch.md).

Priority is driven less by size than by what unblocks/gates what, and by whether an item is **build-verifiable on the host** vs. **hardware-gated** (needs a flash to verify). Most of the open work is now hardware-gated and has piled up: several engines have been *flashed and heard informally but not rigorously measured/voiced* - they sound alive, but CPU headroom (`Meter::cpu`) and the full voicing range haven't been pinned down. The dominant move is therefore a single bench session (P2) that does that measured pass; the remaining items are a deliberate code refactor (P3), an optional voicing tweak (P4), and a strategic build-system decision (P5). Ahead of all of them sits **P0**: the desk/host audit of whether every engine uses the full UI/indicator grammar is **done** (see `indicator-comparison.md` §7), leaving the ranked toolkit migration as the top actionable item (its apply step is hardware-gated and folds into P2).

| # | Item | Effort | Risk | Verify | Gating |
|---|------|--------|------|--------|--------|
| P0 | Migrate engines onto the indicator toolkit (audit DONE; apply is the remaining work) | med | low-med | **hardware flash** | per-engine worklist ready; apply/confirm folds into P2 |
| P1 | Mono-input: answer the normalling question | trivial | n/a | a fact | unblocks/kills its own code item |
| P2 | One bench session: measure + voice the engines flashed-but-not-quantified | low-med | med | **hardware flash** | turns "sounds fine" into measured CPU headroom + confirmed voicing range |
| P3 | Refactor delay engine onto shared primitives (by ear) | med | med-high | **hardware flash** | none (primitives in `dsp/`); folds into P2 |
| P4 | Tape wow/flutter: try quadratic curve + lower maxima | trivial | low | **flash** (by ear) | none (optional voicing); folds into P2 |
| P5 | Finish or back out the CMake adoption (now merged to `main`, incomplete) | high | high | flash + cleanup | strategic; three build-system files straddle `main` |

---

## P0 - Migrate engines onto the indicator toolkit (mechanical migration DONE 2026-07-31)

**Audit + mechanical migration complete.** Every engine was checked against the full visual grammar ([`docs/dev/indicator-grammar.md`](docs/dev/indicator-grammar.md); per-engine table in [`indicator-comparison.md` §7](docs/dev/indicator-comparison.md#7-audit-refresh-2026-07-31-todo-p0--full-current-tree-pass)) and then migrated onto the shared `src/engine/indicators.h` toolkit. All 15 own-display engines now include it; the duplicated hand-rolled grammar is retired. **All engines build clean** on ARM (`make dist` + clean per-engine builds; SRAM_EXEC unchanged-to-slightly-lower).

**Done (the mechanical dedup):**

- **`ring::selector` / `ring::slots`** — retired all 9 hand-rolled Alt-held selectors (bard shelves, csound+chuck patches [shared verbatim], softcut slots, radio banks, glitch algos, pstretch clips, reso models, mosc engines).
- **`led::route_leds`** — replaced the byte-for-byte route L/C/R block in bard, softcut, radio, glitch, pstretch, mosc, delay, qdelay; **added** to edrums (previously showed no route feedback).
- **`pal::` sweep** — unified the palette across every migrated engine, killing the `0x00c0ff`/`0x00aaff`/`0x00a0ff`-near-`kCyan` drift and reso's Reel/Slice/Drift hue drift.
- **`ring::level` / `ring::playhead`** — meters + markers (csound/chuck meters, reverb baseline+decay, delay/qdelay division arcs, radio/glitch/pstretch markers, reso/mosc pitch dots).
- **`motion::breathe_standby` + `transport_view`/`led::transport`** — softcut (replaced its hand-rolled cos breathe + transport-colour ladder; `kErrColor` was already `== pal::kErr`).
- **Faust floor** — `chorus`/`filter`/`voice` meter path → `ring::level`; added a static dim mode-hued "on, ready" floor for the `meter=false` case (no `ITimeSource` in that render, so static rather than a breathe).
- **`led::cycle`** — bard (follow/duck indicator).

**Deferred — net-NEW indicators needing per-engine data plumbing + hardware verification (fold into P2):**

- **`ring::value` pickup feedback** — the biggest remaining expressive gap. Needs each engine to track edit-param/knob/picked-up in `render()` (as `shuttle` does); `softcut`/`pstretch`/`reso` don't yet.
- **`led::clock`** for `reso`(CapTransport)/`delay`/`qdelay`(tempo-synced)/`edrums` — needs the clock source surfaced into `render()`.
- **`led::cycle`** for the LFO/mod engines (`reso` arp/drift, `mosc` CV, `delay`/`qdelay` mod LFO, `reverb` greyhole ModDepth) — needs the modulator phase/depth in `render()`.
- **Breathe** on the still-static-when-idle engines (reso/mosc/delay/qdelay/reverb) — their `render()` has no `ITimeSource`.

**Note:** LED changes are **not** hardware-verified yet (render() is hardware-only; host tests can't exercise it — and the host harness is separately broken pre-existing on `granular/detector.h`). Confirm on the panel during the P2 bench session.

## P1 - Mono-input normalization (left -> right when right is unused)

Highest-leverage *decision* before any code: answering one hardware fact either kills this item or scopes it. The fix itself (raised while testing the stereo delay, engine #2: a mono source into the left input left the right delay tap silent) is to mirror left -> right so a mono source feeds both channels.

**Resolve first - is the right input jack physically normalled to the left?** (Not answerable from the repo; needs the board schematic or a bench check.)

- **Hardware normalling** (preferred if the board supports it): the right input jack normals to the left when nothing is plugged in - automatic, firmware does nothing, and **this whole item is moot** (delete it).

- **Software fallback** (only if NOT normalled - and then this code is hardware-gated, batch into P2): detect a near-silent right input (peak below a small threshold over a window) and copy left -> right. Needs hysteresis/timing so it doesn't flap, and it's a *platform* input concern (applies to any engine), so it belongs in the platform's audio path (e.g. `AppImpl::ProcessAudio` before `engine.process`), not in an individual engine. Caveat: silence-detection can't tell "cable plugged but quiet" from "no cable".

## P2 - One bench session: measure + voice the engines that work but aren't quantified (HARDWARE-GATED)

These engines **have been flashed and heard** - they boot and sound alive on the unit. What's missing is the *measured* pass: real CPU headroom (`Meter::cpu`) and a deliberate sweep of the full voicing range, neither of which a host test or a casual listen establishes. Do it as a single bench session and capture the numbers:

> **The measured pass is now scripted, and pstretch is done (2026-08-01).** Read CPU over the terminal
> instead of `METER=1` (which cannot coexist with the channel - it wants the same OTG core):
>
> ```
> make ENGINE=<e> TERMINAL=1 && make ENGINE=<e> TERMINAL=1 program-dfu   # device in DFU
> reset cpu  ->  drive the engine  ->  query cpu / cpumin / cpumax
> make test-hw                                                          # 30-case sweep, needs dialout
> ```
>
> **Sample the peak more than once.** `max` is "worst block since `reset cpu`", so re-reading it shows
> whether it has converged. pstretch at 8192 was still climbing after six seconds (87.6 -> 91.3%) while
> 4096 settled within 0.3 pp - a difference invisible in a single reading, and the thing that actually
> decides whether an engine has headroom. Also read `min`: it is the platform floor (~2%) with the
> engine's DSP idle, which separates "expensive" from "spiky".
>
> Done: **pstretch** (41.5% avg / 91.3%+ max at 8192; 33.3% / 63.6% at 4096 - see the header note).

- **reverb + tape Faust DSP - CPU + voicing.** Heard on hardware, but the Jiles-Atherton hysteresis (tape) and FDN/plate reverb DSP cost hasn't been measured. CPU: flash `ENGINE=reverb` and `ENGINE=tape`, read `Meter::cpu` for the stereo paths (J-A runs 4 substeps/sample x 2 voices/decks; estimated ~10-25% of 480 MHz but unmeasured). If too hot, the levers are a polynomial Langevin approx or an ADAA-tanh saturator. Voicing: walk the full range - the tape `drive*54` dB clean->crunch sweep across its span, and the reverb's three Faust voices (Dattorro plate / Zita hall / Greyhole, `kReverbCount = 3`, selected per deck on the Mode switch) with a click-free algorithm switch. Levers live in `src/engine/{reverb,tape}/*.dsp`; re-tune and `make faust-gen`. (reverb and tape are already released on `main` - this is a voicing/CPU pass, **not** a merge gate. gigaverb is **excluded** from the reverb engine - the optional `REVERB_GIGAVERB=1` fourth voice overflows SRAM_EXEC and stays out; gigaverb ships only as the standalone `ENGINE=gigaverb`.)

- **tape post-FX resonant low-pass + soft-limited bus.** Confirm the grit+PITCH/grit+MIX cutoff/resonance sweep behaves across its range and that two decks + a high resonance peak don't clip the codec under the soft-limiter.

- **shuttle engine.** Four-track buffer varispeed (reverse/freeze/loop window) + per-track pan + routing switch; builds at ~82% SRAM_EXEC. Confirm CPU under all four tracks rolling and the routing/pan voicing.

- **qdelay - first bring-up done (boots + makes sound, 2026-06-29).** Flashed and confirmed working on the H7. Remaining is the measured pass like the others: confirm the diffusion wash depth and the duck attack/release feel across SIZE, and read `Meter::cpu` with the diffuser engaged on both decks (links at ~77% SRAM_EXEC). Flash with `make engine-qdelay`.

- ~~**delay Reverse pad.**~~ Done (2026-06-29): flashed, reverse read confirmed click-free across delay lengths on hardware.

- **P1 software fallback** (only if P1 says "not normalled"), the **P3 delay refactor** by-ear check, and the **P4 wow/flutter** voicing experiment all fold into this same session.

## P3 - Refactor the delay engine onto the shared primitives (HARDWARE-GATED)

The shared primitives are in `dsp/` (the `.cpp` tier move is done), so the prerequisite is satisfied. This is the concrete second consumer that justified the tier: the delay reimplemented one-pole smoothing and a fractional delay line, which now live in `src/dsp/smooth.h` and `src/dsp/deline.h`. The delay engine deliberately kept both primitives inline (`delay_engine.cpp`: smoothing at `:132`, linear fractional read nearby; no `dsp/` include). It **CHANGES the delay's DSP** - the shared versions are *not* bit-identical drop-ins, confirmed by inspection:

- **Smoothing divergence.** The inline glide (`s_delay += (target - s_delay) * kSmooth`, every sample, no dead-zone, never snaps) differs from `OnePoleSmoother` (`smooth.h`), which adds a dead-zone short-circuit and a snap-to-target within `.002f`. The coefficient is matchable but the dead-zone/snap changes the trajectory. Since the delay smooths the *delay time itself*, that snap is audible as a different pitch-glide on knob moves.

- **Structural mismatch on the delay line.** `DeLine` (`deline.h`) uses the same `a + (b-a)*frac` interpolation but is a **fixed-size template** (compile-time `max_size`) with a decrementing write pointer + modulo wrap. The delay engine allocates a **runtime-sized** buffer from the arena with a forward-indexed read pointer. Adopting `DeLine` requires resolving fixed-vs-runtime sizing, not just a numeric swap.

So do it deliberately with a hardware flash test (judge by ear, not by bit-identity), not a silent swap. Low payoff (no functional gain, the engine works) and med-high risk, so it sits below the bench-drain - fold its by-ear check into P2 when convenient. Note `qdelay` already became the second real consumer of the `dsp/` tier (`dsp/diffuser.h`), so the tier is no longer unjustified even if the delay is never refactored.

## P4 - Tape wow/flutter rate: experiment with a quadratic curve and lower maximums

Optional voicing tweak, not a defect. The MODFREQ ("cycle") knob -> wow/flutter rate map in `src/engine/tape/tapefx.dsp:36-38` is a **cubic** curve with a low floor:

```c
rc    = rate * rate * rate; // favor very low frequencies, increase slowly
wowHz = 0.1 + rc * 2.4;     // 0.1 .. 2.5 Hz
fltHz = 0.5 + rc * 11.5;    // 0.5 .. 12 Hz
```

This is good enough as-is, but it's worth experimenting with two softer variants:

- **Quadratic instead of cubic** (`rc = rate * rate`): a gentler favor-low. Cubic keeps the rate very slow until ~0.7 of knob travel, which may push the usable fast-wobble range too far up; quadratic spreads it out more evenly.

- **Lower the maximums somewhat**: drop the `2.4` / `11.5` multipliers so the top of the knob tops out below the current 2.5 Hz wow / 12 Hz flutter.

Levers are the three lines above; re-tune and `make faust-gen` (regenerates `faust_kernel_tapefx.h`), then evaluate by ear. Purely subjective, so it's flash-gated and low priority - fold into P2 alongside the tape voicing pass. See `docs/engines/tape.md`.

## P5 - Decide the CMake adoption (the spike was merged to `main` but not finished)

**Status update:** the former `spike/cmake-build` branch was **merged into `main`** (`merged karp engines / cmake`, then `update cmake builds`), so `CMakeLists.txt` and `Makefile.cmake` now live on `main` **alongside the original `Makefile`** - the exact "all three build-system files straddling `main`" state the spike notes warned not to ship. The original `Makefile` is still the documented, canonical firmware build (the README's `make ENGINE=...` instructions); CMake rides along, actively maintained, but **unadopted and host-only** - no hardware flash of a CMake `.bin` has been confirmed. So this item is no longer "evaluate a spike"; it is **"finish adoption or back it out,"** and the coexistence is a small standing liability (the engine list is now duplicated across all three files).

The original justification still holds: CMake is worth adopting **only** if committing to the compiler-enforced platform/engine boundary (per-target `target_include_directories(... PRIVATE)`) plus multi-engine growth - **not** for aesthetics, since the grep-guard (`make check-boundary`) and `bear` already cover the boundary and clangd flags. The decision is binary; do not leave three build files on `main` indefinitely.

**To adopt** (close all five; 1-4 are independent of the flash, 5 is the gate):

1. **Collapse the engine-list duplication.** The list lives in `Makefile`, `CMakeLists.txt`, and `Makefile.cmake` today. On adoption: delete the old `Makefile`, rename `Makefile.cmake` -> `Makefile`; the list then lives only in `CMakeLists.txt` and the wrapper forwards `ENGINE=`. This is the main reason the current `main` state is a liability.

2. **Decide the `midi_util.cpp` fix: upstream vs local patch.** The spike papers over libDaisy's CMake gap with `target_sources(daisy ...)`. Either PR the missing source into the bleeptools libDaisy fork's `CMakeLists.txt` or keep the local patch (risk: a future libDaisy bump double-compiles or moves the file). Same call for `per/pwm.cpp` if any engine ever uses `daisy::Pwm`.

3. **Unify the host build.** `host/` still has its own Makefile; fold it into CMake for the stated "one build system for firmware + host" benefit. Not attempted in the spike.

4. **Build the compiler-enforced boundary (the actual headline justification).** Implement the per-target include roots that turn a platform->engine include into a compile error instead of a grep hit - the real reason to adopt. Needs the engine and platform split into separate targets with private includes. Without it, CMake is only the aesthetic win this item says is not worth it.

5. **Flash-verify each image you intend to run.** All engines build host-side under CMake, but only a bench flash confirms boot + audio/IO. **Revised acceptance:** adopt iff a hardware flash of the CMake `.bin` boots and passes a smoke test (byte-identity across two build systems is unreachable and not the bar). If the boot path fights it on hardware, back the CMake files out and stay on Make.

### Spike reference (host-side findings, still accurate)

The boot path is the one real risk and it collapsed to one define: `BOOT_APP` (the only boot-relevant compile define, at `startup_stm32h750xx.c:1550`) is not set by `DaisyProject.cmake` when `CUSTOM_LINKER_SCRIPT` is used, so a naive port re-runs `SystemInit()` and likely won't boot - fix is one line, `target_compile_definitions(daisy PRIVATE BOOT_APP)`. Two more traps (both fixed in the spike): `USE_HAL_DRIVER`/`USE_FULL_LL_DRIVER` are PRIVATE on the daisy lib so they never reach app TUs (breaks bare `size_t` users like `detector.h:11`), and `hid/midi_util.cpp` is in libDaisy's Make module list but absent from its `CMakeLists.txt` (link failure, patched via `target_sources`). Parity achieved: memory map exact (vector table `0x24000000`, `.bss` byte-identical), `.text` within +1.0%; byte-identical objdump is **not** reachable across two build systems (different per-domain flags) and must not be chased. `program-dfu`/`program-boot` reproduced as `add_custom_target`s emitting byte-identical `dfu-util` invocations; `compile_commands.json` falls out natively (no `bear`); the multi-engine matrix works as cached per-engine build dirs, retiring the `.engine-stamp` hack.
