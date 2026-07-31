// SPDX-License-Identifier: GPL-3.0-only
//
// GPLv3 (NOT MIT): incorporates the GPLv3 diffuser. See src/engine/qdelay/{NOTICE.md,LICENSE}.
#include "engine/qdelay/qdelay_engine.h"
#include "engine/arena.h"
#include "engine/indicators.h"   // shared indicator toolkit (docs/dev/indicator-comparison.md §7)
#include "config.h"   // kTempoMinBpm (sizes the delay buffer for the longest division at min tempo)

#include <cmath>
#include <algorithm>

using namespace spotykach;

// Musical-division table (quarter-note beats), ascending so SIZE up = longer delay. Identical to the
// delay engine: 1/16T 1/16 1/8T 1/16. 1/8 1/4T 1/8. 1/4 1/4. 1/2. delay_time = (60/bpm)*beats.
static constexpr float kDivBeats[] = {
    1.f/6.f, 0.25f, 1.f/3.f, 0.375f, 0.5f, 2.f/3.f, 0.75f, 1.f, 1.5f, 2.f
};
static constexpr int kDivCount = sizeof(kDivBeats) / sizeof(kDivBeats[0]);

static constexpr float  kMaxDelaySeconds = 2.f * 60.f / kTempoMinBpm; // buffer holds 1/2 note at min tempo
static constexpr size_t kReadHeadroom    = 4;

static constexpr float kTwoPi = 6.2831853f;

// Duck mode: a fast-attack / slow-release peak follower drives the wet attenuation.
static constexpr float kDuckAtk   = 0.01f;   // per-sample one-pole rise
static constexpr float kDuckRel   = 0.0006f; // per-sample one-pole fall
static constexpr float kDuckAmt   = 3.0f;    // sensitivity: env -> attenuation
static constexpr float kDuckFloor = 0.0f;    // deepest duck (0 = fully silenced under a hot input)

// --- Shifter: crossfading two-head pitch shifter (PITCH output transpose) --------------------------
float QdelayEngine::Shifter::read(float off) const
{
    float rp = static_cast<float>(w) - off;
    while (rp < 0.f) rp += static_cast<float>(kWin);
    const size_t i0 = static_cast<size_t>(rp);
    const float  f  = rp - static_cast<float>(i0);
    const size_t i1 = (i0 + 1 >= kWin) ? 0 : i0 + 1;
    return buf[i0] * (1.f - f) + buf[i1] * f;
}

float QdelayEngine::Shifter::process(float x, float ratio)
{
    buf[w] = x;
    if (++w >= kWin) w = 0;
    if (ratio > 0.999f && ratio < 1.001f) return x; // transparent at unity

    phase += (1.f - ratio);
    while (phase >= kWin) phase -= kWin;
    while (phase < 0.f)   phase += kWin;
    float off1 = phase + kWin * 0.5f;
    if (off1 >= kWin) off1 -= kWin;

    const float g0 = 0.5f * (1.f - std::cos(kTwoPi * phase / kWin));
    const float g1 = 0.5f * (1.f - std::cos(kTwoPi * off1  / kWin));
    return read(phase) * g0 + read(off1) * g1;
}

// --- Tap ------------------------------------------------------------------------------------------
void QdelayEngine::Tap::init(void* mem, float sample_rate, size_t length)
{
    buf   = static_cast<float*>(mem);
    sr    = sample_rate;
    len   = length;
    min_d = sr * 0.001f;
    max_d = static_cast<float>(len - kReadHeadroom);
    div   = 0; beats = kDivBeats[0]; target_d = min_d;
    w = 0; mode = Clean;
    s_delay = min_d; s_fb = 0.f; s_mix = 0.f; s_ratio = 1.f;
    fb = 0.f; mix = 0.f; ratio = 1.f; tone_coef = 1.f;
    tone_lp = 0.f; peak = 0.f; _x = 0.f; _wet = 0.f;
    mod_ph = 0.f; mod_rate = 0.f; mod_depth = 0.f; frozen = false;
    reversed = false; rev_ph = 0.f; duck_env = 0.f;
    out_shift.clear();
    if (buf) for (size_t i = 0; i < len; i++) buf[i] = 0.f;
}

