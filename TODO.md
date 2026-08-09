# TODO

Deferred work, in priority order (highest first). See `docs/` for the platform/engine design and `CHANGELOG.md` for done work.

- [ ] add web controls in the web frontend to control the OSC layer / protocol.
      (The OSC codec itself is **built** as of 2026-08-09 — `make ENGINE=<e> TERMINAL=1 OSC=1`, see the
      P7 section below. What this item wants is the browser end, and it is not a small addition: the web
      front-end talks to nothing today, and a browser cannot open a USB CDC serial port without WebSerial
      (Chromium only, requires a user gesture per connection, and no Safari). Decide that first — the
      realistic options are WebSerial with a graceful "unsupported browser" path, or a small local bridge
      process the page talks to over a WebSocket. The address space and the semantic tier are already
      generated host-side by `tools/skdev/semantic.py`, so whichever transport wins, the naming work is
      done.)

## Done 2026-08-08 — the review's host-verifiable items

Closed out from `REVIEW.md`; see the sections below for what remains.

- [x] **CI** (`.github/workflows/ci.yml`) — 22 firmware builds (20 engines + `TERMINAL=1` on two), the
      four off-target suites + `check-boundary`, and a CMake build of the six flag-sensitive engines.
      `csound`/`chuck` are in `qspi-libs.yml` because each cross-builds a large runtime first. This is
      the standing answer to the `chorus`/`pstretch` class of breakage (an engine that stops linking
      and nobody notices) and to P5's CMake drift.
      - [ ] **Arm the automatic triggers.** Both workflows are `workflow_dispatch`-only for an initial
            period; the push/PR and weekly blocks are written out and commented in each file. Dispatch
            each once from the Actions tab, confirm green, then uncomment. **Until this is done the
            safety net is not actually deployed** — the whole value of CI is that it runs on a commit
            nobody thought to check. First likely friction on a real runner: the
            `carlosperate/arm-none-eabi-gcc-action` pin (`10-2020-q4`, matching the local 10.2.1).
- [x] **`Divider::_triplets_on` was never initialized** — omitted from the constructor's init list and
      with no default member initializer, so it read as indeterminate. Masked on target by `.bss`
      zeroing (every Divider lives inside the file-static `AppImpl`), live UB on the host, and the
      reason 7 checks in `test/test_divider.cpp` failed. `test/` is now 116/116. Note that
      `set_triplets_on()`/`set_swing()` are still **reached from nowhere in the firmware** — implemented,
      integrated into `tick()`, tested, and unreachable by any gesture. Kept deliberately (see the note
      in `src/dsp/divider.h`); wiring them to a gesture is open work.
- [x] **The `web/` export was order-dependent** — `sk_card.verify_card` walked with an unsorted
      `os.walk`, so the finding ORDER varied by filesystem while the committed fixture is compared
      byte-for-byte. It could fail on another machine with identical code. Sorted at the walk and again
      in the exporter.
- [x] **graincloud/granular duplication removed** — `src/engine/graincloud/` was a byte-for-byte copy of
      the granular tree (35 of 42 files identical, ~3,400 lines) that had begun to drift, with the
      *published* engine being the copy that received fewer fixes. Restored to the `SPK_GRAIN_GF`
      design its own impl doc already described. `src/engine/granular/` did **not** move — see
      `docs/dev/graincloud-impl.md` for why (upstream diffability).
- [x] **`host/test_graincloud` was broken and excluded from `make -C host test`** — a missing `<cstdint>`
      in the vendored GrainflowLib headers. Fixed and wired into the suite; it now covers the whole
      assembled engine, not just the kernel.
- [x] **Doc sweep** — 7 broken links fixed, a stale `CLAUDE.md` reference dropped, a stray tool-call
      artifact removed from `chuck-midi-in-porting.md`, the stale "186 KB SRAM_EXEC" figure in
      `architecture.md` corrected, plus `make help` and a real `.PHONY: test`.

- [x] **Host tests for `granular` and `mosc`.** `host/test_mosc.cpp` instantiates and renders **all 24
      Plaits engines**, plus Gate-vs-Drone, level, and the `live_params` round-trip — mosc was the
      largest engine in the tree with no off-target coverage, and being a QSPI build it is not even
      exercised by the normal `ENGINE=` sweep. `host/test_granular_audio.cpp` covers granular's **audio
      path** (load → play → level scaling, record → playback, all three modes); the pre-existing
      `test_engine_params.cpp` already covered its *parameter surface*, which the review missed —
      correction noted in `REVIEW.md`.
