# softcut feasibility spike

Status/bring-up notes for `ENGINE=softcut` - a **feasibility scaffold**, not a finished instrument. Its job is to put a hardware CPU number under the question "can a monome [softcut-lib](https://github.com/monome/softcut-lib) looper run as an sk-engine, and at how many voices?" Everything below the audio measurement (knob map, pads, SD load, routing) is deliberately absent - clone the `shuttle` engine for that once the CPU budget is known.

## What it is

A multi-voice crossfaded tape looper. Each `softcut::Voice` is a resampling read/write head with subsample-accurate crossfade loops, interpolated overdub, and pre/post SVF filters. The scaffold runs each active voice in softcut's **worst-case** path: a non-integer rate (engages the resampling head), overdub recording (play+rec -> the heaviest `processSample`: read+write+crossfade), and both SVFs on.

The **Mode switch** picks how many voices are live, so one flash measures all three counts:

| Mode (Reel/Slice/Drift switch) | Voices |
|--------------------------------|--------|
| 0 (Slice)                      | 2      |
| 1 (Reel)                       | 4      |
| 2 (Drift)                      | 6      |

Inactive voices clear play+rec, so softcut takes its cheap output-zeroing path and cost ~0.

Live knobs (only to make it audible while metering): PITCH = rate spread, SIZE = loop length.

## Build / flash / read the meter

```
make ENGINE=softcut METER=1          # load% streams over USB serial; panel ring A also shows it
make program-dfu                     # flash (hold BOOT+tap RESET to enter DFU first)
```

With `METER=1`, `app.cpp` streams `load% max/avg/min` over USB CDC every 250 ms (keep the serial port open). On the panel: **ring A = CPU load** (faint base = full 2 ms block budget, bright arc = avg, dot = peak, green/amber/red = peak severity, red >= 85% is at/over deadline), **ring B = active voice count**. Flip the Mode switch through Slice/Reel/Drift and record avg+peak at 2/4/6 voices.

The meter wraps the whole audio callback (analog read + UI tick + CV + engine), exactly like the reverb METER numbers in [`reverb-impl.md`](reverb-impl.md), so the figures are directly comparable.

## Measured device load (METER build, worst block)

Worst-case config (every voice overdub-recording at a non-integer rate with both SVFs on), 48 kHz / 96-sample block, 2 ms budget:

| Voices (Mode) | avg load | peak load | verdict |
|---------------|----------|-----------|---------|
| 2 (Slice)     | ~32%     | ~49%      | comfortable |
| 4 (Reel)      | ~62%     | ~79%      | shipping-safe |
| 6 (Drift)     | ~92%     | ~108%     | over the 2 ms deadline -> dropouts |

Clean linear scaling, ~15.3% avg / voice. Two takeaways:

- **The host bench under-predicted by ~2-5x** (it estimated 3-8% / voice; hardware is ~15%). softcut is SDRAM-buffer-bound like the reverb, so host wall-clock cannot model it - this is why the on-device METER pass was load-bearing, not a formality. For reference, the 4-voice 62%/79% is ~1.5x the reverb DoubleMono cap (41%/61%) that already ships.

- **4 voices is the safe target as-is; 6 is recoverable**, via either the `std::function` removal below (typically +15-30% headroom) or simply because real playback voices (no record / integer-ish rate / one filter) are much cheaper than this all-voices-overdubbing worst case.

NB a deck-gating bug was found and fixed during bring-up: the platform pushes `set_config(Mode)` for BOTH decks every loop (deck A then B), so an engine that ignores the deck arg lets deck B clobber deck A's selection. `set_config` now honors `DeckRef::A` only.

## Measured footprint (build, on-target)

- **SRAM_EXEC ~87.7%** of 186 KB at the default `-O2` - fits with headroom, **no QSPI, no `-Os`**. The softcut DSP itself is only ~11 KB; the rest is the platform floor.

- **`.bss` ~35%** of SRAM: voice STATE lives in SRAM (engine members, ~9.3 KB/voice after the port fixes below); only the audio BUFFERS are in the SDRAM arena (softcut's `setVoiceBuffer` split).

- **SDRAM:** one power-of-2 buffer per voice. 2^18 frames = 5.46 s = 1 MB mono/voice (6 MB for 6).

## Port fixes applied to the vendored core (`vendor/`, all marked "sk-engines PORT NOTE")

The vendored library is monome softcut-lib with four embedded-targeted edits. Re-apply these if you re-vendor from upstream:

1. **`TestBuffers.h` -> empty stub.** Upstream holds `float buf[6][131072]` (= **3 MB**) per ReadWriteHead for a Matlab `.m` debug dump - 18 MB for 6 voices, plus an `<fstream>` dependency, for diagnostics that never run on device. The only live call is `init()`; stubbed to no-ops.

2. **`Voice.h`: `std::atomic<phase_t>` (double) -> `std::atomic<float>`.** A 64-bit atomic on the M7 is not lock-free and emits `__atomic_store_8`/`__atomic_load_8` libcalls newlib (nano.specs) does not provide -> link error. 32-bit is naturally lock-free (no libcall) and still tear-free for the ISR-writes / main-loop-reads split these fields exist for. Three store sites in `Voice.cpp` gained explicit `static_cast<float>`.

3. **`Resampler.h`: dropped unused `#include <iostream>`** and **`ReadWriteHead.cpp`: dropped a `std::cerr` log** on a never-hit path. Together these pulled the whole iostream/locale machinery (~150 KB of `.text`) into the image; removing them dropped `.text` from ~317 KB (overflow) to ~167 KB (87.7%). **This is what makes it a normal SRAM build.**

4. **Makefile**: `-DM_PI -DM_PI_2` (strict `-std=c++17` does not expose them from `<cmath>` on arm-none-eabi - same idiom as reso/mosc).

## Known headroom (not yet applied)

`Voice::processBlockMono` dispatches the hot per-sample path through a `std::function` rebuilt every block (`vendor/src/Voice.cpp`) - an indirect call per sample that blocks inlining of `processSample`. Replacing it with a templated/switch dispatch is low-risk and typically recovers 15-30% on the M7. If the metered load at the target voice count is close to budget, this is the first lever.

## Next step (turning the scaffold into an engine)

Once the voice budget is known: clone `shuttle` (the closest existing engine - in-RAM varispeed tape, POS/SIZE loop windows, Alt+Play record, SD slot load), swap its hand-rolled playback for `softcut::Voice`s, and design the knob/pad map around softcut's controls (loop points, fade time, rate slew, the two filters, voice sync, phase quant). The host bench `scratchpad`/`bench_softcut.cpp` from the spike tracks relative per-voice cost if you want a host-side regression.