void QdelayEngine::Tap::set_div(float norm)
{
    div   = std::clamp(static_cast<int>(std::round(norm * (kDivCount - 1))), 0, kDivCount - 1);
    beats = kDivBeats[div];
}

void QdelayEngine::Tap::set_tone(float env)
{
    if (env >= 0.98f) { tone_coef = 1.f; return; }
    const float fc = 40.f * std::exp2(env * 9.f);
    const float c  = 1.f - std::exp(-kTwoPi * fc / sr);
    tone_coef = std::clamp(c, 0.f, 1.f);
}

void QdelayEngine::Tap::set_target(float bpm)
{
    const float d = sr * (60.f / bpm) * beats;
    target_d = std::clamp(d, min_d, max_d);
}

float QdelayEngine::Tap::read_buf(float off) const
{
    float rp = static_cast<float>(w) - off;
    while (rp < 0.f) rp += static_cast<float>(len);
    const size_t i0 = static_cast<size_t>(rp);
    const float  frac = rp - static_cast<float>(i0);
    const size_t i1 = (i0 + 1 >= len) ? 0 : i0 + 1;
    return buf[i0] * (1.f - frac) + buf[i1] * frac;
}

// Reverse delay: backward read over a delay-length window, two heads crossfaded at the wrap (see the
// delay engine for the derivation - identical here).
float QdelayEngine::Tap::read_rev(float win)
{
    if (win < 2.f) win = 2.f;
    rev_ph += 1.f;
    while (rev_ph >= win) rev_ph -= win;
    float p2 = rev_ph + win * 0.5f;
    if (p2 >= win) p2 -= win;
    const float g1 = 0.5f * (1.f - std::cos(kTwoPi * rev_ph / win));
    const float g2 = 0.5f * (1.f - std::cos(kTwoPi * p2     / win));
    return read_buf(win - rev_ph) * g1 + read_buf(win - p2) * g2;
}

// Smooth controls, update the duck envelope, read the (optionally wow-modulated / reversed) tap and
// apply the feedback tone low-pass. Returns the pre-feedback-gain signal (Clean/Duck); the engine may
// route it through the diffuser before write_out (Diffuse). The heard wet is stashed in _wet.
float QdelayEngine::Tap::read_color(float x_in)
{
    _x = x_in;
    if (!buf) { _wet = 0.f; return 0.f; }
    constexpr float kSmooth = 0.0015f;
    s_delay += (target_d - s_delay) * kSmooth;
    s_fb    += (fb       - s_fb)    * kSmooth;
    s_mix   += (mix      - s_mix)   * kSmooth;
    s_ratio += (ratio    - s_ratio) * kSmooth;

    // Per-tap input peak envelope (drives Duck). Fast attack, slow release.
    const float ax = _x < 0.f ? -_x : _x;
    duck_env += (ax - duck_env) * (ax > duck_env ? kDuckAtk : kDuckRel);

    float rate = mod_rate, depth = mod_depth;
    float d = s_delay;
    if (depth > 0.5f) {
        mod_ph += kTwoPi * rate / sr;
        if (mod_ph >= kTwoPi) mod_ph -= kTwoPi;
        d += std::sin(mod_ph) * depth;
        if (d < min_d) d = min_d;
    }
    _wet = reversed ? read_rev(d) : read_buf(d);

    tone_lp += (_wet - tone_lp) * tone_coef; // feedback tone (ENV); transparent when tone_coef == 1
    return tone_lp;
}

// Duck mode: attenuate the heard wet by the input envelope (1 = open, kDuckFloor = fully ducked). Other
// modes pass the wet through at unity.
float QdelayEngine::Tap::duck_gain() const
{
    if (mode != Duck) return 1.f;
    const float g = 1.f - duck_env * kDuckAmt;
    return g < kDuckFloor ? kDuckFloor : (g > 1.f ? 1.f : g);
}

