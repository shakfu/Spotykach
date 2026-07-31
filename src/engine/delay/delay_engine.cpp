// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#include "engine/delay/delay_engine.h"
#include "engine/arena.h"
#include "engine/indicators.h"   // shared indicator toolkit (docs/dev/indicator-comparison.md §7)
#include "config.h"   // kTempoMinBpm (sizes the delay buffer for the longest division at min tempo)

#include <cmath>
#include <algorithm>

using namespace spotykach;

// Musical-division table (quarter-note beats), ascending so SIZE up = longer delay. Straight + dotted
// (.) + triplet (T): 1/16T 1/16 1/8T 1/16. 1/8 1/4T 1/8. 1/4 1/4. 1/2. delay_time = (60/bpm)*beats.
static constexpr float kDivBeats[] = {
    1.f/6.f, 0.25f, 1.f/3.f, 0.375f, 0.5f, 2.f/3.f, 0.75f, 1.f, 1.5f, 2.f
};
static constexpr int kDivCount = sizeof(kDivBeats) / sizeof(kDivBeats[0]);

// The buffer must hold the longest division (1/2 note) at the slowest tempo, plus a couple samples of
// headroom for the fractional read. ~6 s at the min tempo; trivially fits the 48 MB arena.
static constexpr float  kMaxDelaySeconds = 2.f * 60.f / kTempoMinBpm;
static constexpr size_t kReadHeadroom    = 4;

static constexpr float kTwoPi    = 6.2831853f;
static constexpr float kTapeDrive = 1.6f;  // feedback saturation amount in Tape mode

// --- Shifter: crossfading two-head pitch shifter --------------------------------------------------
float DelayEngine::Shifter::read(float off) const
{
    float rp = static_cast<float>(w) - off;
    while (rp < 0.f) rp += static_cast<float>(kWin);
    const size_t i0 = static_cast<size_t>(rp);
    const float  f  = rp - static_cast<float>(i0);
    const size_t i1 = (i0 + 1 >= kWin) ? 0 : i0 + 1;
    return buf[i0] * (1.f - f) + buf[i1] * f;
}

// Read offset advances at (1 - ratio)/sample so the read rate is `ratio` (output transposed by ratio);
// two heads half a window apart, raised-cosine crossfaded (gains sum to 1, zero at the wrap). Unity-safe.
float DelayEngine::Shifter::process(float x, float ratio)
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

// --- Tap: one delay line over borrowed SDRAM ------------------------------------------------------
void DelayEngine::Tap::init(void* mem, float sample_rate, size_t length)
{
    buf   = static_cast<float*>(mem);
    sr    = sample_rate;
    len   = length;
    min_d = sr * 0.001f;                              // 1 ms floor
    max_d = static_cast<float>(len - kReadHeadroom);  // buffer ceiling (room for the fractional read)
    div   = 0; beats = kDivBeats[0]; target_d = min_d;
    w = 0; mode = Clean;
    s_delay = min_d; s_fb = 0.f; s_mix = 0.f; s_ratio = 1.f;
    fb = 0.f; mix = 0.f; ratio = 1.f; tone_coef = 1.f;
    tone_lp = 0.f; peak = 0.f; _x = 0.f; _wet = 0.f;
    mod_ph = 0.f; mod_rate = 0.f; mod_depth = 0.f; frozen = false;
    reversed = false; rev_ph = 0.f;
    out_shift.clear(); fb_shift.clear();
    if (buf) for (size_t i = 0; i < len; i++) buf[i] = 0.f;
}

void DelayEngine::Tap::set_div(float norm)
{
    div   = std::clamp(static_cast<int>(std::round(norm * (kDivCount - 1))), 0, kDivCount - 1);
    beats = kDivBeats[div];
}

// ENV 0..1 -> feedback one-pole low-pass coefficient. env=1 fully open (transparent); lower = darker.
void DelayEngine::Tap::set_tone(float env)
{
    if (env >= 0.98f) { tone_coef = 1.f; return; }
    const float fc = 40.f * std::exp2(env * 9.f);              // ~40 Hz .. ~20 kHz, exponential
    const float c  = 1.f - std::exp(-kTwoPi * fc / sr);
    tone_coef = std::clamp(c, 0.f, 1.f);
}

void DelayEngine::Tap::set_target(float bpm)
{
    const float d = sr * (60.f / bpm) * beats;
    target_d = std::clamp(d, min_d, max_d);
}

// Linear-interpolated read `off` samples behind the write head (wraps the circular buffer).
float DelayEngine::Tap::read_buf(float off) const
{
    float rp = static_cast<float>(w) - off;
    while (rp < 0.f) rp += static_cast<float>(len);
    const size_t i0 = static_cast<size_t>(rp);
    const float  frac = rp - static_cast<float>(i0);
    const size_t i1 = (i0 + 1 >= len) ? 0 : i0 + 1;
    return buf[i0] * (1.f - frac) + buf[i1] * frac;
}

