#include "engine/pstretch/pstretch_engine.h"
#include "engine/indicators.h"   // shared indicator toolkit (docs/dev/indicator-comparison.md §7)

#include "engine/arena.h"
#include "daisysp.h"   // daisysp::SoftLimit

#include <cmath>

namespace spotykach {

void PstretchEngine::init(const EngineContext& ctx) {
    const float sr = ctx.sample_rate > 0.f ? ctx.sample_rate : 48000.f;
    _sr = sr;
    for (int v = 0; v < 2; v++) _mod_rate[v] = kModRateMin * std::exp2(_mod_speed_n[v] * 8.f);
    Arena arena(ctx.arena);
    _stream = ctx.stream;   // SD streaming service (null if SPK_USE_STREAM off or on host without a stub)
    _time   = ctx.time;     // for the scrub re-seek settle (null -> re-seek immediately)
    _transport = ctx.transport;   // tempo source for the clock-synced LFO (null -> free-running only)

    // The FFT working set is in on-chip SRAM (engine members), not the SDRAM arena - see the header. Only
    // the per-voice input ring (allocated below) lives in SDRAM.
    _fft.init(kWindow, _cos, _sin, _brev);
    float* window = _window, *invnorm = _invnorm;
    _shared.fft = &_fft; _shared.window = window; _shared.invnorm = invnorm;
    _shared.n = kWindow; _shared.hop = kHop;

    // cos/sin LUT for the phase smear (radians -> index via L/2pi; negative angles wrap via the mask).
    constexpr float twopi = 6.28318530717958647692f;
    for (int i = 0; i < kLut; i++) {
        _lutc[i] = std::cos(twopi * static_cast<float>(i) / static_cast<float>(kLut));
        _luts[i] = std::sin(twopi * static_cast<float>(i) / static_cast<float>(kLut));
    }
    _shared.lutc = _lutc; _shared.luts = _luts;
    _shared.lutmask = kLut - 1; _shared.lutscale = static_cast<float>(kLut) / twopi;

    // PaulStretch window: (1 - x^2)^1.25, x in [-1, 1] - flat-topped with smooth edges.
    for (int i = 0; i < kWindow; i++) {
        const float x = 2.f * static_cast<float>(i) / static_cast<float>(kWindow - 1) - 1.f;
        window[i] = std::pow(1.f - x * x, 1.25f);
    }
    // 50%-overlap COLA gain per output position: 1 / (w[i]^2 + w[i+hop]^2), so the windowed analysis +
    // synthesis double-windowing reconstructs to unity (modulo the incoherence from phase randomization).
    for (int i = 0; i < kHop; i++) {
        const float a = window[i], b = window[i + kHop];
        const float d = a * a + b * b;
        invnorm[i] = d > 1e-6f ? 1.f / d : 0.f;
    }

    for (int v = 0; v < 2; v++) {
        float* inring = arena.alloc<float>(kRing, 16);   // ~1 MB ring stays in SDRAM (sequential reads)
        // re/im (scattered FFT scratch) stay in on-chip SRAM; ola/fifo (sequential) are SRAM at the default
        // window and the SDRAM arena at >= 8192 (where the SRAM set would overflow) - see the header note.
#if PSTRETCH_WINDOW >= 8192
        _ola[v]  = arena.alloc<float>(kWindow, 16);
        _fifo[v] = arena.alloc<float>(2 * kHop, 16);
#else
        _ola[v]  = _ola_sram[v];
        _fifo[v] = _fifo_sram[v];
#endif
        _voice[v].init(sr, &_shared, inring, kRing, _re[v], _im[v], _ola[v], _fifo[v], 2 * kHop,
                       v == 0 ? 0x12345678u : 0x2545F491u);
        _apply(v == 0 ? DeckRef::A : DeckRef::B);
    }
}

// Push the cached 0..1 control values into a voice as its real units.
void PstretchEngine::_apply(DeckRef::Ref d) {
    const int i = (d == DeckRef::A) ? 0 : 1;
    _voice[i].set_stretch(std::pow(64.f, _stretch_n[i]));             // 1x .. 64x
    _voice[i].set_diffusion(_diffuse_n[i]);                           // 0 .. 1
    _voice[i].set_pitch(std::exp2((_pitch_n[i] - 0.5f) * 2.f));       // +/- 1 octave
    _voice[i].set_freeze(_frozen[i]);
}

// Size/Pos mod-routing switch -> LFO target: Pos-only = Diffusion, Size-only = Stretch, both = Tone.
void PstretchEngine::_update_mod_target(int i) {
    if (_mod_start_on[i] && _mod_size_on[i]) _mod_target[i] = ModTarget::Tone;
    else if (_mod_size_on[i])                _mod_target[i] = ModTarget::Stretch;
    else                                     _mod_target[i] = ModTarget::Diffusion;
}

// Combine deck i's base knob values with its CV offsets and one LFO/follower modulation target, push the
// results to the voice, and return the effective dry/wet. Once per block (block rate is ample for the
// sub-10 Hz ambient modulation). With mod depth 0 and nothing patched this reproduces _apply exactly.
float PstretchEngine::_derive_and_push(int i, const float* in, size_t n) {
    // Modulation source in [-1, 1]: the LFO (sine/triangle) or the input-envelope follower. Computed every
    // block regardless of depth so the Mod CV out and Cycle LED always emit a running LFO; the depth (Glow)
    // only scales how far it moves the internal target. So Glow 0 leaves the AUDIO path un-modulated but the
    // LFO still free-runs as a CV source.
    float m;
    if (_mod_follow[i]) {
        float pk = 0.f;
        for (size_t k = 0; k < n; k++) { const float a = std::fabs(in[k]); if (a > pk) pk = a; }
        _foll[i] += (pk - _foll[i]) * (pk > _foll[i] ? 0.3f : 0.02f);   // fast attack / slow release
        const float e = _foll[i] > 1.f ? 1.f : _foll[i];
        m = 2.f * e - 1.f;                                             // 0..1 -> -1..1
    } else {
        if (_mod_synced[i] && _transport) {   // Alt+Cycle: lock the rate to a musical division of tempo
            int idx = static_cast<int>(_mod_speed_n[i] * static_cast<float>(kSyncDivs));
            if (idx >= kSyncDivs) idx = kSyncDivs - 1; else if (idx < 0) idx = 0;
            _mod_rate[i] = (_transport->tempo() / 60.f) * kSyncMult[idx];
        }
        _lfo_ph[i] += _mod_rate[i] * static_cast<float>(n) / _sr;
        if (_lfo_ph[i] >= 1.f) _gate_out[i] = true;                   // crossed a cycle boundary -> gate out
        _lfo_ph[i] -= std::floor(_lfo_ph[i]);                          // wrap 0..1
        m = (_lfo_shape[i] == 1) ? (4.f * std::fabs(_lfo_ph[i] - 0.5f) - 1.f)      // triangle
                                 : std::sin(6.28318530717958647692f * _lfo_ph[i]); // sine
    }
    _lfo_out[i] = 0.5f * (m + 1.f);                                    // 0..1 unipolar CV (Mod CV out + LED)
    const float depth = _mod_depth[i];

    // Base (knob) + CV, then the single modulated target. Pitch and stretch take their CV jacks; the LFO
    // drives diffusion / stretch / tone (diffusion + tone have no CV jack, so the LFO is their only route).
    float strn = _stretch_n[i] + _cv_str[i];                     // normalized SIZE
    float diff = _diffuse_n[i];                                  // POS diffusion (no CV jack)
    float tone = _tone[i];                                       // ENV tone LP coef (no CV jack)
    const float oct = (_pitch_n[i] - 0.5f) * 2.f + _cv_oct[i];   // pitch in octaves (knob + V/Oct CV)
    if (depth > 0.f) {
        switch (_mod_target[i]) {
            case ModTarget::Diffusion: diff += m * depth * kModDiffAmt; break;
            case ModTarget::Stretch:   strn += m * depth * kModStrAmt;  break;
            case ModTarget::Tone:      tone += m * depth * kModToneAmt; break;
        }
    }
    if (strn < 0.f) strn = 0.f; else if (strn > 1.f) strn = 1.f;
    if (diff < 0.f) diff = 0.f; else if (diff > 1.f) diff = 1.f;
    if (tone < 0.f) tone = 0.f; else if (tone > 1.f) tone = 1.f;

    _voice[i].set_stretch(std::pow(64.f, strn));
    _voice[i].set_diffusion(diff);
    _voice[i].set_pitch(std::exp2(oct));
    _tone_eff[i] = tone;

    float mix = _wet[i] + _cv_mix[i];
    if (mix < 0.f) mix = 0.f; else if (mix > 1.f) mix = 1.f;
    return mix;
}

void PstretchEngine::process(const float* const* in, float** out, size_t size) {
    const size_t n = size > kMaxFrames ? kMaxFrames : size;

    // Fold each deck's knob values + CV offsets + LFO/follower modulation into its voice for this block, and
    // get the effective dry/wet. Done before work() so the modulated stretch/pitch/diffusion drive this hop.
    const float mixA = _derive_and_push(0, in[0], n);
    const float mixB = _derive_and_push(1, in[1], n);

    float wetA[kMaxFrames], wetB[kMaxFrames];
    // 1. Fill each voice's ring from its source: live input, or - in SD mode - a clip streamed from the
    //    card (pull only the few frames the slow read head needs this block, decode int16->float, feed).
    //    2. Advance the pipelined workers under a small SHARED per-block budget so a whole FFT never lands
    //    in one block: deck A takes what it needs (it idles ~1/3 of hops, leaving the budget to B), B gets
    //    the remainder. The work spreads over the ~21 blocks a hop's output lasts, flattening per-block
    //    load. 3. Drain each voice's FIFO.
    for (int v = 0; v < 2; v++) {
        if (_source[v] == Source::SD) {
            uint32_t want = _voice[v].sd_want();
            if (want > 0 && _stream) {
                const DeckRef::Ref d = (v == 0) ? DeckRef::A : DeckRef::B;
                const uint32_t got = _stream->play_consume(d, reinterpret_cast<uint8_t*>(_sdraw),
                                                           want * sizeof(int16_t)) / sizeof(int16_t);
                for (uint32_t k = 0; k < got; k++) _sdbuf[k] = static_cast<float>(_sdraw[k]) * (1.f / 32768.f);
                _voice[v].feed_sd(_sdbuf, got);
            }
        } else {
            _voice[v].write_input(in[v], n);
        }
    }
    int budget = kWorkBudget;
    budget -= _voice[0].work(budget);
    if (budget > 0) _voice[1].work(budget);
    _voice[0].drain(wetA, n);
    _voice[1].drain(wetB, n);

    // Per-deck stereo placement from the routing switch (mirrors the radio/glitch engines).
    float pLa, pRa, pLb, pRb;
    switch (_route) {
        case Route::DoubleMono:        pLa = 1.f; pRa = 0.f; pLb = 0.f; pRb = 1.f; break;   // A left, B right
        case Route::GenerativeStereo:  pLa = _rndL[0]; pRa = _rndR[0]; pLb = _rndL[1]; pRb = _rndR[1]; break;
        case Route::Stereo: default:   pLa = pRa = pLb = pRb = kCenterGain; break;          // both centred
    }
    const float La = _gA * pLa, Ra = _gA * pRa, Lb = _gB * pLb, Rb = _gB * pRb;

    for (size_t i = 0; i < n; i++) {
        // dry/wet (effective mix incl. CV), then ENV tone low-pass per deck.
        float a = in[0][i] * (1.f - mixA) + wetA[i] * mixA;
        float b = in[1][i] * (1.f - mixB) + wetB[i] * mixB;
        _lp[0] += _tone_eff[0] * (a - _lp[0]); a = _lp[0];   // effective tone (base + LFO mod)
        _lp[1] += _tone_eff[1] * (b - _lp[1]); b = _lp[1];

        out[0][i] = daisysp::SoftLimit(a * La + b * Lb);
        out[1][i] = daisysp::SoftLimit(a * Ra + b * Rb);
    }
}

// SIZE -> stretch, POS -> diffusion, PITCH -> pitch, ENV -> tone, MIX -> dry/wet, Crossfade -> A/B blend,
// Aux (Alt+PITCH) -> SD clip select (quantize the knob to a clip index; re-open live if this deck streams).
void PstretchEngine::set_param(ParamId id, DeckRef::Ref d, float v) {
    const int i = (d == DeckRef::A) ? 0 : 1;
    if (id == ParamId::Size)           { _stretch_n[i] = v; _voice[i].set_stretch(std::pow(64.f, v)); }
    else if (id == ParamId::Pos)       { _diffuse_n[i] = v; _voice[i].set_diffusion(v); }
    else if (id == ParamId::Speed)     { _pitch_n[i] = v; _voice[i].set_pitch(std::exp2((v - 0.5f) * 2.f)); }
    else if (id == ParamId::Env)       { _tone[i] = v * v; }                       // 0 = dark, 1 = open
    else if (id == ParamId::Mix)       { _wet[i] = v; }
    else if (id == ParamId::ModAmp)    { _mod_depth[i] = v; }                       // Glow -> LFO depth (0 = off)
    else if (id == ParamId::Crossfade) { _xfade = v; _recompute_xfade(); }
    else if (id == ParamId::Aux) {
        _clip_n[i] = v;
        _ensure_scan();
        if (_nclips > 0) {
            int idx = static_cast<int>(v * static_cast<float>(_nclips));
            if (idx >= _nclips) idx = _nclips - 1;
            if (idx < 0) idx = 0;
            if (idx != _clip_sel[i]) {
                _clip_sel[i] = idx;
                if (_source[i] == Source::SD) {
                    _open_clip(i);                             // change clips live on a streaming deck
                    _scrub_opened[i] = 0.f; _scrub_pending[i] = false;   // new clip starts at frame 0
                }
            }
        }
    }
    else if (id == ParamId::AltPos) {
        // SCRUB (SD only): mark a re-seek to position v in the clip and (re)start the settle clock; the
        // actual FatFs seek happens in prepare() once the knob stops, so a sweep only opens where you land.
        _scrub_n[i] = v;
        if (_source[i] == Source::SD) {
            _scrub_pending[i] = true;
            _scrub_at[i] = _time ? _time->now_ms() : 0;
        }
    }
}

float PstretchEngine::param(ParamId id, DeckRef::Ref d) const {
    const int i = (d == DeckRef::A) ? 0 : 1;
    if (id == ParamId::Size)  return _stretch_n[i];
    if (id == ParamId::Pos)   return _diffuse_n[i];
    if (id == ParamId::Speed) return _pitch_n[i];
    if (id == ParamId::Env)   return _tone[i];
    if (id == ParamId::Mix)   return _wet[i];
    if (id == ParamId::ModSpeed) return _mod_speed_n[i];   // Cycle knob pickup
    if (id == ParamId::ModAmp)   return _mod_depth[i];     // Glow knob pickup
    // Aux readback: the centre of the selected clip's slot (like radio's bank readback) for knob pickup.
    if (id == ParamId::Aux)   return _nclips > 0
                                     ? (static_cast<float>(_clip_sel[i]) + 0.5f) / static_cast<float>(_nclips)
                                     : 0.f;
    if (id == ParamId::AltPos) return _scrub_n[i];   // scrub position for the Alt+POS knob pickup
    return 0.f;
}

void PstretchEngine::set_aux_active(DeckRef::Ref d, bool active) {
    _aux_held[(d == DeckRef::A) ? 0 : 1] = active;
}

// Cycle -> LFO rate. Plain (Cycle alone): free-running ~0.03..7.7 Hz, exponential. Alt+Cycle (sync=true):
// the knob quantizes to a musical division and the rate is locked to the transport tempo, recomputed per
// block in _derive_and_push (so it tracks tempo changes). The Alt state of the last Cycle move sets sync.
void PstretchEngine::set_mod_speed(DeckRef::Ref d, float value, bool sync) {
    const int i = (d == DeckRef::A) ? 0 : 1;
    _mod_speed_n[i] = value;
    _mod_synced[i]  = sync;
    if (!sync) _mod_rate[i] = kModRateMin * std::exp2(value * 8.f);   // synced rate is derived per block
}

// CV inputs are ADDITIVE offsets on top of the knobs (calibrated to ~0 unpatched, so the knob alone rules
// with nothing patched); they are summed and clamped per block in _derive_and_push. V/Oct arrives as
// calibrated semitones (-> octaves); the Size/Pos and Mix jacks arrive as normalized offsets.
void PstretchEngine::cv_voct(DeckRef::Ref d, float value)     { _cv_oct[(d == DeckRef::A) ? 0 : 1] = value * (1.f / 12.f); }
void PstretchEngine::cv_size_pos(DeckRef::Ref d, float value) { _cv_str[(d == DeckRef::A) ? 0 : 1] = value; }
void PstretchEngine::cv_mix(DeckRef::Ref d, float value)      { _cv_mix[(d == DeckRef::A) ? 0 : 1] = value; }
void PstretchEngine::cv_crossfade(float value)               { _cv_xfade = value; _recompute_xfade(); }

// Crossfade gains from the knob + its CV offset (clamped). v<=0.5 holds A at unity and fades B, and vice versa.
void PstretchEngine::_recompute_xfade() {
    float v = _xfade + _cv_xfade;
    if (v < 0.f) v = 0.f; else if (v > 1.f) v = 1.f;
    _gA = v <= 0.5f ? 1.f : 2.f * (1.f - v);
    _gB = v >= 0.5f ? 1.f : 2.f * v;
}

// Mod CV out: emit each deck's LFO (0..1) as a control voltage. Block-constant is fine for the sub-10 Hz
// LFO; the platform also feeds the last value to the Cycle LED brightness. Free-runs regardless of internal
// depth, so pstretch is a usable LFO/CV source even when it is not modulating itself.
void PstretchEngine::process_cv(float* cv0, float* cv1, size_t n) {
    for (size_t i = 0; i < n; i++) { cv0[i] = _lfo_out[0]; cv1[i] = _lfo_out[1]; }
}

// Gate out: one pulse per LFO cycle (the platform owns the ~7 ms pulse width). Clock-synced when the LFO is
// (Alt+Cycle), so this is a tempo-locked clock/reset output. Latched in _derive_and_push, cleared on read.
bool PstretchEngine::gate_out_triggered(DeckRef::Ref d) {
    const int i = (d == DeckRef::A) ? 0 : 1;
    if (!_gate_out[i]) return false;
    _gate_out[i] = false;
    return true;
}

// Gate in (rising edge, per deck): in Capture, RE-GRAB the recent ring (rhythmic re-sampling from a clock);
// in Live/SD, toggle FREEZE (rhythmic hold/stutter). Same _frozen flag as the Play pad, so they stay in sync.
void PstretchEngine::on_gate_trigger(DeckRef::Ref d) {
    const int i = (d == DeckRef::A) ? 0 : 1;
    if (_source[i] == Source::Capture) {
        _voice[i].set_capture(false);
        _voice[i].set_capture(true);
    } else {
        _frozen[i] = !_frozen[i];
        _voice[i].set_freeze(_frozen[i]);
    }
}

// Panel Cycle-LED indicator (the own-display ring is drawn by render()): report the mod type + sync state so
// the platform lights the Cycle LED (clock-source colour when synced, mode colour when Follow). The mode
// field maps the source to a colour slot (Live->Reel, Capture->Slice, SD->Drift).
DeckLeds PstretchEngine::deck_leds(DeckRef::Ref d) {
    const int i = (d == DeckRef::A) ? 0 : 1;
    DeckLeds l{};
    l.mode = (_source[i] == Source::SD) ? Mode::Drift
           : (_source[i] == Source::Capture) ? Mode::Slice : Mode::Reel;
    l.mod_type   = _mod_follow[i] ? ModType::Follow : ModType::LFO;
    l.mod_synced = _mod_synced[i];
    return l;
}

// Main loop (FatFs is not ISR-safe, so this is never in process()): keep any SD deck that isn't streaming
// yet trying to find + open a clip, then service settled Alt+POS scrub re-seeks. Gating the retry on
// "SD-selected but not playing" self-heals the async card mount, a late-inserted card, an empty folder
// filled after boot, or Drift already selected at power-on - no fixed boot window needed, and it stops as
// soon as a clip is streaming.
void PstretchEngine::prepare() {
    if (_stream) {
        for (int i = 0; i < 2; i++) {
            const DeckRef::Ref d = (i == 0) ? DeckRef::A : DeckRef::B;
            if (_source[i] == Source::SD && !_stream->is_playing(d) && _nclips == 0) _rescan = true;
        }
        _ensure_scan();
        if (_nclips > 0) {
            for (int i = 0; i < 2; i++) {
                const DeckRef::Ref d = (i == 0) ? DeckRef::A : DeckRef::B;
                if (_source[i] == Source::SD && !_stream->is_playing(d)) _open_clip(i);
            }
        }
    }
    _apply_scrub();
}

void PstretchEngine::_ensure_scan() {
    if (_rescan && _stream) { _nclips = _stream->scan_bank(kClipDir, _clips, kMaxClips); _rescan = false; }
}

// Re-seek the stream playhead for any deck whose Alt+POS scrub has settled (the knob stopped moving for
// kScrubSettleMs). Only seeks when the target moved more than kScrubStep from the last opened position, so
// jitter and sub-step nudges don't thrash FatFs. A null time source (host) settles immediately.
void PstretchEngine::_apply_scrub() {
    const uint32_t now = _time ? _time->now_ms() : 0;
    for (int i = 0; i < 2; i++) {
        if (!_scrub_pending[i] || _source[i] != Source::SD) continue;
        if (_time && (now - _scrub_at[i]) < kScrubSettleMs) continue;     // still moving -> wait
        _scrub_pending[i] = false;
        if (std::fabs(_scrub_n[i] - _scrub_opened[i]) < kScrubStep) continue;   // negligible move
        if (_nclips <= 0) continue;
        int idx = _clip_sel[i];
        if (idx >= _nclips) idx = _nclips - 1;
        if (idx < 0) idx = 0;
        const uint32_t frames = _clips[idx].frames;
        uint32_t start = static_cast<uint32_t>(_scrub_n[i] * static_cast<float>(frames));
        if (frames > 0 && start >= frames) start = frames - 1;
        _open_clip(i, start);
        _scrub_opened[i] = _scrub_n[i];
    }
}

// Routing switch (global): 0=Stereo, 1=DoubleMono, 2=GenerativeStereo (mirrors granular/radio).
// Mode switch (per deck): the SOURCE selector - 0=Live, 1=Capture, 2=SD-file. Main-loop only, so the
// FatFs work (scan + open) reached via _set_source is safe here.
bool PstretchEngine::set_config(ConfigId id, DeckRef::Ref d, int value) {
    if (id == ConfigId::Route) {
        const Route r = (value == 2) ? Route::GenerativeStereo
                      : (value == 1) ? Route::DoubleMono
                                     : Route::Stereo;
        if (r != _route) { _route = r; if (_route == Route::GenerativeStereo) _roll_random_pans(); }
    } else if (id == ConfigId::Mode) {
        const int i = (d == DeckRef::A) ? 0 : 1;
        const Source s = (value == 2) ? Source::SD : (value == 1) ? Source::Capture : Source::Live;
        _set_source(i, s);
    } else if (id == ConfigId::ModType) {
        _mod_follow[(d == DeckRef::A) ? 0 : 1] = (value == 1);       // 0 = LFO, 1 = input follower
    } else if (id == ConfigId::LfoShape) {
        _lfo_shape[(d == DeckRef::A) ? 0 : 1] = (value == 1) ? 1 : 0; // 0 = sine, 1 = triangle
    } else if (id == ConfigId::StartModOn) {
        const int i = (d == DeckRef::A) ? 0 : 1; _mod_start_on[i] = (value != 0); _update_mod_target(i);
    } else if (id == ConfigId::SizeModOn) {
        const int i = (d == DeckRef::A) ? 0 : 1; _mod_size_on[i]  = (value != 0); _update_mod_target(i);
    }
    return false;
}

// Switch deck i's analysis-ring source. Exits the old source cleanly (stop the stream / drop capture),
// then enters the new one. Freeze is orthogonal and left untouched.
void PstretchEngine::_set_source(int i, Source s) {
    if (s == _source[i]) return;
    switch (_source[i]) {                              // exit the old source
        case Source::SD:      if (_stream) _stream->stop((i == 0) ? DeckRef::A : DeckRef::B);
                              _voice[i].set_sd(false); _voice[i].set_sd_rate(0.f); break;   // 0 -> ratio 1.0
        case Source::Capture: _voice[i].set_capture(false); break;
        case Source::Live:    break;
    }
    _source[i] = s;
    switch (s) {                                       // enter the new source
        case Source::SD:      _open_sd(i); break;
        case Source::Capture: _voice[i].set_capture(true); break;   // snapshot the recent ring, loop it
        case Source::Live:    break;                                // live write_input resumes (both flags off)
    }
}

// Arm SD on deck i: put the voice in SD mode (read head fed from the stream) and open the selected clip
// from its start. Reset the scrub baseline so the clip opens at frame 0 and the next Alt+POS move re-seeks.
void PstretchEngine::_open_sd(int i) {
    _voice[i].set_sd(true);
    _scrub_opened[i] = 0.f; _scrub_pending[i] = false;
    _rescan = true;          // re-scan on entry: the card may have mounted (or been swapped) since boot
    _open_clip(i, 0);        // _open_clip -> _ensure_scan picks up the rescan
}

// (Re)open deck i's currently selected clip from /pstretch at start_frame, looping. Stops any clip already
// playing on that deck and rewinds the voice's SD heads so it crawls the (re)opened file from start_frame.
void PstretchEngine::_open_clip(int i, uint32_t start_frame) {
    if (!_stream) return;
    _ensure_scan();
    if (_nclips <= 0) return;
    int idx = _clip_sel[i];
    if (idx >= _nclips) idx = _nclips - 1;
    if (idx < 0) idx = 0;
    const DeckRef::Ref d = (i == 0) ? DeckRef::A : DeckRef::B;
    _stream->stop(d);
    _build_path(_pathbuf[i], _clips[idx].name);
    if (_clips[idx].is_wav) _stream->start_play_wav(d, _pathbuf[i], start_frame, /*loop=*/true);
    else                    _stream->start_play_raw(d, _pathbuf[i], start_frame, /*loop=*/true);
    // Honour the clip's own sample rate (a .wav reports it; raw -> 0 -> assume the engine rate) so off-rate
    // clips play at native pitch instead of sharp/flat.
    _voice[i].set_sd_rate(_clips[idx].is_wav ? static_cast<float>(_clips[idx].rate) : 0.f);
    _voice[i].sd_rewind();
}

// "pstretch/<name>" into dst (kPathMax). Relative path (no leading slash), like the radio engine.
void PstretchEngine::_build_path(char* dst, const char* name) const {
    int p = 0;
    for (const char* s = kClipDir; *s && p < kPathMax - 1; s++) dst[p++] = *s;
    if (p < kPathMax - 1) dst[p++] = '/';
    for (const char* s = name; *s && p < kPathMax - 1; s++) dst[p++] = *s;
    dst[p] = '\0';
}

// Play pad toggles FREEZE (stop the read head; works in any source). Rev pad, in Capture mode, RE-GRABS
// the recent ring at the current instant (the Mode-switch position itself grabs on entry; the pad refreshes
// it) - a no-op in Live/SD, where the source is the switch's job. Per deck. Returns false (effect, no LED).
bool PstretchEngine::on_play_pad(DeckRef::Ref d, bool reverse) {
    const int i = (d == DeckRef::A) ? 0 : 1;
    if (!reverse) { _frozen[i] = !_frozen[i]; _voice[i].set_freeze(_frozen[i]); }
    else if (_source[i] == Source::Capture) { _voice[i].set_capture(false); _voice[i].set_capture(true); }
    return false;
}

void PstretchEngine::render(DisplayModel& m) {
    m.clear();
    for (DeckRef::Ref dk : { DeckRef::A, DeckRef::B }) {
        const int i = (dk == DeckRef::A) ? 0 : 1;
        // State colour: frozen = cyan (held drone) takes priority; else by source - SD = magenta (streaming
        // a clip) or RED if SD is selected but no clips were found on the card, Capture = amber (looping a
        // grab), Live = green.
        const uint32_t c = _frozen[i] ? 0x00ffffu
                         : (_source[i] == Source::SD ? (_nclips > 0 ? pal::kPink : pal::kRed)
                         : (_source[i] == Source::Capture ? 0xff8000u : pal::kGreen));
        m.play[i] = { c, 0.7f };
        m.ring[i].set_hex_color(0x101010); m.ring[i].set_segment(0.f, 0.999f);
        if (_aux_held[i] && _source[i] == Source::SD && _nclips > 0) {
            // Alt held on a streaming deck: clip selector - a dot per clip, the selected one bright.
            ring::selector(m.ring[i], _nclips, _clip_sel[i], pal::kPink, 0.25f);
        } else {
            // Otherwise a marker whose position tracks the stretch amount, in the state colour (brighter held).
            ring::playhead(m.ring[i], _stretch_n[i],
                           (_frozen[i] || _source[i] != Source::Live) ? 1.f : 0.6f, c);
        }
        m.ring[i].set_updated();
    }
    led::route_leds(m, _route);
}

void PstretchEngine::_roll_random_pans() {
    for (int i = 0; i < 2; i++) {
        _rng = _rng * 1664525u + 1013904223u;
        const float p = static_cast<float>(_rng >> 8) * (1.f / 16777216.f);   // [0,1)
        _rndL[i] = std::cos(p * 1.57079632679f);
        _rndR[i] = std::sin(p * 1.57079632679f);
    }
}

} // namespace spotykach
