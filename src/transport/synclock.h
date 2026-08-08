#pragma once
#include <cstdint>   // uint*_t (transitive-include hygiene; host build)

#include <cmath>
#include <array>
#include <functional>

#include "nocopy.h"

inline static bool fcomp(const float lhs, const float rhs, const int precision = 2) {
    auto digits = precision * 10;
    auto lhs_int = static_cast<int32_t>(roundf(lhs * digits));
    auto rhs_int = static_cast<int32_t>(roundf(rhs * digits));
    return lhs_int == rhs_int;
}

namespace spotykach {

class SynClock {
public:
    SynClock();
    ~SynClock() {}

    void Init(const uint32_t update_interval_mks, const uint32_t ppqn_out);

    uint32_t PPQNIn() const { return _ppqn_in; }
    void SetPPQNIn(const uint32_t);

    /*
    Called by internal interrupt timer
    */
    void Tick(const bool external) {
      if (_external_clock && external) _external_clock_tick();
      else if (_is_running) _emit_ticks(); 
    }

    void SetOnTick(std::function<void(const bool)> on_tick) { _on_tick = on_tick; }
    
    float Tempo() const { return 60000000.f / _tempo_mks; }

    /*
    Setting tempo from internal control. 
    Has no effect in case of syncing to extrnal clock.
    */
    void SetTempo(const float norm_value);

    /*
    In case of external clock sync this
    method only schedules start. Actual ticking
    starts on the first tick of the external clock.
    see clock_in_tick() below.
    */
    void Run() {
      Reset();
      if (_external_clock) _is_about_to_run = true; else _is_running = true;
    }

    void Stop() {
      _is_running = false;
    }

    void Reset();

    bool IsRunning() const { return _is_running; }

    void SetExternalClock(const bool on) { _external_clock = on; }
    bool ExternalClock() const { return _external_clock; }

private:
    NOCOPY(SynClock)
    /*
    External clock received
    This method starts playback on first received clock
    after playback was scheduled. After that, calls sync 
    for every tick received.
    */
    void _external_clock_tick();

    /*
    See cpp file for details
    */
    void _emit_ticks(const bool on_external_tick = false, const bool kick_off = false);
    
    uint32_t _get_tempo_mks(const float tempo) { return static_cast<uint32_t>(60.f * 1e6 / tempo); }

    std::function<void(const bool)> _on_tick;

    static constexpr float kExtClockOffset = .05f;
    static constexpr int32_t kOverflowThresh = 1073741823; //INT32_MAX / 2;

    float _manual_tempo;

    int32_t _ppqn_in;
    int32_t _ppqn_out;
    int32_t _tr_time;
    int32_t _ticks_per_clock;
    int32_t _ticks;
    int32_t _fticks;
    int32_t _ticks_at_last_clock;
    int32_t _tempo_ticks;
    int32_t _tempo_mks;
    // Default member initializers, not just constructor ones. `_external_clock` was declared here and
    // omitted from the constructor's initializer list, so it was read indeterminate - the same defect,
    // in the same subsystem, as Divider::_triplets_on. On target it was masked by .bss zeroing (the
    // only SynClock lives inside the file-static AppImpl), but any automatic-storage Transport - i.e.
    // every host test - could come up believing it was slaved to an external clock, in which case
    // Run() merely ARMS (`_is_about_to_run`) and Tick(false) never emits. The internal clock then
    // never runs: measured, 0 ticks in 3000 blocks. That is why nothing clock-dependent (the granular
    // Slice mode, the step sequencer, key/bar boundaries) could be tested off-target, and why the
    // `transport.tick(false)` calls in test_engine_params were doing nothing.
    bool _hold             = false;
    bool _is_running       = false;
    bool _is_about_to_run  = false;
    bool _last_state       = false;
    bool _external_clock   = false;   // internal clock unless a source switch says otherwise
};

};