// Reverse delay: play the most recent `win` samples backwards. A phase walks forward 0..win while the
// read offset (= win - phase) shrinks from the oldest sample toward the write head, so time runs in
// reverse; at the wrap the offset would jump win->0 (a click), so two heads half a window apart are
// raised-cosine crossfaded (gains sum to 1, zero at the wrap). Mirrors the Shifter's seamless-wrap trick.
float DelayEngine::Tap::read_rev(float win)
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

// Smooth controls, read the (optionally wow-modulated) fractional tap, and colorize the signal that
// will be fed back: tone low-pass (all modes), plus Tape saturation or Shimmer +12 shift. Returns the
// pre-feedback-gain signal; the heard wet (stashed in _wet) is the raw tap, transposed later by PITCH.
float DelayEngine::Tap::read_color(float x_in)
{
    _x = x_in;
    if (!buf) { _wet = 0.f; return 0.f; }
    constexpr float kSmooth = 0.0015f; // per-sample one-pole glide (no zipper)
    s_delay += (target_d - s_delay) * kSmooth;
    s_fb    += (fb       - s_fb)    * kSmooth;
    s_mix   += (mix      - s_mix)   * kSmooth;
    s_ratio += (ratio    - s_ratio) * kSmooth;

    // Delay-time modulation: the user LFO (MODFREQ rate / MOD_AMT depth) gives chorus/flange/vibrato;
    // Tape adds a floor so it always warbles a touch even with the mod knobs down (its character).
    float rate = mod_rate, depth = mod_depth;
    if (mode == Tape) { if (rate < 0.5f) rate = 0.7f; depth += 0.0015f * sr; } // ~0.7 Hz, +1.5 ms floor
    float d = s_delay;
    if (depth > 0.5f) {
        mod_ph += kTwoPi * rate / sr;
        if (mod_ph >= kTwoPi) mod_ph -= kTwoPi;
        d += std::sin(mod_ph) * depth;
        if (d < min_d) d = min_d;
    }
    // Reverse: read the smoothed-delay-length window backwards (the mod LFO still wobbles the window in
    // Tape). Otherwise the normal forward tap `d` samples behind the write head.
    _wet = reversed ? read_rev(d) : read_buf(d);

    float s = _wet;
    tone_lp += (s - tone_lp) * tone_coef;        // feedback tone (ENV); transparent when tone_coef == 1
    s = tone_lp;
    if (mode == Tape)         s = std::tanh(s * kTapeDrive);
    else if (mode == Shimmer) s = fb_shift.process(s, 2.f); // +12 (octave up) each pass -> shimmer
    return s;
}

// Write input + feedback (cross-fed in ping-pong: fbsig may be the OTHER tap's), advance the write
// head, and return the wet/dry mix. The heard wet is the raw tap (_wet) transposed by PITCH.
float DelayEngine::Tap::write_out(float fbsig)
{
    if (!buf) return _x;
    // Freeze: loop the buffer at unity feedback, ignoring new input (you still hear the input dry, so
    // you can play over the held loop). Clean stays an exact loop; Tape/Shimmer evolve under the soft-clip.
    const float fb_eff = frozen ? 1.f : s_fb;
    const float x_eff  = frozen ? 0.f : _x;
    const float w_in   = x_eff + fb_eff * fbsig;
    // Clean stays bit-for-bit; Tape/Shimmer soft-clip the loop so the colored feedback can't run away
    // (transparent below ~0.6, gently saturating above).
    buf[w] = (mode == Clean) ? w_in : std::tanh(w_in * 0.8f) * 1.25f;
    if (++w >= len) w = 0;

    const float a = _x < 0.f ? -_x : _x;
    peak = a > peak ? a : peak * 0.9995f;

    return _x * (1.f - s_mix) + out_shift.process(_wet, s_ratio) * s_mix;
}

// --- DelayEngine ----------------------------------------------------------------------------------
void DelayEngine::init(const EngineContext& ctx)
{
    _transport = ctx.transport; // delay time syncs to this clock (read-only); the platform owns it.

    // Seed the platform-engine-seeded knobs to sensible startup values (the rest default to 0 = dry,
    // no feedback): SIZE = 1/4-ish division, PITCH = unity, ENV = open tone (so Clean boots transparent).
    for (int d = 0; d < DeckRef::Count; d++) {
        _param[static_cast<size_t>(ParamId::Size)][d]  = 1.0f;
        _param[static_cast<size_t>(ParamId::Speed)][d] = 0.5f; // unity pitch
        _param[static_cast<size_t>(ParamId::Env)][d]   = 1.0f; // open feedback tone
    }

    // Sub-allocate two delay lines from the platform's opaque SDRAM arena (each ~6 s).
    Arena arena(ctx.arena);
    const size_t len = static_cast<size_t>(ctx.sample_rate * kMaxDelaySeconds) + kReadHeadroom;
    _tap[DeckRef::A].init(arena.alloc<float>(len), ctx.sample_rate, len);
    _tap[DeckRef::B].init(arena.alloc<float>(len), ctx.sample_rate, len);
}

