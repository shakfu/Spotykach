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
    bool _hold;
    bool _is_running;
    bool _is_about_to_run;
    bool _last_state;
    bool _external_clock;
};

};
