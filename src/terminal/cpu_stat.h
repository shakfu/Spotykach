// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// CPU load readout for the terminal channel. See docs/dev/terminal-impl.md.
//
// Why this exists: the platform already owns a whole-callback `daisy::CpuLoadMeter` (`src/meter.h`,
// driven from `AppImpl::ProcessAudio`), but reading it used to mean building with `METER=1`, which
// brings up a SECOND USB device (`_meter_usb`, FS_EXTERNAL) purely to print the numbers. That device
// claims the same OTG core the terminal needs, so `METER=1 TERMINAL=1` is not a build that can work.
//
// The channel makes the second device unnecessary: the meter is cheap (two `System::GetTick()` reads
// per block), so a TERMINAL build drives it and reports on request over the channel that already
// exists. `app.cpp` therefore gates the Init/OnBlockStart/OnBlockEnd calls on `METER || SPK_TERMINAL`
// while keeping the USB-printing block under `METER` alone.
//
// This header exists rather than dispatch.cpp reaching for `meter.h` directly because `meter.h`
// includes `daisy_seed.h`, which does not exist on the host. Same split as `usb_diag.h`: an ARM
// implementation that reads the peripheral, an inert host one so `host/test_terminal.cpp` can link.

namespace spotykach {

// A CPU load sample, as PERCENT of the audio block budget (0..100). Percent rather than the meter's
// native 0..1 because that is the unit every consumer already speaks - the `METER=1` logger prints
// `load%`, and TODO.md's headroom targets are written as percentages.
//
// `min`/`max` are extremes since the last `cpu_stat_reset()`, NOT a rolling window. Without a reset
// they run since boot, so `max` is dominated by the boot transient - reset before a measurement or the
// peak reading means nothing. `avg` is the meter's own smoothed average and is unaffected by resets
// beyond being re-seeded.
//
// All three read `nan` in one narrow window: `CpuLoadMeter::Reset()` sets them to NAN and re-seeds on
// the next `OnBlockEnd()`, so a read landing between a reset and the next audio block has genuinely
// measured nothing. That gap is one block (~1 ms at 48 kHz / 48 samples) against a USB round-trip, so
// it is hard to hit in practice; it is left as `nan` rather than coerced to 0, because "no sample yet"
// and "zero load" are different answers and only one of them is true.
struct CpuStat {
    float avg = 0.f;
    float min = 0.f;
    float max = 0.f;
};

// Sample the platform meter. Zeroes on a host build (no audio callback, nothing to measure).
void cpu_stat_read(CpuStat& s);

// Clear the min/max extremes so the next reading measures a chosen interval rather than all of boot.
// Exposed as `reset cpu`; the intended sequence is `reset cpu` -> drive the engine -> `query cpumax`.
void cpu_stat_reset();

}  // namespace spotykach
