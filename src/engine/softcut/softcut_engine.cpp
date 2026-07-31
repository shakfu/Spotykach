#include "engine/softcut/softcut_engine.h"
#include "engine/arena.h"
#include "engine/indicators.h"   // shared indicator toolkit (docs/dev/indicator-comparison.md §7)

#include <cmath>
#include <cstring>

#ifdef METER
#include "meter.h"
#endif

namespace spotykach {

namespace {
// daisysp::SoftLimit's cubic soft-clip, inlined to avoid a heavy include. Near-identity for small |x|,
// tames the overshoot when overdub feedback + filter resonance push the bus past 0 dBFS.
inline float soft(float x) {
    x = x < -3.f ? -3.f : (x > 3.f ? 3.f : x);
    return x * (27.f + x * x) / (27.f + 9.f * x * x);
}
inline float clamp1(float x) { return x < -1.f ? -1.f : (x > 1.f ? 1.f : x); }
}

// Bipolar capstan-rate map (shared shape with the shuttle engine). Knob splits at noon (v=0.5): a small
// deadzone -> exact 0 (guaranteed stop), then a linear ramp to +/-kMaxSpeed. CW forward, CCW reverse.
float SoftcutEngine::rate_from_knob(float v) {
    const float c = v - 0.5f;
    const float a = std::fabs(c);
    if (a <= kDead) return 0.f;
    const float frac = (a - kDead) / (0.5f - kDead);
    const float s = frac * kMaxSpeed;
    return c < 0.f ? -s : s;
}

// Inverse for a positive (forward) rate - the knob value the Play pad snaps to, so param(Speed) reports
// a position consistent with the map and the PITCH pickup reseeds to it.
float SoftcutEngine::knob_for_rate(float s) {
    const float frac = s / kMaxSpeed;
    return 0.5f + (kDead + frac * (0.5f - kDead));
}

void SoftcutEngine::init(const EngineContext& ctx) {
    _stream = ctx.stream;
    _time   = ctx.time;
    _sr     = ctx.sample_rate;

    // One power-of-2 mono loop buffer per voice from the SDRAM arena (softcut's setVoiceBuffer model).
    Arena ar(ctx.arena);
    _cap = kBufFrames;
    for (int d = 0; d < 2; d++)
        for (int s = 0; s < kTracks; s++) {
            _buf[d][s] = ar.alloc<float>(kBufFrames);
            if (!_buf[d][s]) _cap = 0;                       // arena exhausted -> defensive
        }

    for (int d = 0; d < 2; d++)
        for (int s = 0; s < kTracks; s++) {
            softcut::Voice& v = _voice[d][s];
            v.reset();
            v.setSampleRate(_sr);
            if (_buf[d][s]) v.setBuffer(_buf[d][s], _cap);
            v.setLoopFlag(true);
            v.setRecLevel(1.0f);                             // overdub writes input fully; ENV = feedback
            v.setPreLevel(_fb_n[d][s]);
            v.setRecFlag(false);
            v.setPlayFlag(false);
            v.setRate(0.f);                                  // stopped at noon
            v.setFadeTime(0.001f + _fade_n[d][s] * kFadeMaxSec);
            v.setRateSlewTime(0.001f + _slew_n[d][s] * kSlewMaxSec);
            v.setPostFilterLp(1.0f);                         // post filter is a sweepable low-pass
            v.setPostFilterDry(0.0f);
            _apply_filter(d, s);
            _apply_window(d, s);
        }
    _recompute_blend();
    _recompute_pan();
}

float SoftcutEngine::_extent_sec(int i, int s) const {
    const uint32_t frames = (_len[i][s] > 0) ? _len[i][s] : _cap;
    return _sr > 0.f ? static_cast<float>(frames) / _sr : 0.f;
}

float SoftcutEngine::_loop_start_sec(int i, int s) const {
    const float ext = _extent_sec(i, s);
    float len = _size_n[i][s] * ext;
    if (len < kMinLoopSec) len = kMinLoopSec;
    if (len > ext)         len = ext;
    float start = _pos_n[i][s] * (ext - len);
    return start < 0.f ? 0.f : start;
}

// POS/SIZE -> softcut loop region (seconds). SIZE sets length (clamped to a minimum and the extent);
// POS slides the start across the part of the extent the window does not cover.
void SoftcutEngine::_apply_window(int i, int s) {
    const float ext = _extent_sec(i, s);
    float len = _size_n[i][s] * ext;
    if (len < kMinLoopSec) len = kMinLoopSec;
    if (len > ext)         len = ext;
    float start = _pos_n[i][s] * (ext - len);
    if (start < 0.f) start = 0.f;
    _voice[i][s].setLoopStart(start);
    _voice[i][s].setLoopEnd(start + len);
}

// flux-pad post filter: cutoff (exp Hz) and resonance (softcut Rq = reciprocal-Q, so smaller = more
// resonant). At cutoff fully open (~20 kHz) with no resonance the low-pass is near-transparent.
void SoftcutEngine::_apply_filter(int i, int s) {
    const float fc = kFcMin * std::pow(kFcSpan, _cut_n[i][s]);
    const float rq = kRqOpen * std::pow(kRqSpan, _res_n[i][s]);
    _voice[i][s].setPostFilterFc(fc);
    _voice[i][s].setPostFilterRq(rq);
}

void SoftcutEngine::_request_load(DeckRef::Ref d, int s) {
    if (!_stream || !_cap) return;
    const int i = idx(d);
    const uint32_t now = _time ? _time->now_ms() : 0;
    if (_stream->start_play(d, _path(d, _tape_slot[i][s]))) {
        _load[i][s] = Load::Priming; _wpos[i][s] = 0; _len[i][s] = 0;
        _overdub[i][s] = false; _voice[i][s].setRecFlag(false);   // don't overwrite the clip being loaded
    } else {
        _err_until[i] = now + kErrFlashMs;
    }
}

// Main-loop housekeeping: boot preload + advance each track's SD->RAM load (mirrors the shuttle engine;
// the FatFs-touching stream calls must run here, not in the audio ISR). Loading writes in place into the
// voice's existing buffer, so softcut picks up the new content with no re-point - we only re-apply the
// loop window (the extent grew) and cut the head to the new loop start.
void SoftcutEngine::prepare() {
    if (!_stream) return;

    // Finalize any completed save: the ISR has pushed the whole loop, so stop() patches the WAV header
    // (the record tail drains over the next stream.process() passes) and the slot is refreshed.
    for (int dd = 0; dd < 2; dd++) {
        if (_save[dd] == Save::Finalize) {
            _stream->stop((dd == 0) ? DeckRef::A : DeckRef::B);
            _save[dd] = Save::Idle; _save_voice[dd] = -1; _rescan[dd] = true;
        }
    }

    // Commit a deferred Alt+Seq full-save once its hold window has passed (no erase cancelled it).
    {
        const uint32_t now = _time ? _time->now_ms() : 0;
        for (int dd = 0; dd < 2; dd++)
            if (_save_arm[dd] && now - _save_arm_ms[dd] >= kSaveArmMs) {
                _save_arm[dd] = false;
                _start_save((dd == 0) ? DeckRef::A : DeckRef::B, /*full=*/true);
            }
    }

    if (_preload_armed) {
        if (!_preload_mounted) {
            for (DeckRef::Ref d : { DeckRef::A, DeckRef::B })
                for (int s = 0; s < kTracks && !_preload_mounted; s++)
                    if (_stream->exists(_path(d, s))) _preload_mounted = true;
            if (!_preload_mounted && _time && _time->now_ms() > kPreloadDeadlineMs)
                _preload_armed = false;
        }
        if (_preload_mounted) {
            for (DeckRef::Ref d : { DeckRef::A, DeckRef::B }) {
                const int i = idx(d);
                bool busy = false;
                for (int s = 0; s < kTracks; s++) busy = busy || (_load[i][s] != Load::Idle);
                if (_preload_next[i] < kTracks && !busy) {
                    const int s = _preload_next[i]++;
                    if (_len[i][s] == 0 && _stream->exists(_path(d, s))) {
                        _tape_slot[i][s] = s;
                        _request_load(d, s);
                    }
                }
            }
            if (_preload_next[0] >= kTracks && _preload_next[1] >= kTracks) _preload_armed = false;
        }
    }

    for (DeckRef::Ref d : { DeckRef::A, DeckRef::B }) {
        const int i = idx(d);
        if (_rescan[i]) { _rescan[i] = false; for (int s = 0; s < kTapeSlots; s++) _slot_used[i][s] = _stream->exists(_path(d, s)); }

        for (int s = 0; s < kTracks; s++) {
            if (_load[i][s] == Load::Priming) {
                const uint32_t target = _stream->loop_frames(d);
                if (target > 0)                   _load[i][s] = Load::Draining;
                else if (!_stream->is_playing(d)) _load[i][s] = Load::Idle;   // empty/failed file
            }
            if (_load[i][s] == Load::Draining) {
                const uint32_t target   = _stream->loop_frames(d);
                const uint32_t cap_left = (_cap > _wpos[i][s]) ? _cap - _wpos[i][s] : 0;
                uint32_t want = target - _wpos[i][s];
                if (want > kLoadChunk) want = kLoadChunk;
                if (want > cap_left)   want = cap_left;
                const uint32_t got = _stream->play_consume(
                    d, reinterpret_cast<uint8_t*>(_buf[i][s] + _wpos[i][s]), want * sizeof(float)) / sizeof(float);
                _wpos[i][s] += got;
                if (_wpos[i][s] >= target || _wpos[i][s] >= _cap) {
                    _len[i][s] = _wpos[i][s]; _stream->stop(d); _load[i][s] = Load::Idle;
                    _apply_window(i, s);
                    _voice[i][s].cutToPos(_loop_start_sec(i, s));    // play the loaded clip from its start
                }
            }
        }
    }
}

void SoftcutEngine::process(const float* const* in, float** out, size_t size) {
    const size_t n = size > kMaxFrames ? kMaxFrames : size;

#ifdef SOFTCUT_DIAG_PASSTHROUGH
    // Diagnostic build (make ENGINE=softcut SOFTCUT_EXTRA=-DSOFTCUT_DIAG_PASSTHROUGH): bypass softcut and
    // ALL gating (crossfade, MIX, overdub gate, pan) - sum both inputs to both outputs unconditionally.
    // Audio heard -> the current patch reaches the engine input and the silence is engine gating/logic;
    // silent -> input is not arriving (jack/gain/source). Channel-agnostic so it works on either input.
    for (size_t i = 0; i < size; i++) { const float m = in ? (in[0][i] + in[1][i]) : 0.f; out[0][i] = m; out[1][i] = m; }
    return;
#endif
#ifdef SOFTCUT_DIAG_TONE
    // Diagnostic build (SOFTCUT_EXTRA=-DSOFTCUT_DIAG_TONE): ignore input, emit a 440 Hz tone. Tests ONLY
    // the output/DAC/headphone path. Tone audible -> output works, so the earlier silence is INPUT not
    // arriving. Tone silent -> the output/monitoring path is dead (and no engine would be audible).
    static float ph = 0.f;
    const float inc = 6.2831853f * 440.f / _sr;
    for (size_t i = 0; i < size; i++) {
        const float s = 0.2f * std::sin(ph);
        ph += inc; if (ph > 6.2831853f) ph -= 6.2831853f;
        out[0][i] = s; out[1][i] = s;
    }
    return;
#endif

    // Mono record source = sum of both input channels, so a loop captures the signal no matter which
    // input jack it is patched to (the module's audio input may land on either channel). Every voice
    // records and monitors this same mono mix; the stereo image is created on the OUTPUT side by the
    // per-voice pan/route, not by which input channel a deck listens to.
    float inmix[kMaxFrames];
    for (size_t k = 0; k < n; k++) inmix[k] = in ? (in[0][k] + in[1][k]) : 0.f;

    // Each voice -> a mono block. softcut's head does play + overdub + crossfade + filters internally.
    float mono[2][kTracks][kMaxFrames];
    bool  any_overdub = false;
    for (int d = 0; d < 2; d++) {
        for (int s = 0; s < kTracks; s++) {
            _voice[d][s].processBlockMono(inmix, mono[d][s], static_cast<int>(n));
            any_overdub = any_overdub || _overdub[d][s] || _defining[d][s];
        }
    }

    // Per-voice bus gain = MIX volume x A/B blend x route-dependent pan, computed once per block.
    float gL[2][kTracks], gR[2][kTracks];
    for (int s = 0; s < kTracks; s++) {
        gL[0][s] = _gA * _gain[0][s] * _panL[0][s]; gR[0][s] = _gA * _gain[0][s] * _panR[0][s];
        gL[1][s] = _gB * _gain[1][s] * _panL[1][s]; gR[1][s] = _gB * _gain[1][s] * _panR[1][s];
    }

    // Live input monitoring while overdubbing. softcut's output is the loop read-head, NOT a pass-through
    // of the input, so without this you hear nothing of what you are laying down until the loop wraps
    // (up to the whole buffer with a long SIZE). Monitor the mono input mix (at the deck blend) whenever
    // a deck's focused voice is recording, so a looper monitors like a tape while you record.
    const bool  recA = _overdub[0][_active[0]] || _defining[0][_active[0]];
    const bool  recB = _overdub[1][_active[1]] || _defining[1][_active[1]];
    const float monA = recA ? _gA * kMonGain : 0.f;
    const float monB = recB ? _gB * kMonGain : 0.f;
    const float monSum = monA + monB;

    // Sum the panned voices (+ dry monitor). Soft-clip when any voice is overdubbing (feedback/resonance
    // and the monitor can build past 0 dBFS); otherwise a plain clamp keeps clean playback transparent.
    for (size_t k = 0; k < n; k++) {
        float mon = monSum * inmix[k];
        float l = mon, r = mon;
        for (int d = 0; d < 2; d++)
            for (int s = 0; s < kTracks; s++) { l += mono[d][s][k] * gL[d][s]; r += mono[d][s][k] * gR[d][s]; }
        if (any_overdub) { out[0][k] = soft(l); out[1][k] = soft(r); }
        else             { out[0][k] = clamp1(l); out[1][k] = clamp1(r); }
    }
    // kMaxFrames covers the platform block (256 == block_size today). If a larger block is ever
    // configured, only [0,n) was written above - zero any tail so the output is silence, never garbage.
    for (size_t k = n; k < size; k++) { out[0][k] = 0.f; out[1][k] = 0.f; }

    // SD save: push the focused loop's frames into the per-deck record ring (the main loop drains them
    // to the card). Honor the ring's accepted byte count and resume next block - never block the ISR.
    for (int dd = 0; dd < 2; dd++) {
        if (_save[dd] != Save::Writing || _save_voice[dd] < 0 || !_stream) continue;
        const int sv = _save_voice[dd];
        const uint32_t remain = _save_end[dd] - _save_pos[dd];
        const uint32_t want   = remain < kSaveChunk ? remain : kSaveChunk;
        const DeckRef::Ref dref = (dd == 0) ? DeckRef::A : DeckRef::B;
        const uint32_t got = _stream->record_produce(
            dref, reinterpret_cast<const uint8_t*>(_buf[dd][sv] + _save_pos[dd]),
            want * sizeof(float)) / sizeof(float);
        _save_pos[dd] += got;
        if (_save_pos[dd] >= _save_end[dd]) _save[dd] = Save::Finalize;   // main loop will stop()/finalize
    }
}

void SoftcutEngine::set_param(ParamId id, DeckRef::Ref d, float v) {
    const int i = idx(d);
    const int s = _active[i];                                 // knobs address the FOCUSED track
    switch (id) {
        // Freeze the RECORD rate while a fresh take is defining (as POS/SIZE below are): the take then
        // always records at 1x, so its length is wall-clock time, not integrated rate. The knob value is
        // cached and the closed loop snaps back to unity in on_record_pad.
        case ParamId::Speed:  _rate_n[i][s] = v; if (!_defining[i][s]) _voice[i][s].setRate(rate_from_knob(v)); break;
        // While a fresh take is recording (defining), the loop is held open at the full buffer so the
        // take can run the whole 10.9 s - don't let the live POS/SIZE knobs reshape it mid-record (they
        // would cap the take early). The knob values are still cached and applied once the loop closes.
        case ParamId::Pos:    _pos_n[i][s]  = v; if (!_defining[i][s]) _apply_window(i, s); break;
        case ParamId::Size:   _size_n[i][s] = v; if (!_defining[i][s]) _apply_window(i, s); break;
        case ParamId::Mix:    _gain[i][s]   = v; break;
        case ParamId::Env:    _fb_n[i][s]   = v; _voice[i][s].setPreLevel(v); break;   // overdub feedback
        case ParamId::AltPos: _pan[i][s]    = v; _recompute_pan(); break;
        case ParamId::Crossfade: _xfade = v; _recompute_blend(); break;
        case ParamId::ModAmp: _fade_n[i][s] = v; _voice[i][s].setFadeTime(0.001f + v * kFadeMaxSec); break;
        case ParamId::FluxIntensity: _cut_n[i][s] = v; _apply_filter(i, s); break;     // flux+PITCH cutoff
        case ParamId::FluxMix:       _res_n[i][s] = v; _apply_filter(i, s); break;     // flux+MIX  reso
        case ParamId::Aux: {
            const int sl = static_cast<int>(v * kTapeSlots);
            const int ns = sl < 0 ? 0 : (sl >= kTapeSlots ? kTapeSlots - 1 : sl);
            if (ns != _tape_slot[i][s]) { _tape_slot[i][s] = ns; _request_load(d, s); }
            break;
        }
        default: break;
    }
}

float SoftcutEngine::param(ParamId id, DeckRef::Ref d) const {
    const int i = idx(d);
    const int s = _active[i];
    switch (id) {
        case ParamId::Speed:  return _rate_n[i][s];
        case ParamId::Pos:    return _pos_n[i][s];
        case ParamId::Size:   return _size_n[i][s];
        case ParamId::Mix:    return _gain[i][s];
        case ParamId::Env:    return _fb_n[i][s];
        case ParamId::AltPos: return _pan[i][s];
        case ParamId::ModAmp: return _fade_n[i][s];
        case ParamId::ModSpeed: return _slew_n[i][s];
        case ParamId::FluxIntensity: return _cut_n[i][s];
        case ParamId::FluxMix:       return _res_n[i][s];
        case ParamId::Aux:    return (static_cast<float>(_tape_slot[i][s]) + 0.5f) / static_cast<float>(kTapeSlots);
        default: return 0.f;
    }
}

void SoftcutEngine::set_mod_speed(DeckRef::Ref d, float v, bool /*sync*/) {
    const int i = idx(d);
    const int s = _active[i];
    _slew_n[i][s] = v;
    _voice[i][s].setRateSlewTime(0.001f + v * kSlewMaxSec);
}

void SoftcutEngine::set_aux_active(DeckRef::Ref d, bool held) {
    const int i = idx(d);
    if (held && !_aux_held[i]) _rescan[i] = true;
    _aux_held[i] = held;
}

bool SoftcutEngine::take_param_reseed(DeckRef::Ref d) {
    const int i = idx(d);
    const bool f = _want_reseed[i];
    _want_reseed[i] = false;
    return f;
}

// Rev (reverse=true): swap the deck's focused track + reseed the pickup. Plain Play: toggle the focused
// track's loop, snapping rate to unity on engage; stopping also disarms overdub.
bool SoftcutEngine::on_play_pad(DeckRef::Ref d, bool reverse) {
    const int i = idx(d);
    const uint32_t now = _time ? _time->now_ms() : 0;
    if (_time && now - _last_trig_ms[i] < kDebounceMs) return false;
    _last_trig_ms[i] = now;

    if (reverse) {
        _active[i] ^= 1;
        _want_reseed[i] = true;
        _swap_show[i] = 60;
        return false;
    }

    const int s = _active[i];
    _err_until[i] = 0;
    if (_defining[i][s]) return false;                   // a fresh take is closed with Alt+Play, not Play
    if (!_rolling[i][s]) {
        _rolling[i][s] = true; _voice[i][s].setPlayFlag(true);
        _rate_n[i][s] = knob_for_rate(1.f); _voice[i][s].setRate(1.f); _want_reseed[i] = true;
        _voice[i][s].cutToPos(_loop_start_sec(i, s));    // kick the head into motion (fade in from start)
    } else {
        _rolling[i][s] = false; _voice[i][s].setPlayFlag(false);
        if (_overdub[i][s]) { _overdub[i][s] = false; _voice[i][s].setRecFlag(false); }
    }
    return false;
}

// Alt+Play: the record gesture. On an EMPTY voice it works like a looper - the first press records a
// fresh loop, the second press closes the loop at exactly the length you played and starts looping it.
// On a voice that already has content (SD-loaded or previously recorded) it arms/disarms OVERDUB on
// top. softcut needs a defined, head-aligned loop to record into, so a fresh take must size its own loop
// rather than overdub into an undefined window (the empty-buffer record bug).
void SoftcutEngine::on_record_pad(DeckRef::Ref d, bool reverse) {
    if (reverse) { _start_save(d, /*full=*/false); return; }   // Alt+Rev: save the trimmed loop window
    const int i = idx(d);
    const int s = _active[i];
    const uint32_t now = _time ? _time->now_ms() : 0;
    if (_time && now - _last_trig_ms[i] < kDebounceMs) return;
    _last_trig_ms[i] = now;

    if (_defining[i][s]) {
        // Close the fresh loop: its length is how far the record head has travelled. Snap the loop to
        // exactly that (full SIZE, POS 0) and keep rolling with overdub off, so it plays back at once.
        float lenSec = _voice[i][s].getSavedPosition();
        const float maxSec = static_cast<float>(_cap) / _sr;
        if (lenSec < kMinLoopSec) lenSec = kMinLoopSec;
        if (lenSec > maxSec)      lenSec = maxSec;
        _len[i][s]     = static_cast<uint32_t>(lenSec * _sr);
        _defining[i][s] = false;
        _voice[i][s].setRecFlag(false);
        _size_n[i][s] = 1.f; _pos_n[i][s] = 0.f;             // the loop IS the take
        _apply_window(i, s);
        _voice[i][s].cutToPos(0.f);
        _rate_n[i][s] = knob_for_rate(1.f); _voice[i][s].setRate(1.f);  // closed take plays at unity (record was 1x-locked)
        _want_reseed[i] = true;                              // reseed POS/SIZE + PITCH pickups to the new loop
        return;
    }
    if (_overdub[i][s]) {                                    // overdub disarm
        _overdub[i][s] = false; _voice[i][s].setRecFlag(false);
        return;
    }
    if (_len[i][s] == 0) {
        // Fresh take: clear the buffer and record forward over an open loop (the whole buffer) so the
        // length is whatever you play; the next press closes it. Clearing happens on the main loop, and
        // the voice was stopped/empty, so there is no ISR read to race.
        if (_buf[i][s]) std::memset(_buf[i][s], 0, _cap * sizeof(float));
        _rolling[i][s] = true; _voice[i][s].setPlayFlag(true);
        _rate_n[i][s] = knob_for_rate(1.f); _voice[i][s].setRate(1.f); _want_reseed[i] = true;
        _voice[i][s].setLoopStart(0.f);
        _voice[i][s].setLoopEnd(static_cast<float>(_cap) / _sr);   // open loop: no wrap mid-take
        _voice[i][s].cutToPos(0.f);
        _voice[i][s].setRecFlag(true);
        _defining[i][s] = true;
        return;
    }
    // Voice already has content -> overdub onto it (auto-start rolling if stopped).
    if (!_rolling[i][s]) {
        _rolling[i][s] = true; _voice[i][s].setPlayFlag(true);
        _rate_n[i][s] = knob_for_rate(1.f); _voice[i][s].setRate(1.f); _want_reseed[i] = true;
        _voice[i][s].cutToPos(_loop_start_sec(i, s));
    }
    _overdub[i][s] = true; _voice[i][s].setRecFlag(true);
}

// Seq pad: realign ALL voices to their loop start at once - softcut's cutToPos does a click-free
// crossfaded jump, so drifted free-running loops snap back to a common downbeat (the v1 sync gesture).
// Deck-agnostic (either Seq pad does the global realign).
void SoftcutEngine::on_seq_trigger(DeckRef::Ref /*d*/) {
    for (int d = 0; d < 2; d++)
        for (int s = 0; s < kTracks; s++)
            if (_rolling[d][s]) _voice[d][s].cutToPos(_loop_start_sec(d, s));
}

// Save the focused voice's loop to its currently-selected SD slot (float32 mono WAV, the format load
// reads). `full` = the whole recorded take [0,_len]; else the POS/SIZE loop window (what you hear).
// Streamed via the platform record path: here we open the file and arm the write; process() (ISR)
// pushes the frames into the record ring, the main loop drains them to the card, prepare() finalizes.
// Pick the destination with Alt+PITCH first.
void SoftcutEngine::_start_save(DeckRef::Ref d, bool full) {
    const int i = idx(d);
    const int s = _active[i];
    const uint32_t now = _time ? _time->now_ms() : 0;
    if (!_stream || !_cap) return;
    if (_save[i] != Save::Idle || _load[i][0] != Load::Idle || _load[i][1] != Load::Idle) return; // deck busy
    uint32_t start, end;
    if (full) {
        start = 0; end = _len[i][s];                                          // the whole take
    } else {
        start = static_cast<uint32_t>(_loop_start_sec(i, s) * _sr);
        end   = start + static_cast<uint32_t>(loop_len_sec(d, s) * _sr);      // the POS/SIZE window
    }
    if (_len[i][s] == 0 || end <= start || end > _cap) { _err_until[i] = now + kErrFlashMs; return; }
    if (_stream->start_record(d, _path(d, _tape_slot[i][s]))) {
        if (_overdub[i][s]) { _overdub[i][s] = false; _voice[i][s].setRecFlag(false); } // clean snapshot
        _save[i] = Save::Writing; _save_voice[i] = s; _save_pos[i] = start; _save_end[i] = end;
    } else {
        _err_until[i] = now + kErrFlashMs;
    }
}

// Alt+Seq (tap): arm a DEFERRED save of the whole take. prepare() commits it once the ~1.5s hold
// window has elapsed without an erase (Option B), so holding Alt+Seq cancels it and erases instead.
void SoftcutEngine::on_seq_toggle_arm(DeckRef::Ref d) {
    if (!_stream) return;
    const int i = idx(d);
    _save_arm[i]    = true;
    _save_arm_ms[i] = _time ? _time->now_ms() : 0;
}

// Alt+Seq (hold ~1.5s): clean-discard erase of the focused voice. Cancels the pending tap-save and
// empties the buffer so the next Alt+Play records a fresh take. Runs on the main loop (UI tick), so the
// buffer clear is safe; setPlayFlag(false) stops the audio read within a block (a tiny click on wipe is
// fine). _len = 0 marks the voice empty.
void SoftcutEngine::clear_sequence(DeckRef::Ref d) {
    const int i = idx(d);
    const int s = _active[i];
    _save_arm[i] = false;                                   // a hold cancels the pending tap-save
    _rolling[i][s] = false; _overdub[i][s] = false; _defining[i][s] = false;
    _voice[i][s].setPlayFlag(false); _voice[i][s].setRecFlag(false); _voice[i][s].stop();
    _len[i][s] = 0; _wpos[i][s] = 0;
    if (_buf[i][s]) std::memset(_buf[i][s], 0, _cap * sizeof(float));
    _pos_n[i][s] = 0.f; _size_n[i][s] = 0.5f;               // back to the boot loop window
    _apply_window(i, s);
    _want_reseed[i] = true;
}

void SoftcutEngine::_recompute_blend() {
    _gA = _xfade <= 0.5f ? 1.f : 2.f * (1.f - _xfade);
    _gB = _xfade >= 0.5f ? 1.f : 2.f * _xfade;
}

bool SoftcutEngine::set_config(ConfigId id, DeckRef::Ref, int value) {
    if (id == ConfigId::Route) {
        const Route r = (value == 2) ? Route::GenerativeStereo
                      : (value == 1) ? Route::DoubleMono
                                     : Route::Stereo;
        if (r != _route) {
            _route = r;
            if (_route == Route::GenerativeStereo) _roll_random_pans();
            else                                   _recompute_pan();
        }
    }
    return false;
}

// Resolve each voice's pan to equal-power L/R gains per the routing switch: DoubleMono = manual Alt+POS
// pan; Stereo = auto-spread (a deck's voices fan across L..R); GenerativeStereo = random positions.
void SoftcutEngine::_recompute_pan() {
    for (int i = 0; i < 2; i++) {
        for (int s = 0; s < kTracks; s++) {
            float p;
            switch (_route) {
                case Route::Stereo:           p = (kTracks > 1) ? static_cast<float>(s) / (kTracks - 1) : 0.5f; break;
                case Route::GenerativeStereo: p = _rnd[i][s]; break;
                default:                      p = _pan[i][s]; break;   // DoubleMono
            }
            _panL[i][s] = std::cos(p * kHalfPi);
            _panR[i][s] = std::sin(p * kHalfPi);
        }
    }
}

void SoftcutEngine::_roll_random_pans() {
    for (int i = 0; i < 2; i++) {
        for (int s = 0; s < kTracks; s++) {
            _rng = _rng * 1664525u + 1013904223u;
            _rnd[i][s] = static_cast<float>(_rng >> 8) * (1.f / 16777216.f);
        }
    }
    _recompute_pan();
}

static constexpr uint32_t kTrackHue[SoftcutEngine::kTracks] = {
    0x7c4dff,   // track 1: indigo
    0xffc000,   // track 2: amber
};

void SoftcutEngine::render(DisplayModel& m) {
    m.clear();
    const uint32_t now     = _time ? _time->now_ms() : 0;
    const float    breathe = motion::breathe_standby(now);   // 0.35..0.60 "loaded & ready" pulse

    for (DeckRef::Ref dk : { DeckRef::A, DeckRef::B }) {
        const int  i   = idx(dk);
        const int  s   = _active[i];
        if (_save[i] != Save::Idle) {              // saving a loop to SD: solid amber ring + indicator
            m.play[i] = { pal::kAmber, 1.f };
            ring::level(m.ring[i], 0.999f, pal::kAmber);
            m.ring[i].set_updated();
            continue;
        }
        const bool  err = _time && now < _err_until[i];
        const float rr  = rate_from_knob(_rate_n[i][s]);
        const bool  rec = _overdub[i][s] || _defining[i][s];
        // Direction-coded transport of the focused track: overdub red, forward green, reverse cyan,
        // engaged-but-frozen dim white, rejected amber, idle -> the track hue breathing (standby).
        const auto tv = transport_view(_rolling[i][s], rec, rr, err, kTrackHue[s], breathe * 0.5f);
        led::transport(m, i, tv);

        if (_aux_held[i]) {
            // Alt+PITCH held: SD slot selector for the focused track (selected bright, used mid, empty dim).
            uint32_t used = 0;
            for (int t = 0; t < kTapeSlots; t++) if (_slot_used[i][t]) used |= (1u << t);
            ring::slots(m.ring[i], kTapeSlots, _tape_slot[i][s], used);
        } else if (_swap_show[i] > 0) {
            _swap_show[i]--;
            m.ring[i].set_brightness(1.f);
            m.ring[i].set_hex_color(kTrackHue[s]); m.ring[i].set_segment(0.f, 0.999f);
            m.ring[i].set_point_hex_color(0xffffff);
            for (int t = 0; t <= s; t++) m.ring[i].set_point(static_cast<uint8_t>(t * 4), 1.f);
        } else {
            const uint32_t back = tv.live ? tv.rgb : kTrackHue[s];
            ring::level(m.ring[i], 0.999f, back, tv.live ? 1.f : breathe);   // full-ring transport/idle picture
            // Read-position dot, mapped into the loop window (softcut reports head position in seconds).
            const float ext = _extent_sec(i, s);
            if (ext > 0.f) {
                const float st  = _loop_start_sec(i, s);
                float len = _size_n[i][s] * ext; if (len < kMinLoopSec) len = kMinLoopSec; if (len > ext) len = ext;
                const float frac = (len > 0.f) ? (_voice[i][s].getSavedPosition() - st) / len : 0.f;
                ring::playhead(m.ring[i], frac, tv.live ? 1.f : 0.55f);
            }
        }
        m.ring[i].set_updated();
    }
    led::route_leds(m, _route);
}

const char* SoftcutEngine::_path(DeckRef::Ref d, int slot) {
    char* p = _pbuf;
    for (const char* s = "softcut/loop_"; *s; ) *p++ = *s++;
    *p++ = (d == DeckRef::A) ? 'a' : 'b';
    *p++ = '_';
    *p++ = static_cast<char>('1' + slot);
    for (const char* s = ".wav"; *s; ) *p++ = *s++;
    *p = '\0';
    return _pbuf;
}

int   SoftcutEngine::active_track(DeckRef::Ref d) const { return _active[idx(d)]; }
bool  SoftcutEngine::is_rolling(DeckRef::Ref d, int t) const { return _rolling[idx(d)][t]; }
bool  SoftcutEngine::is_overdubbing(DeckRef::Ref d, int t) const { return _overdub[idx(d)][t] || _defining[idx(d)][t]; }
bool  SoftcutEngine::is_saving(DeckRef::Ref d) const { return _save[idx(d)] != Save::Idle; }
bool  SoftcutEngine::is_empty(DeckRef::Ref d, int t) const { return _len[idx(d)][t] == 0; }
float SoftcutEngine::track_rate(DeckRef::Ref d, int t) const { return rate_from_knob(_rate_n[idx(d)][t]); }
float SoftcutEngine::loop_start_sec(DeckRef::Ref d, int t) const { return _loop_start_sec(idx(d), t); }
float SoftcutEngine::loop_len_sec(DeckRef::Ref d, int t) const {
    const int i = idx(d);
    const float ext = _extent_sec(i, t);
    float len = _size_n[i][t] * ext; if (len < kMinLoopSec) len = kMinLoopSec; if (len > ext) len = ext;
    return len;
}

} // namespace spotykach