// Derive a tap's per-block targets from the cached knob values for deck `src` (deck B reads A's when a
// linked route is active) plus that deck's character mode.
void DelayEngine::apply_params(Tap& t, DeckRef::Ref src)
{
    const size_t s = static_cast<size_t>(src);
    t.set_div(_param[static_cast<size_t>(ParamId::Size)][s]);
    t.fb    = _param[static_cast<size_t>(ParamId::Pos)][s] * 0.95f;          // capped to avoid runaway
    t.mix   = _param[static_cast<size_t>(ParamId::Mix)][s];
    t.ratio = std::exp2((_param[static_cast<size_t>(ParamId::Speed)][s] - 0.5f) * 2.f); // +/-1 octave
    t.set_tone(_param[static_cast<size_t>(ParamId::Env)][s]);
    t.mode      = _mode[s];
    t.mod_rate  = _mod_rate[s];
    t.mod_depth = _param[static_cast<size_t>(ParamId::ModAmp)][s] * 0.012f * t.sr; // MOD_AMT -> 0..12 ms
    t.frozen    = _freeze[s];
    t.reversed  = _reverse[s];
}

void DelayEngine::process(const float* const* in, float** out, size_t size)
{
    const float bpm      = _transport ? _transport->tempo() : 120.f;
    const bool  linked   = (_route != Route::DoubleMono);          // Stereo + Ping-pong share deck A's controls
    const bool  pingpong = (_route == Route::GenerativeStereo);    // cross-feedback on the 3rd route position

    apply_params(_tap[DeckRef::A], DeckRef::A);
    apply_params(_tap[DeckRef::B], linked ? DeckRef::A : DeckRef::B);
    _tap[DeckRef::A].set_target(bpm);
    _tap[DeckRef::B].set_target(bpm);

    for (size_t i = 0; i < size; i++) {
        const float fa = _tap[DeckRef::A].read_color(in[0][i]);
        const float fb = _tap[DeckRef::B].read_color(in[1][i]);
        if (pingpong) {                                            // echoes bounce L<->R
            out[0][i] = _tap[DeckRef::A].write_out(fb);
            out[1][i] = _tap[DeckRef::B].write_out(fa);
        } else {
            out[0][i] = _tap[DeckRef::A].write_out(fa);
            out[1][i] = _tap[DeckRef::B].write_out(fb);
        }
    }
}

void DelayEngine::set_param(ParamId id, DeckRef::Ref deck, float v)
{
    _param[static_cast<size_t>(id)][_safe(deck)] = v; // source of truth; applied per block in process()
}

float DelayEngine::param(ParamId id, DeckRef::Ref deck) const
{
    return _param[static_cast<size_t>(id)][_safe(deck)];
}

// MODFREQ -> the mod LFO rate: ~0.05 Hz (slow chorus) .. ~12.8 Hz (vibrato), exponential. (The MOD_AMT
// depth arrives as ParamId::ModAmp via set_param.) The sync flag is unused - the LFO is free-running.
void DelayEngine::set_mod_speed(DeckRef::Ref deck, float value, bool /*sync*/)
{
    value = value < 0.f ? 0.f : (value > 1.f ? 1.f : value);
    _mod_rate[_safe(deck)] = 0.05f * std::exp2(value * 8.f);
}

// Play pad toggles Freeze for that deck (loop the buffer at unity feedback). The Rev pad toggles
// Reverse for that deck (read the buffer backwards over a delay-length window). Returns false (no
// "empty" concept for a delay).
bool DelayEngine::on_play_pad(DeckRef::Ref deck, bool reverse)
{
    const auto d = _safe(deck);
    if (reverse) _reverse[d] = !_reverse[d];
    else         _freeze[d]  = !_freeze[d];
    return false;
}

// ConfigId::Mode (the Reel/Slice/Drift switch, per deck) selects the character; ConfigId::Route selects
// the topology. Route wire values: 0=Stereo, 1=DoubleMono, 2=GenerativeStereo (see core.ui.cpp).
bool DelayEngine::set_config(ConfigId id, DeckRef::Ref deck, int value)
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

void DelayEngine::render(DisplayModel& m)
{
    m.clear();
    // Ring tint by character: Clean = cyan (canonical), Tape = amber, Shimmer = violet.
    static const uint32_t kModeColor[ModeCount] = { pal::kCyan, 0xff8000u, 0x8800ffu };

    led::route_leds(m, _route);   // 3 mode LEDs show the ROUTE (DoubleMono / Stereo / GenerativeStereo)

    const bool linked = (_route != Route::DoubleMono);
    for (int c = 0; c < 2; c++) {
        const int     src = linked ? DeckRef::A : c;       // linked routes mirror deck A
        const uint8_t md  = _mode[src];
        ring::level(m.ring[c], static_cast<float>(_tap[c].div + 1) / kDivCount, kModeColor[md]); // division arc (SIZE)
        m.ring[c].set_updated();
        // Play pad: white when frozen, cyan when reversed, else green lit by the input signal.
        m.play[c] = _freeze[src]  ? DisplayModel::Indicator{ pal::kWhite, 1.f }
                  : _reverse[src] ? DisplayModel::Indicator{ pal::kCyan,  1.f }
                                  : DisplayModel::Indicator{ pal::kGreen, _tap[c].peak > 1e-3f ? 1.f : 0.15f };
    }
}