- [x] **P5, the CMake decision — resolved: keep both, Makefile canonical.** See P5 below and
      [`docs/dev/cmake-gap.md`](docs/dev/cmake-gap.md#decision-2026-08-08-keep-both--makefile-canonical).
      Conditional on arming CI.
- [x] **Front-door restructure.** README opens with a four-step quickstart (binary → flash → card →
      first sound) above the catalogue; `docs/manual.md` is now the **platform** manual with a
      per-engine routing table, and granular's control reference moved into
      [`docs/engines/granular.md`](docs/engines/granular.md) where it belongs.
- [x] **`SynClock::_external_clock` was never initialized** — found by writing the granular audio test.
      Same defect as `Divider::_triplets_on`, in the same subsystem: declared, absent from the
      constructor's init list, masked on target by `.bss` zeroing. Off target it could come up believing
      it was externally slaved, in which case `Run()` only *arms* and the internal clock never emits —
      measured at **0 ticks in 3000 blocks**. That is why nothing clock-dependent had ever been testable
      off-target, and why `test_engine_params`'s `transport.tick(false)` calls were doing nothing.

**Still open from the review** (each needs a bench, not typing): the P2 session, which everything
hardware-gated funnels into — and arming CI's automatic triggers, above.

> **Update 2026-08-01 - the bench session is now instrumented.** This file was written as if P2 could > only be a manual listening pass. That predates the USB-C terminal channel, which was fixed and verified > on hardware 2026-07-31 (root cause: the panel jack is on OTG_HS, not OTG_FS - see > [`terminal-impl.md`](docs/dev/terminal-impl.md)). Two things landed since, both host-verified: > > - **`query cpu` / `cpumin` / `cpumax` + `reset cpu`** - the platform's `CpuLoadMeter` is now readable >   over the channel, so P2's headroom numbers are `reset cpu` -> drive the engine -> `query cpumax`, >   scripted per engine. Previously the meter needed `METER=1`, which brings up a second USB device on >   the same OTG core the terminal uses - the numbers and the channel that would collect them were >   mutually exclusive. > - **`live_params()`/`live_configs()` on every engine** - the stated blocker on `make test-hw`. The >   generic sweep no longer sets params engines ignore, so the mechanical half of P2 can run unattended. > > Neither is hardware-verified yet; both fold into the P2 session. > > **And one regression found on the way - now FIXED and hardware-verified.** `pstretch` had stopped > building entirely: not "cannot host the terminal" but no link at all, terminal or not, at either > window (`region SRAM overflowed by 80576 bytes` on the committed `v0.6.1` tree). Commit `993210f` > moved 114K from the data `SRAM` region into `SRAM_EXEC` (186K -> 300K) so the channel would fit > everywhere; pstretch's FFT working set needed 297664 B of exactly that region. > > Fixed with a per-engine linker script, `linker/alt_sram_pstretch.lds` (200K/312K instead of > 300K/212K), selected automatically on `ENGINE=pstretch` so a plain `make ENGINE=pstretch` is correct > too. Same approach and precedent as `linker/alt_qspi_chuck.lds`, and it leaves the other 20 engines on > the 300K split untouched. **All three pstretch images flashed and confirmed working on hardware > 2026-08-01** (8192 with and without the terminal, and 4096 with it) - so the moved code/data boundary > boots, which a link check could not have established. > > **P2 is therefore no longer purely pending - its first engine is measured.** `make test-hw` ran > against real hardware for the first time (`30 passed, 3 skipped`), and pstretch has CPU numbers: >
> | | WINDOW=8192 | WINDOW=4096 |
> |---|---|---|
> | CPU avg / max | 41.5% / **87.6 -> 91.3% (still climbing)** | 33.3% / **63.6% (converged)** |
> | `SRAM` (312K) | 97.40% | 82.01% |
> > **Open decision - pstretch's default window is now a voicing call with evidence attached.** 4096 is > the only config with real margin on both axes at once (~36% CPU, ~50 KB data, vs ~9% and ~8 KB), and > its peak has converged where 8192's had not - so 8192's true worst case is unknown and worse than > 91%. But 4096 is a shorter smear (~85 ms vs ~171 ms) and the long wash may be the point of the > engine. **Default stays 8192 until someone listens to both.** Numbers in > [`docs/engines/pstretch.md`](docs/engines/pstretch.md).

Priority is driven less by size than by what unblocks/gates what, and by whether an item is **build-verifiable on the host** vs. **hardware-gated** (needs a flash to verify). Most of the open work is now hardware-gated and has piled up: several engines have been *flashed and heard informally but not rigorously measured/voiced* - they sound alive, but CPU headroom (`Meter::cpu`) and the full voicing range haven't been pinned down. The dominant move is therefore a single bench session (P2) that does that measured pass; the remaining items are a deliberate code refactor (P3), an optional voicing tweak (P4), and a strategic build-system decision (P5). Ahead of all of them sits **P0**: the desk/host audit of whether every engine uses the full UI/indicator grammar is **done** (see `indicator-comparison.md` §7), leaving the ranked toolkit migration as the top actionable item (its apply step is hardware-gated and folds into P2).

**One exception to the hardware-gated pile-up, added 2026-08-01: P1.5, SD card onboarding.** It is the only substantial **host-verifiable** item open, so it can proceed today without a flash and without competing for the bench. It is also the only item here that is a *user-facing distribution gap* rather than engineering debt: the project ships firmware for ten card-reading engines but no base card, and the card's format rules fail silently. Sequenced after P1 only because P1 is a trivial fact-finding question, not because it is less important.

| # | Item | Effort | Risk | Verify | Gating |
|---|------|--------|------|--------|--------|
| P0 | Migrate engines onto the indicator toolkit (audit DONE; apply is the remaining work) | med | low-med | **hardware flash** | per-engine worklist ready; apply/confirm folds into P2 |
| P1 | Mono-input: answer the normalling question | trivial | n/a | a fact | unblocks/kills its own code item |
| P1.5 | ~~SD card onboarding: base card + one converter front-end + a `verify` diagnostic~~ **DONE 2026-08-01** (host); one hardware confirm folds into P2 | med-high | low | **host** (final card confirm folds into P2) | nothing - the only substantial host-verifiable item open |
| P2 | One bench session: measure + voice the engines flashed-but-not-quantified | low-med | med | **hardware flash** | turns "sounds fine" into measured CPU headroom + confirmed voicing range |
| P3 | Refactor delay engine onto shared primitives (by ear) | med | med-high | **hardware flash** | none (primitives in `dsp/`); folds into P2 |
| P4 | Tape wow/flutter: try quadratic curve + lower maxima | trivial | low | **flash** (by ear) | none (optional voicing); folds into P2 |
| P5 | Finish or back out the CMake adoption (now merged to `main`, incomplete) | high | high | flash + cleanup | strategic; three build-system files straddle `main` |
| P6 | Web front-end: browser SD card builder + WebSerial terminal (**built**; needs a real-browser pass) | done | low | **browser + flash** | code done, and the browser pass is now a scripted checklist ([`docs/dev/web-frontend-checks.md`](docs/dev/web-frontend-checks.md)) rather than an exploratory afternoon; open decision on shipping `TERMINAL=1` releases is unchanged |
| P7 | OSC codec for the terminal channel (`SPK_TERMINAL_OSC`) — **built + hardware-verified 2026-08-09** (63/63 cross-codec parity on `tape`) | done | low | — | gate resolved: `TERMINAL=1` ships. Remaining: `/sk/log` framing so `DEBUG=1` can coexist with SLIP; `param_label()` for other engines if wanted |

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

## P1.5 - SD card onboarding: there is no base card, and the format rules fail silently (IMPLEMENTED 2026-08-01)

> **Status: all three deliverables built and host-tested** (`scripts/sk_card.py` + `card_layout.py` + > `card_audio.py`, 60 tests in `scripts/test_sk_card.py`, `make sdcard` / `make check-sdcard`, docs at > [`docs/sd-card.md`](docs/sd-card.md)). Verified on the host: `init` produces a card its own `verify` > passes clean; `verify` catches all ten failure modes below on a deliberately broken card and exits > non-zero; `convert` round-trips mp3/wav through both cysox and ffmpeg with pitch and level intact; > `make sdcard` emits an 11.9 MB byte-reproducible zip into `dist/<version>/`. > > **Two things learned while building it, both now encoded in the tooling.** (1) `cysox` writing a > `.wav` produces 32-bit **integer** PCM — precisely the format that is the classic "looks like float, > plays as noise" trap on this device — so the backend decodes to headerless `.f32` and re-encodes > through our own writer instead. (2) libsox format support is a build-time property: this machine's > has no mp3 or flac handler, so a fixed cysox-first preference would fail on most real user input. > Backend choice is therefore per-file, probed with `cysox.sox.find_format`. > > **Remaining, hardware-gated (fold into P2):** flash each SD engine and confirm a generated card > actually mounts, scans and plays — the one claim the host cannot establish. Also unverified on > hardware: whether the synthesized demo content is *musically* useful per engine, as opposed to merely > well-formed.

**The gap (as originally written).** A working card exists on the developer's desk; a new user has no way to get one. There is no downloadable base card, no single command that builds one, and the conversion tooling is three separate Python scripts (`scripts/convert_tape_audio.py`, `convert_radio_audio.py`, `prepare_audiobooks.py`) that assume the reader already knows which script their engine needs. Ten engines read the card and **eight distinct layouts across four incompatible audio formats** are in play - none converted on-device.

**What the card actually has to look like** (path literals read out of the source, not the prose docs - this table does not exist anywhere else in one place):

| Engine | Path | Format | Source |
|---|---|---|---|
| granular | `SK/{B,G,P,R,T,Y}/{1..6}.WAV` | 48k **stereo**, f32 *or* int16, UPPERCASE names | `hw/card.cpp:61-66` |
| tape | `tapes/tape_{a,b}_{1..8}.wav` | 48k **mono f32** (`AudioFormat 3`) | `tape_engine.cpp:397` |
| shuttle | `shuttle/tape_{a,b}_{1..8}.wav` | 48k mono f32, **~30 s cap** (RAM) | `shuttle_engine.cpp:520` |
| radio | `radio/{0..15}/*.raw` | **headerless int16** mono 48k (+ optional `radio/rate.txt`) | `radio_engine.cpp:306` |
| bard | `bard/{0..15}/NAME.WAV` + `NAME.TXT` + `BOOKS.TXT` | **int16 mono 24k**, 8.3 names (+ `bard/BARD.CFG`) | `bard_engine.cpp:872` |
| pstretch | `pstretch/*.wav` | int16 mono, any rate (pitch-corrected on device) | `pstretch_engine.h:178,271` |
| csound | `csound/{0..7}.csd` | text (`examples/csound/` ships 7) | `csound_patch.h:34` |
| chuck | `chuck/{0..7}.ck` | text (`examples/chuck/` ships 8) | `chuck_patch.h:49` |

Plus `SK/config.txt` (see the manual) and `SK/MEM` (`memory/storage.h:22-23`).

**Why this is worse than a missing download: every rule fails silently or near-silently.** The firmware does no conversion - it reads file body bytes straight into frames - so a wrong-format file is not rejected, it is **reinterpreted as garbage**. A filename over 12 chars is simply **invisible** to the directory scan (`prepare_audiobooks.py` documents this for bard; it applies to every scanned bank). A leading `/` in a path silently resolves to the wrong volume (the bug that made the csound bank look empty - `csound-impl.md:163`). 32-bit *integer* PCM is the easy mistake for 32-bit *float* (`preparing-audio.md`). The device's only feedback is an LED: steady amber = empty slot, strobing amber = wrong format (tape), red vs magenta pad (pstretch). A newcomer with a correct-*looking* card gets noise or silence and no way to tell which of four rules they broke.

**Deliverable - three parts, in dependency order:**

1. **`sk-card verify <card-root>`: the diagnostic. Highest value per line of code, so build it first.** Walk an existing card and report every violation with the fix: wrong rate/depth/channel-count, name too long for the scan, wrong case, stray AppleDouble/`._*` files (a real past red herring - see `radio-impl.md:75`), files in a folder no engine reads. This converts the silent-garbage failure into a line of text, and it helps existing users with a card they already built, not just new ones. Pure host code, fully unit-testable against fixture trees (`scripts/test_*.py` is the precedent).

2. **`make sdcard` -> a base card zipped into `dist/<version>/`.** The full folder skeleton, `config.txt`, `BARD.CFG`, `radio/rate.txt`, the `examples/{chuck,csound}` patches copied into `chuck/`+`csound/`, and a short `README.TXT` **inside each folder** restating that folder's exact format - the rules arrive where the user is standing. Then **synthesized** demo audio (tones/sweeps/noise beds/rhythmic test patterns generated procedurally, in each engine's exact format) so every engine makes sound on a fresh card. Landing it in `dist/` means `make gh-release` ships it beside the binaries for free.