// Write input + feedback (cross-fed in ping-pong; diffused in Diffuse - the engine passes the right
// fbsig), advance the write head, and return the (ducked) wet/dry mix. PITCH transposes the wet.
float QdelayEngine::Tap::write_out(float fbsig)
{
    if (!buf) return _x;
    const float fb_eff = frozen ? 1.f : s_fb;
    const float x_eff  = frozen ? 0.f : _x;
    const float w_in   = x_eff + fb_eff * fbsig;
    // Clean stays bit-for-bit; the colored modes soft-clip the loop so feedback can't run away.
    buf[w] = (mode == Clean) ? w_in : std::tanh(w_in * 0.8f) * 1.25f;
    if (++w >= len) w = 0;

    const float a = _x < 0.f ? -_x : _x;
    peak = a > peak ? a : peak * 0.9995f;

    const float wet = out_shift.process(_wet, s_ratio) * duck_gain();
    return _x * (1.f - s_mix) + wet * s_mix;
}

// --- QdelayEngine ---------------------------------------------------------------------------------
void QdelayEngine::init(const EngineContext& ctx)
{
    _transport = ctx.transport;

    for (int d = 0; d < DeckRef::Count; d++) {
        _param[static_cast<size_t>(ParamId::Size)][d]  = 1.0f;
        _param[static_cast<size_t>(ParamId::Speed)][d] = 0.5f; // unity pitch
        _param[static_cast<size_t>(ParamId::Env)][d]   = 1.0f; // open feedback tone
    }

    // Sub-allocate two delay lines (each ~6 s) then the shared stereo diffuser from the SDRAM arena.
    Arena arena(ctx.arena);
    const size_t len = static_cast<size_t>(ctx.sample_rate * kMaxDelaySeconds) + kReadHeadroom;
    _tap[DeckRef::A].init(arena.alloc<float>(len), ctx.sample_rate, len);
    _tap[DeckRef::B].init(arena.alloc<float>(len), ctx.sample_rate, len);
    _diffuser.init(arena.alloc<float>(Diffuser::capacity_floats(ctx.sample_rate)), ctx.sample_rate);
    _diffuser.set_size(0.5f);
}

void QdelayEngine::apply_params(Tap& t, DeckRef::Ref src)
{
    const size_t s = static_cast<size_t>(src);
    t.set_div(_param[static_cast<size_t>(ParamId::Size)][s]);
    t.fb    = _param[static_cast<size_t>(ParamId::Pos)][s] * 0.95f;
    t.mix   = _param[static_cast<size_t>(ParamId::Mix)][s];
    t.ratio = std::exp2((_param[static_cast<size_t>(ParamId::Speed)][s] - 0.5f) * 2.f); // +/-1 octave
    t.set_tone(_param[static_cast<size_t>(ParamId::Env)][s]);
    t.mode      = _mode[s];
    t.mod_rate  = _mod_rate[s];
    t.mod_depth = _param[static_cast<size_t>(ParamId::ModAmp)][s] * 0.012f * t.sr; // MOD_AMT -> 0..12 ms
    t.frozen    = _freeze[s];
    t.reversed  = _reverse[s];
}

