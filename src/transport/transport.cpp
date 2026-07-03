// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#include "transport.h"

#include <cmath>
#include <array>
#include <functional>

#include "config.h"

using namespace spotykach;

// Key-interval lookup (was driver.h): norm 0..1 -> musical bar length in 1/16 units. File-local; only
// set_key_tick_interval_norm uses it. The kKeyInterval enum itself is contract-owned (engine/mode.h).
static const std::array<kKeyInterval, 17> kKeyIntervals = {
    kKeyInterval::k1_16,
    kKeyInterval::k1_4,
    kKeyInterval::k2_4,
    kKeyInterval::k3_4,
    kKeyInterval::k4_4,
    kKeyInterval::k5_4,
    kKeyInterval::k6_4,
    kKeyInterval::k7_4,
    kKeyInterval::k8_4,
    kKeyInterval::k9_4,
    kKeyInterval::k10_4,
    kKeyInterval::k11_4,
    kKeyInterval::k12_4,
    kKeyInterval::k13_4,
    kKeyInterval::k14_4,
    kKeyInterval::k15_4,
    kKeyInterval::k16_4
};

Transport::Transport():
_divider            { Divider(kPPQNIntern) },
_tick_index         { 0 },
_source             { Source::internal },
_key_tick_count     { 0 },
_key_tick_interval  { 4 },
_tap                { false }
{};

void Transport::init(const float sample_rate, const float buffer_size, const ITimeSource* time)
{
    _time = time;
    _tempo.set_time_source(time);
    _reset_us = _time ? _time->now_us() : 0;

    _clock.Init(2000, kPPQNIntern); //2000 mks => 500Hz (see app.cpp)
    _clock.SetPPQNIn(Source::ts4);

    using namespace std::placeholders;

    auto on_clock = std::bind(&Transport::_on_clock_tick, this, _1);
    _clock.SetTempo(_tempo.bpm());
    _clock.SetOnTick(on_clock);
    _clock.Run();
};

void Transport::toggle_source()
{
    _clock.Stop();
    _divider_reset_counts(true);
    static const auto src_cnt = 3;
    static Source src[src_cnt] = { Source::internal, Source::ts4, Source::midi };
    for (uint8_t i = 0; i < src_cnt; i++) {
        if (src[i] == _source) {
            auto n = i + 1;
            _source = src[n < src_cnt ? n : 0];
            _clock.SetPPQNIn(_source);
            _clock.SetExternalClock(_source != Source::internal);
            break;
        }
    }
    // Arm the dropout timer so switching into an external source doesn't immediately
    // read as "clock gone" before the first tick arrives (matches upstream toggle_source).
    _reset_us = _time ? _time->now_us() : 0;
    _clock.Run();
}

void Transport::tick(const bool external_tick)
{
    if (external_tick) {
        // An external clock edge. If we stopped the clock after a dropout, restart it -
        // in external mode Run() only arms (_is_about_to_run); this edge kicks it off and
        // re-aligns the timeline to the downbeat. Re-arm the dropout timer.
        if (!_clock.IsRunning()) _clock.Run();
        _reset_us = _time ? _time->now_us() : 0;
        _clock.Tick(true);
        return;
    }

    // No external edge this block. In external mode, if no tick has arrived for over 1s
    // (> the ~750ms max interval at 20bpm / 4PPQN) treat the clock as gone: stop the
    // timeline and force a divider realign once, so playback resumes cleanly and aligned
    // when the clock returns rather than freezing on SynClock's _hold. The IsRunning()
    // guard makes this fire once per dropout instead of every block.
    if (_clock.ExternalClock()) {
        uint32_t elapsed_us = _time ? (_time->now_us() - _reset_us) : 0u;
        if (elapsed_us >= 1000000u /*1s*/) {
            if (_clock.IsRunning()) {
                _clock.Stop();
                _divider_reset_counts(true);
            }
            return;
        }
    }

    _clock.Tick(false);
    // Manual-tempo tracking only applies to the internal clock; SynClock::SetTempo has
    // side effects (toggles run state + Reset) in external mode, so keep the guard.
    if (!_clock.ExternalClock()) _clock.SetTempo(_tempo.bpm());
};

void Transport::_emit(const bool tick, const bool is_quarter, const float tempo, const bool reset)
{
    if (tick) _tick_index++;
    if (_on_tick) {
        TransportTick e;
        e.index   = _tick_index;
        e.tick    = tick;
        e.key     = _is_key;
        e.quarter = is_quarter;
        e.tempo   = tempo;
        e.reset   = reset;
        _on_tick(e);
    }
}

void Transport::_on_clock_tick(const bool external_tick)
{
    if (external_tick) {
        _reset_us = _time ? _time->now_us() : 0;
        _send_clock = true;
    }

    // Given the clock resolution is 48PPQN, sending every second tick.
    if (_send_clock && _on_clock_out) _on_clock_out();
    _send_clock = !_send_clock;

    auto is_quarter = false;
    auto tick = _divider.tick();
    _is_key = false;
    if (tick) {
        if (--_key_tick_count <= 0) _make_key();

        if (--_quarter_tick_count <= 0) {
            _quarter_tick_count = static_cast<int8_t>(_divider.resolution());
            // Changing from something les then quarter to quarter and more I need to make sure that
            // key counter is in syc with quarters counter. Otherwise key counts can fall between quarters.
            if (_key_tick_interval >= _quarter_tick_count && (_key_tick_count % _quarter_tick_count) != 0) {
                _make_key();
            }
            is_quarter = true;
        }
    }

    // Fan the tick out to the subscribed engine (granular fan-out, delay sync, sequencer step), then
    // signal the quarter to the platform (metronome LED) - matching the old _indicate_quarter order
    // (the engine's click fires inside the tick, on_quarter last).
    _emit(tick, is_quarter, _clock.Tempo(), /*reset*/false);
    if (is_quarter && _on_quarter) _on_quarter(_is_key);
};

void Transport::set_key_tick_interval_norm(const float norm)
{
    _key_tick_interval = kKeyIntervals[std::round(norm * (kKeyIntervals.size() - 1))];
}

void Transport::reset()
{
    if (_source == Source::internal) return;
    _divider_reset_counts();
}

void Transport::_divider_reset_counts(const bool force)
{
    auto reset_elapsed_us = _time ? (_time->now_us() - _reset_us) : 0xFFFFFFFFu;
    if (reset_elapsed_us >= 4000u/*4ms*/ || force) {
        //The clock is stopped or we're long after the tick,
        // so the next one will be the key
        _key_tick_count = 0;
        _quarter_tick_count = 0;
        _divider.reset();
        // Tell the engine the grid was realigned (granular resets its per-deck track dividers).
        _emit(/*tick*/false, /*is_quarter*/false, _clock.Tempo(), /*reset*/true);
        if (_clock.IsRunning()) {
            _clock.Stop();
            _clock.Run();
        }
    }
    else if (!_is_key) {
        // There's recent tick and it's not the key one.
        // Then we simulate the key.
        _quarter_tick_count = static_cast<int8_t>(_divider.resolution());
        _make_key();
        _emit(/*tick*/true, /*is_quarter*/true, _clock.Tempo(), /*reset*/false);
        if (_on_quarter) _on_quarter(_is_key);
    }
}

void Transport::_make_key()
{
    _key_tick_count = _key_tick_interval;
    _is_key = true;
}