3. **One `sk-card` front-end over the three converters.** They already encode all the format knowledge and are good; what is missing is that a newcomer must choose among them and know the target layout. One command taking a card root plus a pile of audio, dispatching per engine and writing files to the right place in the right format. This is mostly a dispatcher - resist rewriting the converters.

**Dependency strategy - keep the decoder off the critical path.** Only step 3 needs to decode arbitrary user audio. Steps 1 and 2 must not:

- **`verify` needs no decoder.** It only inspects files already on the card, and those are WAV or headerless raw - both parseable with `struct` in ~20 lines. Keep it stdlib so the diagnostic always runs, including for a user whose problem *is* a broken toolchain.

- **`make sdcard` needs no decoder.** Demo content is synthesized (`struct` + `math`), and the target lands in `dist/`, where `build_release.py` is deliberately stdlib-only so plain `python3` suffices with no venv (Makefile:793). Adding a third-party import to the release path would forfeit that.

- **`convert` gets a backend registry**, extending the one `convert_tape_audio.py` already has (`CONVERTERS` + `available()` probe + `--tool`, `:71-122`). Add [`cysox`](https://github.com/shakfu/cysox) (in-process libsox via Cython) as the preferred backend and keep ffmpeg as fallback. Two reasons not to make it mandatory: it needs **system libsox** (`libsox-dev` / `brew install sox`) rather than bundling it, so it relocates the install barrier rather than removing it; and **mp3 depends on the libsox build** (`libsox-fmt-mp3` is separate on Debian) - which bites exactly where it hurts, since LibriVox ships mp3 and that is bard's primary source. Probe at runtime, prefer cysox, fall back to ffmpeg, and say which one ran.

  Where cysox is a clear win: structured metadata via `cysox.info()` instead of parsing `ffprobe` output or shelling out to `soxi`, real exceptions instead of `CalledProcessError` + stderr scraping, and `cysox.stream()` for chunked reads (the headerless `.raw` radio writer and `prepare_audiobooks.py`'s silence detection both want that). `cysox` is already in the `dev` group in `pyproject.toml`.

**Explicitly out of scope: bundling real recordings.** LibriVox (public domain) is the natural bard source and the Music Thing RadioMusic library the natural radio source, but both bloat the download and drag in provenance tracking. Ship them as a documented `fetch` step, not as release payload. Synthesized demo content has the same "it works on first boot" effect with zero rights questions.

**Verification.** Parts 1-3 are host-verifiable and unit-testable - which is why this is the top actionable item while everything else queues behind the bench. The one hardware-gated step is confirming a generated card actually boots and plays on each SD engine; fold that into P2.

## P2 - One bench session: measure + voice the engines that work but aren't quantified (HARDWARE-GATED)

These engines **have been flashed and heard** - they boot and sound alive on the unit. What's missing is the *measured* pass: real CPU headroom (`Meter::cpu`) and a deliberate sweep of the full voicing range, neither of which a host test or a casual listen establishes. Do it as a single bench session and capture the numbers:

> **The measured pass is now scripted, and pstretch is done (2026-08-01).** Read CPU over the terminal > instead of `METER=1` (which cannot coexist with the channel - it wants the same OTG core): > > ``` > make ENGINE=<e> TERMINAL=1 && make ENGINE=<e> TERMINAL=1 program-dfu   # device in DFU > reset cpu  ->  drive the engine  ->  query cpu / cpumin / cpumax > make test-hw                                                          # 30-case sweep, needs dialout > ``` > > **Sample the peak more than once.** `max` is "worst block since `reset cpu`", so re-reading it shows > whether it has converged. pstretch at 8192 was still climbing after six seconds (87.6 -> 91.3%) while > 4096 settled within 0.3 pp - a difference invisible in a single reading, and the thing that actually > decides whether an engine has headroom. Also read `min`: it is the platform floor (~2%) with the > engine's DSP idle, which separates "expensive" from "spiky". > > Done: **pstretch** (41.5% avg / 91.3%+ max at 8192; 33.3% / 63.6% at 4096 - see the header note).

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

## P5 - Decide the CMake adoption — **RESOLVED 2026-08-08: keep both, Makefile canonical**

> **Decision and its reasoning: [`docs/dev/cmake-gap.md`](docs/dev/cmake-gap.md#decision-2026-08-08-keep-both--makefile-canonical).**
>
> This item framed the choice as binary — finish adoption or back it out — because three build files
> with a hand-duplicated engine list drift silently, and had (seven divergences, two of them hard build
> failures). CI now builds six flag-sensitive engines through CMake alongside the full Makefile matrix,
> so the drift is caught mechanically; the liability was never having two build systems, it was having
> no forcing function.
>
> **Conditional:** both workflows are `workflow_dispatch`-only for now, so nothing runs unasked. Arming
> the push trigger is what makes this decision real — until then it rests on someone pressing a button.
>
> **Explicitly NOT adopted:** item 4 below, the compiler-enforced platform/engine boundary, which was
> the original headline justification. Without it CMake rides along for its per-engine cached build dirs
> and its correct flag tracking, not for the boundary. Items 1-3 are moot while both stay. Item 5 (a
> hardware flash of a CMake image) is unchanged and still belongs to the P2 bench session.
>
> The original analysis is kept below, unedited, because it is what the decision was made against.

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

## P6 - Web front-end: browser-based SD card builder + terminal (BUILT 2026-08-01, host-verified only)

**Built.** Both phases landed in [`web/`](web) — a static page, no dependencies, no build step (`make web-serve`, `make test-web`, `make web-data`). Design and the outcome against it are in [`docs/dev/web-frontend.md`](docs/dev/web-frontend.md); the app's own notes are in [`web/README.md`](web/README.md). In-browser DFU flashing stayed out of scope (the Daisy Web Programmer already covers it, and a half-written image is the worst failure in the system).

All three constraints were resolved as the doc proposed:

1. **Chromium only** — handled by designing around the read-only path. Verify, Build and Convert all work in Safari and Firefox via drag-in-files → download-a-zip; only in-place card editing and the terminal need Chromium, and each says so where it is missing rather than failing silently.

2. **The terminal does not exist on released firmware** — **still an open firmware decision, unchanged by this work.** Phase 2 is built and tested against a scripted fake device, so writing it cost nothing and gated nothing, but it remains close to useless to anyone who does not build their own image. The Terminal tab leads with that fact. Deciding whether to ship `TERMINAL=1` releases (~19-25 KB `SRAM_EXEC` everywhere, and USB MIDI on the QSPI engines only) is the item that is actually left.

3. **One source of truth** — done, and taken further than the doc asked. `card_layout.py --json` now exports the table *and every piece of generated text* (the per-folder READMEs, the root README, the default config), so the browser builds a card byte-identical to `sk_card.py init` — asserted per file by SHA-256 — while declaring none of it. Convert's target-naming became a per-bank template (`Bank.target`) consumed by both front-ends, replacing six per-engine branches in `sk_card.py`.

Drift is guarded from both sides: `make test-scripts` regenerates the export and fails if the committed copy has moved (`scripts/test_web_export.py`), and `make test-web` fails if the JS disagrees with the Python — the WAV writers by byte equality against `card_audio.py` fixtures, and the checker by reaching the same verdicts *and the same fix text* as `verify_card` on a deliberately-broken card.

**What is NOT verified, and is what remains:** no real browser has loaded the page, no real card has been read through the File System Access API, no mp3 has been converted and heard on the device, and no `TERMINAL=1` build has been driven over WebSerial. The full list is under "Remaining verification" in the design doc. Items 3 and 4 there fold naturally into the P2 bench session.

**That pass is now scripted**, in [`docs/dev/web-frontend-checks.md`](docs/dev/web-frontend-checks.md): nine checks with exact steps, the expected output, and the CLI command to diff each result against, so it is about thirty minutes rather than exploratory poking. Writing it against the source turned up two things worth targeting rather than discovering: `fromDataTransfer` reads `dt.items` *after* an `await`, by which time the drag data store may be invalidated (C4 aims at it — a dropped loose file is the likely casualty), and the service worker registers only when `location.protocol === 'https:'`, so the offline check cannot be run against `make web-serve` and needs a real deploy or a local HTTPS cert.

**Rewritten in TypeScript** (`web/src/`, bundled by bun to a committed `web/dist/app.js`), and re-layered: `core/` holds the rules and touches no browser API, `platform/` holds the four browser-only APIs, `app/` holds one view-model per tab with all the state and none of the DOM, and `ui/` renders. Every browser capability enters through an interface in `core/ports.ts`, so the view-models are tested against fakes with no DOM and no device - including the two behaviours that previously needed hardware (an empty port chooser, a device unplugged mid-session). Tests enforce the layering rather than asserting it, and one fails when the committed bundle is older than `src/`. 206 tests, up from 113 at the original landing.

**Two additions since the build**, both host-verifiable and so done rather than deferred:

- **A Reference tab**, the web counterpart of `sk_card.py layout` — the one subcommand that had no screen. It states what every engine expects on the card, the constraints that fail silently, the sidecar defaults (`radio/rate.txt` sets the playback rate for a whole bank and appeared nowhere in the UI), and the `SK/config.txt` properties with their ranges. It asks nothing of the browser, which makes it the one tab that behaves identically in Safari, and a test asserts it writes down no figure the layout owns.

- **A drift guard on the service-worker asset list**, which was hand-maintained with nothing checking it. The failure it invited was silent and one-sided — a forgotten entry breaks only offline, only for users who already installed the worker. Now checked against a filesystem walk in both directions, plus an import-graph walk from the entry point, with all three confirmed to fail on the drift they describe.

## P7 - OSC codec for the terminal channel (`SPK_TERMINAL_OSC`) - BUILT 2026-08-09

**Status: built, off-target green, and VERIFIED ON HARDWARE 2026-08-09.** `make ENGINE=<e> TERMINAL=1 OSC=1`.
The gating question below - whether `TERMINAL=1` ships at all - was answered yes on 2026-08-09, which is
what unblocked this. What shipped, against what the spec below predicted:

- **Firmware.** `src/terminal/{slip.h,osc.h,osc_decode.cpp,osc_sink.h,osc_encode.cpp,osc_addr.{h,cpp}}`.
  Layer [2] only; layers [1] and [3] are untouched, and an address resolves to a line in the existing
  grammar which goes through the existing `dispatch_line()`, so there is no second verb table.
- **`IEngine::param_label()`** landed as specified - one virtual, defaulting to nullptr, cosmetic to the
  device. **`radio` and `tape` implement it** (6 and 10 labels): radio's PITCH advertises as *station*,
  tape's `Size` as *character* and its two grit slots as the low-pass they actually drive. Every other
  engine keeps the default, producing a semantic tier identical to the generic one minus the `param/`
  segment - the documented degraded-not-broken path. `host/test_osc_labels.cpp` checks both tables
  against the REAL engines, including that no two live slots on one engine share a label.
- **The logger/SLIP conflict** took shortcut (a): `OSC=1` with `DEBUG=1` is a build error. The right
  answer, wrapping log output as `/sk/log` frames, is still open and is what would make `DEBUG=1` usable
  on an OSC build.
- **Host side.** `tools/skdev/{osc,semantic,oscdevice}.py` - wire format, the generated semantic tier,
  and an `OscDevice` with the same method surface as `Device`. The first two are dependency-free, so
  `tools/test_osc_codec.py` (24 checks) runs in CI with no pyserial and no hardware, against a describe
  bundle emitted by the real firmware code path.
- **Tests.** `host/test_terminal_osc.cpp` (`make -C host test-terminal-osc`), wired into `make -C host
  test`. Covers SLIP, the wire format, coercion, every address family against the exact `IEngine` call,
  reply typing, ack mode, the error taxonomy, inbound bundles, and `describe`.
- **Cost, measured:** ~9.0 KB `SRAM_EXEC` + ~12.4 KB SRAM over the line build - roughly 4x the spec's
  estimate, mostly the 6 KB descriptor-bundle scratch the estimate omitted entirely. `delay` goes 68.9%
  -> 72.3% of `SRAM_EXEC`. See the footprint table in the spec.

**What is left:**

- [x] **A hardware pass — DONE 2026-08-09, passed.** Cross-codec parity on a cased Spotykach running
      `tape`: **63/63 identical** against both codecs (`make test-hw` vs `make test-hw CODEC=osc`).
      Because the sweep's cases are generated from `describe`, an identical result list also proves both
      codecs advertise the same param/config/query sets. The ~4 KB descriptor bundle arrives whole;
      steady-state round trip is 0.18 ms. Five defects were found and fixed in the process (one
      firmware: OSC describe dropped Enum labels; four host-client) — see the spec's bench section.
- [ ] **`/sk/log` framing**, to lift the `DEBUG=1` restriction.
- [x] **`param_label()` for the remaining engines — DONE 2026-08-09.** 16 engines now carry tables
      (115 labels): radio, tape, graincloud, delay, qdelay, edrums, reso, mosc, reverb, shuttle,
      softcut, bard, glitch, pstretch, csound, chuck. Three deliberate abstentions, each for a reason
      worth keeping:
      - **granular** — the shared `ParamId` vocabulary IS granular's own words (the enum "mirrors the
        granular engine's MValue-backed set"), so layer 2 and layer 3 coincide and a table would just
        be a second copy of `kParamNames` that could drift from it.
      - **csound / chuck** — only `Aux` ("patch") is labelled. Every other slot is a generic
        pass-through to the loaded `.orc`/`.ck`, whose meaning the PATCH defines; a fixed label would
        be a confident lie that changes with every patch.
      - **chorus / filter / gigaverb / voice** — these never narrowed `live_params()`, so `describe`
        lists the whole `ParamId` enum. Labelling there would name slots the engine ignores. They need
        liveness masks first; that is the real prerequisite.
      `host/test_osc_labels.cpp` links 11 of them and enforces the invariants on all 11.

- [x] **`live_params()` for chorus, filter, gigaverb, voice — ALREADY PRESENT; the earlier claim here
      that they were missing was wrong.** All four inherit a DERIVED mask from their shared wrapper:
      `FaustEngine`/`FaustChainEngine` compute it from the bind table the manifest generates
      (`faust_fx.h`, `faust_chain.h`), and `GenEngine` from the wrapper's `index_of` switch
      (`gen_engine.h`). Verified by instantiating each: chorus `masked=1` with 3 advertised params
      (its 4th bind is ModSpeed, platform-owned and filtered out), filter and voice `masked=1` with 4.
      Deriving is strictly better than the hand-written masks the other engines carry — these headers
      are GENERATED, so a hand-listed mask would be a second copy of the bind table, free to drift on
      the next `make faust-engine`.

      The mistake came from grepping the per-engine headers for `live_params`, which is inherited and
      so does not appear there. Worth remembering: on these four engines, look at the wrapper.

- [x] **`param_label()` for the generated engines — DONE 2026-08-09, by derivation.** Same argument as
      the mask: the Faust bind table already carries the slider name, and that name IS the layer-3
      word (`filter` binds "cutoff" to `ParamId::Speed`, "drive" to `Size`). `param_label()` on both
      Faust wrappers now reads it, so chorus/filter/voice — and every future generated Faust engine —
      get labels with no per-engine code and no drift. Measured: chorus `size`→*delay*,
      `modamp`→*depth*; filter `speed`→*cutoff*, `pos`→*reso*, `size`→*drive*; voice `speed`→*freq*,
      `size`→*shape*, `mix`→*level*. A chain engine can bind one role in both stages (voice puts Speed
      on the oscillator's "freq" AND the filter's "cutoff"); stage A wins, since it is the sound source
      and the stage the manifest lists first.

- [ ] **`param_label()` for gigaverb** needs a generator change, not a code change. `GenEngine` has no
      slider names to derive from — `index_of` returns an index — and `gigaverb_engine.h` is GENERATED
      by `scripts/gen_engine.py` from `gigaverb.json`, so hand-adding a table there would be
      overwritten. The names already exist as comments the generator emits (`bandwidth`, `damping`,
      `dry`, `revtime`, `roomsize`, `tail`, `spread`, `early`); the fix is to have it emit a `name_of`
      alongside `index_of`, and a `param_label()` on `GenEngine` that reads it. Small, but it belongs
      in the generator.

---

### The original design note (2026-08-07), kept for its reasoning

The terminal channel was designed codec-agnostic from the start: [`terminal-control.md`](docs/dev/terminal-control.md) puts line-ASCII and OSC at layer [2] behind one dispatcher, so an alternate codec is a compile flag rather than a rewrite. The flag name has been reserved and listed as *"(later, unbuilt)"* since; what did not exist until now was an address space. [`docs/dev/terminal-osc.md`](docs/dev/terminal-osc.md) specifies one, along with SLIP framing, type coercion, the reply grammar, and the footprint estimate.

**What it is for, precisely.** Not testing - the pytest harness is already well served by lines, and text is trivial to assert on. The one thing OSC buys is that the device becomes a node in a Max/Pd/TouchOSC rig, where a fader binds to an address once and then sends floats. Every decision in the spec is made for that client; if that rig never materializes, this item should be closed rather than built.

**The design question that took the longest, because it is the one that would have been expensive to get wrong.** The panel is fixed hardware and every engine reinterprets it, which gives three distinct namings: the silkscreen (PITCH/SIZE/ENV, constant), the `ParamId` slot (`speed`/`size`/`env`, constant, and the only one machine-readable - `names.cpp`), and the engine's actual meaning (`radio`: PITCH is *station select*; `tape`: `ParamId::Size` is *character*, `tape_engine.cpp:128`). The enum header says where the middle layer's vocabulary came from: it "mirrors the granular engine's MValue-backed set", inherited by every other engine as generic slots.

The address is the **slot** - `/sk/a/param/speed`, all lowercase - and the engine's meaning travels as a **cosmetic label in `describe`**, never in the path. That split is what lets *one control-surface layout drive every build*: the wire never changes, and a layout generated from `describe` prints "station" on that fader against radio and "character" on the SIZE fader against tape. Putting the meaning in the path would have forced a per-engine layout and, with it, an engine-name segment - which the spec records as rejected, along with value-in-address, verb-first paths, numeric slots, and silkscreen naming (layer 1 -> layer 2 is one-to-many: PITCH also reaches `Aux` on the Alt layer).

**Two tiers, and only one of them is firmware.** The device speaks a generic, engine-independent address space; a host-side translator generated from `describe` offers the engine-specific, human-readable namespace on top (`/radio/a/station` -> `/sk/a/param/speed`). That keeps the per-engine vocabulary - the part that is hand-maintained and can rot - out of the wire format, where drift would be a protocol bug rather than a display bug. It also gives the engine-name segment the one place it is load-bearing: the semantic namespace *is* engine-specific by definition, which is exactly why it must not be in the generic one. Host cost only; firmware needs nothing beyond `param_label()`.

Layer-1 (silkscreen) addressing was considered for the generic tier - the panel is the true cross-engine invariant - and rejected on evidence: `core.ui.cpp:466-524` routes a knob through a `DeckLayout` branch *and* `MValue` soft-takeover pickup, which deliberately swallows a write that does not match the stored knob position. Correct for a pot, catastrophic for a control message; and `alt+size` resolves to `Win` or `PolySlice` depending on mode, so one address would hit different params by config. Reaching it would mean duplicating the UI branch in the terminal (crossing the `check-boundary` line) or pushing messages through the pot-apply pass `mode test` exists to disable.

**What it needs that does not exist yet:**

- **`IEngine::param_label()`** - a third engine-owned virtual beside `live_params()`/`live_configs()`, defaulting to the `kParamNames` entry so no engine is forced to care. ~6-12 short strings for the engines that want them. Deliberately cosmetic: no address, reply or error derives from it, because it is a hand-maintained table that can rot.
- **A resolution to the logger/SLIP conflict.** This is the sharpest constraint OSC adds and it has no analogue in the line build: `[tag]` log lines interleaving on the shared CDC are harmless for line-ASCII and *fatal* for SLIP, where a log line lands inside a packet. Either force `INFS_LOG=0` (the acceptable shortcut) or wrap log output as `/sk/log` frames (the right answer).
- **A host-side translator** in `tools/`, as a component of `skdev` rather than a separate program - `OscDevice` already reads `describe` to build its param map, so the translator is that map plus slugify/compose rules and a reverse index. Testable with no device against the descriptor fixtures that already exist (`web/test/model.test.ts`, ported from `tools/skdev/descriptor.py`): semantic -> generic -> semantic must be the identity.

- **A bigger TX FIFO.** `describe` becomes one OSC bundle carrying full addresses plus labels, ~2-3 KB, and a bundle cannot be streamed the way lines can - so 2 KB -> 4 KB. This turns the dispatch doc's FIFO-sizing *recommendation* into a requirement.

**Cost:** ~3.7 KB flash, ~2.5 KB SRAM over the line build, estimated and unmeasured. Affordable on the engines that can already host `TERMINAL=1`; the ones at the `SRAM_EXEC` edge simply do not get OSC.

**Gating, honestly stated.** This sits behind the same unresolved decision as P6's item 2 - whether `TERMINAL=1` ships in releases at all. An OSC codec on firmware nobody but the author builds is a rig integration for a rig that cannot exist. It is also explicitly last in the terminal channel's own phasing, after `SPK_TERMINAL_MEASURE` (phase 2) and `SPK_TERMINAL_STIM` (phase 3), both still unbuilt.

**Verification is unusually cheap when it does happen**, because layer [3] is shared: `skdev` grows an `OscDevice` with the same method surface, and `test_generic.py`'s cross-engine sweep runs unmodified against either codec. The acceptance criterion is parity - the same sweep, both codecs, identical results - so anything the OSC build answers differently is a codec bug by definition. Two checks are new: that every address `describe` advertises composes exactly as the spec predicts (catching drift between `osc_addr.cpp` and the descriptor, the two places that both know how an address is spelled), and that the same address set appears on every build with only the labels differing - the property the universal-layout claim rests on, which should be asserted rather than assumed.