void QdelayEngine::process(const float* const* in, float** out, size_t size)
{
    const float bpm      = _transport ? _transport->tempo() : 120.f;
    const bool  linked   = (_route != Route::DoubleMono);
    const bool  pingpong = (_route == Route::GenerativeStereo);

    apply_params(_tap[DeckRef::A], DeckRef::A);
    apply_params(_tap[DeckRef::B], linked ? DeckRef::A : DeckRef::B);
    _tap[DeckRef::A].set_target(bpm);
    _tap[DeckRef::B].set_target(bpm);

    const bool diffA = _tap[DeckRef::A].mode == Diffuse;
    const bool diffB = _tap[DeckRef::B].mode == Diffuse;
    const bool any_diffuse = diffA || diffB;
    // Size the diffuser from deck A's SIZE so it tracks the delay length musically (linked routes use A).
    _diffuser.set_size(_param[static_cast<size_t>(ParamId::Size)][DeckRef::A]);

    for (size_t i = 0; i < size; i++) {
        float fa = _tap[DeckRef::A].read_color(in[0][i]);
        float fb = _tap[DeckRef::B].read_color(in[1][i]);

        // Diffuse: run the stereo diffuser over the feedback pair; each tap takes the diffused copy
        // only if it is in Diffuse mode (so DoubleMono can mix a Diffuse deck with a Clean/Duck deck).
        if (any_diffuse) {
            float da = fa, db = fb;
            _diffuser.process(da, db);
            if (diffA) fa = da;
            if (diffB) fb = db;
        }

        if (pingpong) {
            out[0][i] = _tap[DeckRef::A].write_out(fb);
            out[1][i] = _tap[DeckRef::B].write_out(fa);
        } else {
            out[0][i] = _tap[DeckRef::A].write_out(fa);
            out[1][i] = _tap[DeckRef::B].write_out(fb);
        }
    }
}

void QdelayEngine::set_param(ParamId id, DeckRef::Ref deck, float v)
{
    _param[static_cast<size_t>(id)][_safe(deck)] = v;
}

float QdelayEngine::param(ParamId id, DeckRef::Ref deck) const
{
    return _param[static_cast<size_t>(id)][_safe(deck)];
}

void QdelayEngine::set_mod_speed(DeckRef::Ref deck, float value, bool /*sync*/)
{
    value = value < 0.f ? 0.f : (value > 1.f ? 1.f : value);
    _mod_rate[_safe(deck)] = 0.05f * std::exp2(value * 8.f);
}

// Play pad -> Freeze; Rev pad -> Reverse (per deck), mirroring the delay engine.
bool QdelayEngine::on_play_pad(DeckRef::Ref deck, bool reverse)
{
    const auto d = _safe(deck);
    if (reverse) _reverse[d] = !_reverse[d];
    else         _freeze[d]  = !_freeze[d];
    return false;
}

bool QdelayEngine::set_config(ConfigId id, DeckRef::Ref deck, int value)
{
    if (id == ConfigId::Route) {
        const Route r = value == 2 ? Route::GenerativeStereo : value == 1 ? Route::DoubleMono : Route::Stereo;
        if (r == _route) return false;
        _route = r;
        return true;
    }
    if (id == ConfigId::Mode) {
        const auto    d = _safe(deck);
        const uint8_t m = static_cast<uint8_t>(std::clamp(value, 0, static_cast<int>(ModeCount) - 1));
        if (m == _mode[d]) return false;
        _mode[d] = m;
        return true;
    }
    return false;
}

void QdelayEngine::render(DisplayModel& m)
{
    m.clear();
    // Ring tint by character: Clean = cyan (canonical), Diffuse = teal, Duck = amber (canonical).
    static const uint32_t kModeColor[ModeCount] = { pal::kCyan, 0x00ddaau, pal::kAmber };

    led::route_leds(m, _route);   // 3 mode LEDs show the ROUTE (DoubleMono / Stereo / GenerativeStereo)

    const bool linked = (_route != Route::DoubleMono);
    for (int c = 0; c < 2; c++) {
        const int     src = linked ? DeckRef::A : c;
        const uint8_t md  = _mode[src];
        ring::level(m.ring[c], static_cast<float>(_tap[c].div + 1) / kDivCount, kModeColor[md]); // division arc (SIZE)
        m.ring[c].set_updated();
        // Play pad: white when frozen, cyan when reversed, else green lit by the input signal.
        m.play[c] = _freeze[src]  ? DisplayModel::Indicator{ pal::kWhite, 1.f }
                  : _reverse[src] ? DisplayModel::Indicator{ pal::kCyan,  1.f }
                                  : DisplayModel::Indicator{ pal::kGreen, _tap[c].peak > 1e-3f ? 1.f : 0.15f };
    }
}
