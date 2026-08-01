// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#include "terminal/cpu_stat.h"

#if SPK_TERMINAL

#if defined(__arm__)
#include "meter.h"    // Meter::cpu().load - the platform's whole-callback CpuLoadMeter
#endif

#pragma GCC optimize("Os")

namespace spotykach {

#if !defined(__arm__)

// Host build (host/test_terminal.cpp): there is no audio callback and no tick counter, so there is
// nothing to measure. Report zeros - the dispatcher links against these because `query cpu` is in the
// platform table, and a host test asserting "cpu reads 0" is exactly right.
void cpu_stat_read(CpuStat& s) { s = CpuStat{}; }
void cpu_stat_reset() {}

#else

void cpu_stat_read(CpuStat& s) {
    const auto& m = Meter::cpu().load;
    s.avg = m.GetAvgCpuLoad() * 100.f;
    s.min = m.GetMinCpuLoad() * 100.f;
    s.max = m.GetMaxCpuLoad() * 100.f;
}

void cpu_stat_reset() { Meter::cpu().load.Reset(); }

#endif  // __arm__

}  // namespace spotykach

#endif  // SPK_TERMINAL
