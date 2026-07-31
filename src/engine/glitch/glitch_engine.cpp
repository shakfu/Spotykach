// SPDX-License-Identifier: GPL-3.0-only
//
// This engine is GPLv3, NOT MIT like the rest of this repository: it incorporates glitch_voice.h, whose
// algorithms are ported from the GPLv3 Noisferatu (https://github.com/rob-scape/noisferatu), so a build
// with ENGINE=glitch is a combined work distributed under GPLv3. See src/engine/glitch/{NOTICE.md,LICENSE}.
#include "engine/glitch/glitch_engine.h"
#include "engine/indicators.h"   // shared indicator toolkit (docs/dev/indicator-comparison.md §7)

#include "daisysp.h"   // daisysp::SoftLimit

namespace spotykach {

void GlitchEngine::init(const EngineContext& ctx) {
    const float sr = ctx.sample_rate > 0.f ? ctx.sample_rate : 48000.f;
    // Distinct seeds so the two decks decorrelate (same algorithm on both still sounds different).
    _voice[0].init(sr, 0x12345678u);
    _voice[1].init(sr, 0x2545F491u);
    for (int i = 0; i < 2; i++) {
        _voice[i].set_p1(_p1[i]);
        _voice[i].set_p2(_p2[i]);
        _voice[i].set_pitch(_pitch[i]);
    }
}

void GlitchEngine::process(const float* const* /*in*/, float** out, size_t size) {
    // Per-deck stereo placement from the routing switch (mirrors the radio engine).
    float pLa, pRa, pLb, pRb;
    switch (_route) {
        case Route::DoubleMono:        pLa = 1.f; pRa = 0.f; pLb = 0.f; pRb = 1.f; break;       // A left, B right
        case Route::GenerativeStereo:  pLa = _rndL[0]; pRa = _rndR[0]; pLb = _rndL[1]; pRb = _rndR[1]; break;
        case Route::Stereo: default:   pLa = pRa = pLb = pRb = kCenterGain; break;              // both centred
    }
    const float La = _gain[0] * _gA * pLa, Ra = _gain[0] * _gA * pRa,
                Lb = _gain[1] * _gB * pLb, Rb = _gain[1] * _gB * pRb;

    for (size_t i = 0; i < size; i++) {
        // Deck A
        float a = _voice[0].process();
        _lp[0] += _tone[0] * (a - _lp[0]);   // ENV -> one-pole low-pass (tone)
        a = _lp[0];
        // Deck B
        float b = _voice[1].process();
        _lp[1] += _tone[1] * (b - _lp[1]);
        b = _lp[1];

        out[0][i] = daisysp::SoftLimit(a * La + b * Lb);
        out[1][i] = daisysp::SoftLimit(a * Ra + b * Rb);
    }
}

// SIZE -> param 1, POS -> param 2, PITCH -> master pitch, ENV -> output tone, MIX -> volume,
// Crossfade -> A/B blend, Alt+PITCH (Aux) -> ALGORITHM select.
void GlitchEngine::set_param(ParamId id, DeckRef::Ref d, float v) {
    const int i = (d == DeckRef::A) ? 0 : 1;
    if (id == ParamId::Size)            { _p1[i] = v; _voice[i].set_p1(v); }
    else if (id == ParamId::Pos)        { _p2[i] = v; _voice[i].set_p2(v); }
    else if (id == ParamId::Speed)      { _pitch[i] = v; _voice[i].set_pitch(v); }
    else if (id == ParamId::Env)        { _tone[i] = v * v; }                       // 0 = dark, 1 = open
    else if (id == ParamId::Mix)        { _gain[i] = v; }
    else if (id == ParamId::Crossfade)  { _xfade = v; _gA = v <= 0.5f ? 1.f : 2.f * (1.f - v);
                                                       _gB = v >= 0.5f ? 1.f : 2.f * v; }
    else if (id == ParamId::Aux) {
        _aux[i] = v;
        int idx = static_cast<int>(v * glitch::kAlgoCount);
        idx = idx < 0 ? 0 : (idx >= glitch::kAlgoCount ? glitch::kAlgoCount - 1 : idx);
        if (idx != _algo_index(d)) _voice[i].set_algo(static_cast<glitch::Algo>(idx));
    }
}

float GlitchEngine::param(ParamId id, DeckRef::Ref d) const {
    const int i = (d == DeckRef::A) ? 0 : 1;
    if (id == ParamId::Size)  return _p1[i];
    if (id == ParamId::Pos)   return _p2[i];
    if (id == ParamId::Speed) return _pitch[i];
    if (id == ParamId::Env)   return _tone[i];
    if (id == ParamId::Mix)   return _gain[i];
    if (id == ParamId::Aux)   return (static_cast<float>(_algo_index(d)) + 0.5f) / static_cast<float>(glitch::kAlgoCount);
    return 0.f;
}

void GlitchEngine::set_aux_active(DeckRef::Ref d, bool held) {
    _aux_held[(d == DeckRef::A) ? 0 : 1] = held;
}

// Routing switch (mirrors the granular/radio int mapping): 0=Stereo, 1=DoubleMono, 2=GenerativeStereo.
bool GlitchEngine::set_config(ConfigId id, DeckRef::Ref, int value) {
    if (id == ConfigId::Route) {
        const Route r = (value == 2) ? Route::GenerativeStereo
                      : (value == 1) ? Route::DoubleMono
                                     : Route::Stereo;
        if (r != _route) { _route = r; if (_route == Route::GenerativeStereo) _roll_random_pans(); }
    }
    return false;
}

// Play pad -> regenerate this deck's glitch buffer (a fresh sparse pattern for the buffer-player algos).
// Rev pad inert. Returns false (no is-empty LED semantics for a generator).
bool GlitchEngine::on_play_pad(DeckRef::Ref d, bool reverse) {
    if (!reverse) _voice[(d == DeckRef::A) ? 0 : 1].regen();
    return false;
}

void GlitchEngine::render(DisplayModel& m) {
    m.clear();
    for (DeckRef::Ref dk : { DeckRef::A, DeckRef::B }) {
        const int i = (dk == DeckRef::A) ? 0 : 1;
        m.play[i] = { pal::kGreen, 0.6f };   // a generator is always "running"

        if (_aux_held[i]) {
            // ALGORITHM selector: kAlgoCount dots around the ring, the current algorithm bright.
            ring::selector(m.ring[i], glitch::kAlgoCount, _algo_index(dk), pal::kWhite, 0.15f, 0x202020);
        } else {
            // Otherwise a faint base ring with a marker at the current algorithm position.
            m.ring[i].set_hex_color(0x101010); m.ring[i].set_segment(0.f, 0.999f);
            ring::playhead(m.ring[i],
                           (static_cast<float>(_algo_index(dk)) + 0.5f) / static_cast<float>(glitch::kAlgoCount),
                           1.f, 0x00ff80);
        }
        m.ring[i].set_updated();
    }
    led::route_leds(m, _route);
}

int GlitchEngine::_algo_index(DeckRef::Ref d) const {
    return static_cast<int>(_voice[(d == DeckRef::A) ? 0 : 1].algo());
}

void GlitchEngine::_roll_random_pans() {
    for (int i = 0; i < 2; i++) {
        _rng = _rng * 1664525u + 1013904223u;
        const float p = static_cast<float>(_rng >> 8) * (1.f / 16777216.f);   // [0,1)
        _rndL[i] = std::cos(p * 1.57079632679f);
        _rndR[i] = std::sin(p * 1.57079632679f);
    }
}

} // namespace spotykach
