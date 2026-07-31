#include "engine/shuttle/shuttle_engine.h"
#include "engine/arena.h"
#include "engine/indicators.h" // shared indicator toolkit (docs/dev/indicator-grammar.md §8)
#include "engine/tape/tapefx.h" // shared TapeFx wrapper around the cyfaust-generated tfx_tapefx::mydsp

#include <cmath>
#include <cstring>
#include <new> // placement new

namespace spotykach {

// Bipolar capstan-speed map. The knob splits at noon (v=0.5): a small deadzone about centre maps to an
// exact 0 (a guaranteed full stop -> silence), and outside it the speed ramps LINEARLY from 0 at the
// deadzone edge to +/-kMaxSpeed at the extremes. Forward (CW, v>0.5) positive, reverse (CCW) negative.
float ShuttleEngine::speed_from_knob(float v) {
    const float c = v - 0.5f;
    const float a = std::fabs(c);
    if (a <= kDead) return 0.f;
    const float frac = (a - kDead) / (0.5f - kDead);     // 0 at the deadzone edge -> 1 at full throw
    const float s = frac * kMaxSpeed;
    return c < 0.f ? -s : s;
}

// Inverse for a positive (forward) speed - the knob value the Play pad snaps to, so param(Speed)
// reports a position consistent with the map and the pot pickup reseeds to it.
float ShuttleEngine::knob_for_speed(float s) {
    const float frac = s / kMaxSpeed;
    return 0.5f + (kDead + frac * (0.5f - kDead));
}

void ShuttleEngine::init(const EngineContext& ctx) {
    _stream = ctx.stream;
    _time   = ctx.time;
    _cap    = static_cast<uint32_t>(kBufSeconds * ctx.sample_rate);
    Arena ar(ctx.arena);
    for (int d = 0; d < 2; d++)
        for (int s = 0; s < kTracks; s++) {
            _buf[d][s] = ar.alloc<float>(_cap);
            if (!_buf[d][s]) _cap = 0;                // arena exhausted -> no tape (defensive)
        }
    // One tape-FX kernel per track (4 total) in the SDRAM arena (each ~16 KB of delay-line state).
    const int sr = static_cast<int>(ctx.sample_rate);
    for (int d = 0; d < 2; d++)
        for (int s = 0; s < kTracks; s++)
            if (void* m = ar.alloc<uint8_t>(sizeof(TapeFx), alignof(TapeFx))) {
                _fx[d][s] = new (m) TapeFx();
                _fx[d][s]->init(sr);
                for (int p = 0; p < 6; p++) _fx[d][s]->set(p, _fx_n[d][s][p]); // seed from cached defaults
            }
    _recompute_blend();
    _recompute_pan();
}

void ShuttleEngine::_request_load(DeckRef::Ref d, int s) {
    if (!_stream || !_cap) return;
    const int i = idx(d);
    const uint32_t now = _time ? _time->now_ms() : 0;
    if (_stream->start_play(d, _path(d, _tape_slot[i][s]))) {
        _load[i][s] = Load::Priming; _wpos[i][s] = 0; _len[i][s] = 0;
        _recording[i][s] = false; _rolling[i][s] = false; _read[i][s] = 0.0;
    } else {
        _err_until[i] = now + kErrFlashMs;
    }
}

// Main-loop housekeeping: refresh the slot cache and advance each track's SD->RAM load (the FatFs-
// touching stream calls must run here, not in the audio ISR). Only one stream exists, so loads are
// effectively serialized; a track whose load has not started yet simply waits a pass.
void ShuttleEngine::prepare() {
    if (!_stream) return;

    // Boot preload: fill ALL FOUR tracks from the card so a freshly powered shuttle is ready to jam
    // without a manual Alt+PITCH load (the tape engine streams slot 0 lazily on Play, but this RAM engine
    // must load up front). Each track t loads slot t's file (deck A: tape_a_1/_2.wav -> tracks 0/1; deck
    // B: tape_b_1/_2.wav). A deck's two tracks share one SD stream, so per deck the loads are serialized
    // (request the next track only when that deck's stream is free); the two decks load in parallel.
    // The card mounts cooperatively later in this same loop, so first wait for the volume to come up
    // (any successful exists() probe), retrying across passes; give up after a deadline if no card/files.
    if (_preload_armed) {
        if (!_preload_mounted) {
            for (DeckRef::Ref d : { DeckRef::A, DeckRef::B })
                for (int s = 0; s < kTracks && !_preload_mounted; s++)
                    if (_stream->exists(_path(d, s))) _preload_mounted = true;
            if (!_preload_mounted && _time && _time->now_ms() > kPreloadDeadlineMs)
                _preload_armed = false;                         // no card / no files within the boot window
        }
        if (_preload_mounted) {
            for (DeckRef::Ref d : { DeckRef::A, DeckRef::B }) {
                const int i = idx(d);
                const bool busy = _load[i][0] != Load::Idle || _load[i][1] != Load::Idle;
                if (_preload_next[i] < kTracks && !busy) {
                    const int s = _preload_next[i]++;           // advance whether we load or skip this track
                    if (_len[i][s] == 0 && _stream->exists(_path(d, s))) {
                        _tape_slot[i][s] = s;                   // track t <- slot t's file
                        _request_load(d, s);                    // -> Priming; the deck is now busy until it drains
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
                // The ring (~1 MB) stays ahead of an 8 K-frame pull, so play_consume serves real audio
                // (not zero-fill); we pull exactly `target` frames total and never read past EOF.
                const uint32_t got = _stream->play_consume(
                    d, reinterpret_cast<uint8_t*>(_buf[i][s] + _wpos[i][s]), want * sizeof(float)) / sizeof(float);
                _wpos[i][s] += got;
                if (_wpos[i][s] >= target || _wpos[i][s] >= _cap) {
                    _len[i][s] = _wpos[i][s]; _read[i][s] = 0.0; _stream->stop(d); _load[i][s] = Load::Idle;
                }
            }
        }
    }
}

void ShuttleEngine::process(const float* const* in, float** out, size_t size) {
    const size_t n = size > kMaxFrames ? kMaxFrames : size;
    float a0[kMaxFrames], a1[kMaxFrames], b0[kMaxFrames], b1[kMaxFrames];
    _render_track(DeckRef::A, 0, in, 0, a0, n);
    _render_track(DeckRef::A, 1, in, 0, a1, n);
    _render_track(DeckRef::B, 0, in, 1, b0, n);
    _render_track(DeckRef::B, 1, in, 1, b1, n);

    // Per-track tape FX (wow/flutter + Jiles-Atherton saturation + resonant low-pass) on each rolling
    // track's signal, before the pan/sum. BYPASSED when the track's FX is at neutral settings: the
    // wow/flutter fdelay imposes a fixed ~25 ms delay (and the filter is not bit-identity even fully
    // open) regardless of depth, so a neutral kernel is NOT transparent - skipping it keeps faithful
    // varispeed playback exact when the FX is untouched, and also confines the Jiles-Atherton cost to
    // tracks both rolling AND actually using the FX. Neutral = drive/char/wow off, cutoff fully open,
    // reso off (rate is irrelevant at zero wow depth).
    auto engaged = [](const float* p) {
        return p[0] > 0.f || p[1] > 0.f || p[2] > 0.f || p[4] < 1.f || p[5] > 0.f;
    };
    bool any_fx = false;
    for (int d = 0; d < 2; d++)
        for (int t = 0; t < kTracks; t++)
            if (_fx[d][t] && _rolling[d][t] && engaged(_fx_n[d][t])) {
                float* buf = (d == 0) ? (t == 0 ? a0 : a1) : (t == 0 ? b0 : b1);
                _fx[d][t]->process(buf, static_cast<int>(n));
                any_fx = true;
            }

    // Each track is panned independently into the stereo bus: total gain = MIX volume x A/B blend x
    // its per-track pan (L/R, route-dependent). Coeffs computed once per block, outside the sample loop.
    const float LA0 = _gA * _gain[0][0] * _panL[0][0], RA0 = _gA * _gain[0][0] * _panR[0][0];
    const float LA1 = _gA * _gain[0][1] * _panL[0][1], RA1 = _gA * _gain[0][1] * _panR[0][1];
    const float LB0 = _gB * _gain[1][0] * _panL[1][0], RB0 = _gB * _gain[1][0] * _panR[1][0];
    const float LB1 = _gB * _gain[1][1] * _panL[1][1], RB1 = _gB * _gain[1][1] * _panR[1][1];
    // Sum the four panned tracks. SoftLimit the bus ONLY when FX is engaged on some rolling track: up to
    // four saturated tracks plus the filter's resonant peak (Q ~10) can overshoot 0 dBFS, and there is no
    // codec headroom past +/-1, so the cubic soft-clip (same one the tape/edrums/granular buses use) tames
    // those peaks. But SoftLimit is not identity below unity, so with the FX off we keep the plain linear
    // sum - that preserves shuttle's bit-faithful varispeed playback (and the clean-replay guarantee).
    if (any_fx) {
        for (size_t i = 0; i < n; i++) {
            out[0][i] = daisysp::SoftLimit(a0[i] * LA0 + a1[i] * LA1 + b0[i] * LB0 + b1[i] * LB1);
            out[1][i] = daisysp::SoftLimit(a0[i] * RA0 + a1[i] * RA1 + b0[i] * RB0 + b1[i] * RB1);
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            out[0][i] = a0[i] * LA0 + a1[i] * LA1 + b0[i] * LB0 + b1[i] * LB1;
            out[1][i] = a0[i] * RA0 + a1[i] * RA1 + b0[i] * RB0 + b1[i] * RB1;
        }
    }
}

// One track -> a mono block: record-monitor (and capture to its buffer), bidirectional varispeed
// playback, or silence. Reverse and freeze fall out of the SIGNED _speed with no special-casing.
// POS/SIZE -> the track's loop window in frames. SIZE sets the length (clamped to a minimum and to the
// recording); POS slides the start across the part of the buffer the window does not cover, so the
// window always stays inside [0, len). At SIZE=full, the window is the whole buffer and POS is inert.
void ShuttleEngine::_window(int i, int s, uint32_t& start, uint32_t& len) const {
    const uint32_t L = _len[i][s];
    if (L == 0) { start = 0; len = 0; return; }
    uint32_t ll = static_cast<uint32_t>(_size_n[i][s] * static_cast<float>(L));
    if (ll < kMinLoopFrames) ll = kMinLoopFrames;
    if (ll > L) ll = L;
    start = static_cast<uint32_t>(_pos_n[i][s] * static_cast<float>(L - ll));
    len   = ll;
}

void ShuttleEngine::_render_track(DeckRef::Ref d, int s, const float* const* in, int ch, float* mono, size_t n) {
    const int i = idx(d);
    uint32_t S, Lw; _window(i, s, S, Lw);                     // this track's loop window (frames)
    // Seq-pad re-align. A track that is audibly moving dips through a short declick ramp (fade out ->
    // jump to 0 -> fade in) so the pointer discontinuity makes no click; a silent / stopped track has
    // nothing to mask, so it just snaps. _declick counts 2*kDeclickRamp..0 across blocks; the jump is at
    // the gain minimum (kDeclickRamp).
    if (_realign[i][s]) {
        _realign[i][s] = false;
        const bool audible = _rolling[i][s] && Lw > 0 && _speed[i][s] != 0.f && _buf[i][s];
        if (audible) _declick[i][s] = 2 * kDeclickRamp;
        else { _read[i][s] = static_cast<double>(S); _declick[i][s] = 0; }   // snap to the window start
    }
    if (_recording[i][s]) {
        _declick[i][s] = 0;
        for (size_t k = 0; k < n; k++) {
            const float x = in ? in[ch][k] : 0.f;
            mono[k] = x;                                          // monitor the input being recorded
            if (_wpos[i][s] < _cap) { _buf[i][s][_wpos[i][s]++] = x; _len[i][s] = _wpos[i][s]; }
        }
        return;
    }
    if (_rolling[i][s] && Lw > 0 && _speed[i][s] != 0.f && _buf[i][s]) {
        const double   Sd  = static_cast<double>(S);
        const double   Ld  = static_cast<double>(Lw);
        const double   End = Sd + Ld;
        const uint32_t E   = S + Lw;                              // one past the window's last frame
        const float    sp  = _speed[i][s];
        double p = _read[i][s];
        { double rel = p - Sd; rel -= std::floor(rel / Ld) * Ld; p = Sd + rel; }  // bring p into the window
        for (size_t k = 0; k < n; k++) {
            float g = 1.f;
            if (_declick[i][s] > 0) {                             // realign declick ramp in progress
                const int dc = _declick[i][s];
                if (dc == kDeclickRamp) p = Sd;                   // jump to the window start at the gain min
                g = (dc > kDeclickRamp) ? (dc - kDeclickRamp) * (1.f / kDeclickRamp)   // fade out 1 -> 0
                                        : (kDeclickRamp - dc) * (1.f / kDeclickRamp);  // fade in  0 -> 1
                _declick[i][s] = dc - 1;
            }
            uint32_t i0 = static_cast<uint32_t>(p);
            if (i0 >= E) i0 = E - 1;                              // guard the fp upper edge
            uint32_t i1 = i0 + 1; if (i1 >= E) i1 = S;            // wrap to the window start for interp
            const float fr = static_cast<float>(p - static_cast<double>(i0));
            mono[k] = (_buf[i][s][i0] + (_buf[i][s][i1] - _buf[i][s][i0]) * fr) * g;
            p += sp;
            while (p >= End) p -= Ld;                             // wrap forward within the window (loop)
            while (p < Sd)   p += Ld;                             // wrap reverse within the window (loop)
        }
        _read[i][s] = p;
        return;
    }
    // stopped / empty -> silence. Finish an interrupted declick instantly (output is silent anyway), so
    // the realign target (the window start) is still reached and no stale fade lingers.
    if (_declick[i][s] > 0) { _read[i][s] = static_cast<double>(S); _declick[i][s] = 0; }
    for (size_t k = 0; k < n; k++) mono[k] = 0.f;
}

void ShuttleEngine::set_param(ParamId id, DeckRef::Ref d, float v) {
    const int i = idx(d);
    const int s = _active[i];                                     // knobs address the FOCUSED track
    // Flag a value-bar overlay when a display knob actually moves (the platform re-sends the current
    // value every loop, so gate on a real change). render() shows _edit_val until _edit_until.
    auto edit = [&](float cur) { if (_time && std::fabs(v - cur) > 1e-4f) { _edit_val[i] = v; _edit_param[i] = id; _edit_until[i] = _time->now_ms() + kEditShowMs; } };
    if (id == ParamId::Speed) { edit(_speed_n[i][s]); _speed_n[i][s] = v; _speed[i][s] = speed_from_knob(v); }
    else if (id == ParamId::Pos)  { edit(_pos_n[i][s]);  _pos_n[i][s]  = v; }   // loop window start  (computed per block)
    else if (id == ParamId::Size) { edit(_size_n[i][s]); _size_n[i][s] = v; }   // loop window length (computed per block)
    else if (id == ParamId::Mix) { edit(_gain[i][s]); _gain[i][s] = v; }
    else if (id == ParamId::AltPos) { _pan[i][s] = v; _recompute_pan(); }   // per-track manual pan
    else if (id == ParamId::Crossfade) { _xfade = v; _recompute_blend(); }
    else if (id == ParamId::Aux) {
        const int sl = static_cast<int>(v * kTapeSlots);
        const int ns = sl < 0 ? 0 : (sl >= kTapeSlots ? kTapeSlots - 1 : sl);
        if (ns != _tape_slot[i][s]) { _tape_slot[i][s] = ns; _request_load(d, s); }   // pick a slot -> load it
    }
    // Tape FX on the focused track: GRIT pad modifier = saturation (grit+PITCH=drive, grit+MIX=char),
    // FLUX pad modifier = filter (flux+PITCH=cutoff, flux+MIX=reso). MODFREQ (rate) is in set_mod_speed.
    else if (id == ParamId::GritIntensity) { _fx_n[i][s][0] = v; if (_fx[i][s]) _fx[i][s]->set(0, v); } // drive
    else if (id == ParamId::GritMix)       { _fx_n[i][s][1] = v; if (_fx[i][s]) _fx[i][s]->set(1, v); } // character
    else if (id == ParamId::ModAmp)        { _fx_n[i][s][2] = v; if (_fx[i][s]) _fx[i][s]->set(2, v); } // wow/flutter depth
    else if (id == ParamId::FluxIntensity) { _fx_n[i][s][4] = v; if (_fx[i][s]) _fx[i][s]->set(4, v); } // filter cutoff
    else if (id == ParamId::FluxMix)       { _fx_n[i][s][5] = v; if (_fx[i][s]) _fx[i][s]->set(5, v); } // filter resonance
}

float ShuttleEngine::param(ParamId id, DeckRef::Ref d) const {
    const int i = idx(d);
    // Crossfade is global (deck-A slot). set_param stores it and derives the A/B blend gains, but
    // param() had no case, so `get param crossfade` reported 0 for a value the engine really holds.
    // Found by the on-target sweep 2026-07-31 (six engines shared this omission); the UI only ever
    // writes this param, so nothing else noticed.
    if (id == ParamId::Crossfade) return _xfade;
    const int s = _active[i];
    if (id == ParamId::Speed)  return _speed_n[i][s];
    if (id == ParamId::Pos)    return _pos_n[i][s];
    if (id == ParamId::Size)   return _size_n[i][s];
    if (id == ParamId::Mix)    return _gain[i][s];
    if (id == ParamId::AltPos) return _pan[i][s];
    if (id == ParamId::Aux)    return (static_cast<float>(_tape_slot[i][s]) + 0.5f) / static_cast<float>(kTapeSlots);
    if (id == ParamId::GritIntensity) return _fx_n[i][s][0]; // drive
    if (id == ParamId::GritMix)       return _fx_n[i][s][1]; // character
    if (id == ParamId::ModAmp)        return _fx_n[i][s][2]; // wow/flutter depth
    if (id == ParamId::ModSpeed)      return _fx_n[i][s][3]; // wow/flutter rate (MODFREQ)
    if (id == ParamId::FluxIntensity) return _fx_n[i][s][4]; // filter cutoff (seeds the flux+PITCH pickup open)
    if (id == ParamId::FluxMix)       return _fx_n[i][s][5]; // filter resonance
    return 0.f;
}

void ShuttleEngine::set_mod_speed(DeckRef::Ref d, float v, bool /*sync*/) {
    const int i = idx(d);
    const int s = _active[i];                                     // FX addresses the FOCUSED track
    _fx_n[i][s][3] = v; if (_fx[i][s]) _fx[i][s]->set(3, v);       // MODFREQ -> wow/flutter rate
}

void ShuttleEngine::set_aux_active(DeckRef::Ref d, bool held) {
    const int i = idx(d);
    if (held && !_aux_held[i]) _rescan[i] = true;
    _aux_held[i] = held;
}

bool ShuttleEngine::take_param_reseed(DeckRef::Ref d) {
    const int i = idx(d);
    const bool f = _want_reseed[i];
    _want_reseed[i] = false;
    return f;
}

// Rev pad (reverse=true): swap which of the deck's two tracks is focused, and ask the platform to
// reseed the focused track's knobs (Speed/Mix/Aux) so the pots catch it without a jump. Both tracks
// keep playing - the swap only moves the edit focus. Plain Play (reverse=false): toggle the focused
// track's rolling and SNAP it to unity on engage (with the same pickup reseed).
bool ShuttleEngine::on_play_pad(DeckRef::Ref d, bool reverse) {
    const int i = idx(d);
    const uint32_t now = _time ? _time->now_ms() : 0;
    if (_time && now - _last_trig_ms[i] < kDebounceMs) return false;
    _last_trig_ms[i] = now;

    if (reverse) {
        _active[i] ^= 1;
        _want_reseed[i] = true;
        _swap_show[i] = 60;                                       // briefly flash which track is focused
        return false;                                            // not "empty" - suppress the empty flash
    }

    const int s = _active[i];
    _err_until[i] = 0;
    if (_recording[i][s]) return _len[i][s] == 0;                // ignore Play while recording
    if (!_rolling[i][s]) {
        if (_len[i][s] == 0) { _err_until[i] = now + kErrFlashMs; return true; }
        _rolling[i][s] = true;
        _speed_n[i][s] = knob_for_speed(1.f); _speed[i][s] = 1.f; _want_reseed[i] = true;  // snap to unity
    } else {
        _rolling[i][s] = false;
    }
    return _len[i][s] == 0;
}

// Alt+Play: toggle recording on the focused track (play XOR record for that track). Start overwrites
// the buffer from frame 0; stop finalizes the recorded length. The deck's OTHER track is unaffected.
void ShuttleEngine::on_record_pad(DeckRef::Ref d, bool reverse) {
    if (reverse) return;
    const int i = idx(d);
    const int s = _active[i];
    const uint32_t now = _time ? _time->now_ms() : 0;
    if (_time && now - _last_trig_ms[i] < kDebounceMs) return;
    _last_trig_ms[i] = now;
    if (_recording[i][s]) { _recording[i][s] = false; _len[i][s] = _wpos[i][s]; }
    else { _recording[i][s] = true; _rolling[i][s] = false; _wpos[i][s] = 0; _len[i][s] = 0; _read[i][s] = 0.0; }
}

// Seq pad: re-align ALL four tracks to their loop start in one atomic gesture, so free-running tracks
// that have drifted apart snap back to a common downbeat. Deck-agnostic (either deck's Seq pad does the
// same global align). The actual pointer zero is deferred to the ISR (a flag per track), not written
// here, because _read is a double the audio ISR also updates - the flag avoids a torn cross-thread
// write, and all four flags being set before the next block makes the snap simultaneous.
void ShuttleEngine::on_seq_trigger(DeckRef::Ref /*d*/) {
    for (int d = 0; d < 2; d++)
        for (int s = 0; s < kTracks; s++)
            _realign[d][s] = true;
}

void ShuttleEngine::_recompute_blend() {
    _gA = _xfade <= 0.5f ? 1.f : 2.f * (1.f - _xfade);
    _gB = _xfade >= 0.5f ? 1.f : 2.f * _xfade;
}

// Routing switch (mirrors the granular/tape int mapping so the panel L/C/R reads the same):
// 0 = Stereo (centre), 1 = DoubleMono (left), 2 = GenerativeStereo (right). The platform calls this
// every loop, so act only on an actual switch transition - otherwise GenerativeStereo would re-roll
// its random pans on every pass instead of once on entry.
bool ShuttleEngine::set_config(ConfigId id, DeckRef::Ref, int value) {
    if (id == ConfigId::Route) {
        const Route r = (value == 2) ? Route::GenerativeStereo
                      : (value == 1) ? Route::DoubleMono
                                     : Route::Stereo;
        if (r != _route) {
            _route = r;
            if (_route == Route::GenerativeStereo) _roll_random_pans();   // also recomputes the gains
            else                                   _recompute_pan();
        }
    }
    return false;
}

// Resolve each track's effective pan position to equal-power L/R gains, per the routing switch:
//   DoubleMono       -> the manual Alt+POS pan (_pan)
//   Stereo           -> auto-spread: a deck's tracks fan evenly across L..R (2 tracks -> hard L/R)
//   GenerativeStereo -> the random positions rolled in _roll_random_pans (_rnd)
void ShuttleEngine::_recompute_pan() {
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

// Assign every track a fresh random pan position (the GenerativeStereo / RIGHT routing). Uses a small
// LCG so it needs no platform RNG and stays deterministic, mirroring the tape engine.
void ShuttleEngine::_roll_random_pans() {
    for (int i = 0; i < 2; i++) {
        for (int s = 0; s < kTracks; s++) {
            _rng = _rng * 1664525u + 1013904223u;
            _rnd[i][s] = static_cast<float>(_rng >> 8) * (1.f / 16777216.f);   // [0,1)
        }
    }
    _recompute_pan();
}

// Per-track standby hue. Each deck's ring shows only its FOCUSED track, so giving the two tracks
// distinct hues makes it obvious at a glance which one is currently visible. Picked clear of the
// transport colors (green fwd / cyan rev / red rec / amber err) so an idle glow is never mistaken
// for a transport state.
static constexpr uint32_t kTrackHue[ShuttleEngine::kTracks] = {
    0x7c4dff,   // track 1: indigo
    0xffc000,   // track 2: amber
};

void ShuttleEngine::render(DisplayModel& m) {
    m.clear();
    const uint32_t now = _time ? _time->now_ms() : 0;
    // Slow standby pulse so a loaded-but-stopped deck reads as "on, ready" instead of powered-off
    // (0.35..0.60 ambient glow); shared with softcut/granular via the indicator toolkit.
    const float breathe = motion::breathe_standby(now);
    for (DeckRef::Ref dk : { DeckRef::A, DeckRef::B }) {
        const int  i   = idx(dk);
        const int  s   = _active[i];
        const bool err = _time && now < _err_until[i];
        // Direction-coded transport of the focused track: recording red, forward green, reverse cyan,
        // engaged-but-frozen dim white, rejected action amber, idle -> the track's hue breathing as
        // an ambient "loaded and ready" standby.
        const auto tv = transport_view(_rolling[i][s], _recording[i][s], _speed[i][s], err,
                                       kTrackHue[s], breathe * 0.5f);
        led::transport(m, i, tv);

        if (_aux_held[i]) {
            // Alt+PITCH held: tape-slot selector for the focused track (selected bright, recorded mid,
            // empty dim).
            uint32_t used = 0;
            for (int t = 0; t < kTapeSlots; t++) if (_slot_used[i][t]) used |= (1u << t);
            ring::slots(m.ring[i], kTapeSlots, _tape_slot[i][s], used);
        } else if (_swap_show[i] > 0) {
            // Just after a Rev swap: flash the focused-track number (1 or 2 dots) for a moment, on the
            // new track's hue so the color change itself reinforces the swap.
            _swap_show[i]--;
            m.ring[i].set_brightness(1.f);
            m.ring[i].set_hex_color(kTrackHue[s]); m.ring[i].set_segment(0.f, 0.999f);
            m.ring[i].set_point_hex_color(0xffffff);
            for (int t = 0; t <= s; t++) m.ring[i].set_point(static_cast<uint8_t>(t * 4), 1.f);
        } else if (_time && now < _edit_until[i]
                   && (_edit_param[i] == ParamId::Pos || _edit_param[i] == ParamId::Size) && _len[i][s] > 0) {
            // POS/SIZE moving: show the loop WINDOW itself - an arc whose length is SIZE, with a bright
            // dot at its START (the position POS sets), so you see where the loop begins and how long
            // it is (not a meaningless 0..value bar).
            uint32_t S, Lw; _window(i, s, S, Lw);
            const float L = static_cast<float>(_len[i][s]);
            ring::window(m.ring[i], static_cast<float>(S) / L, static_cast<float>(Lw) / L, kTrackHue[s], 0.7f);
            ring::playhead(m.ring[i], static_cast<float>(S) / L, 1.f);   // start-of-loop marker (= POS)
        } else if (_time && now < _edit_until[i]) {
            // Other knobs (MIX level, PITCH speed) apply immediately: a plain value bar in the track
            // hue (no pickup-deviation - a granular-MValue concept, N/A here; see the header note).
            ring::value(m.ring[i], _edit_val[i], kTrackHue[s]);
        } else if (_len[i][s] > 0) {
            // Normal: the loop WINDOW (POS..POS+SIZE) drawn as the ring arc in the transport color
            // (live) or the breathing track hue (idle), with the read head moving inside it - so the
            // loop start/length are visible, not just a full-ring backdrop.
            const uint32_t back = tv.live ? tv.rgb : kTrackHue[s];
            const float    bri  = tv.live ? 1.f : breathe;
            uint32_t S, Lw; _window(i, s, S, Lw);
            const float L = static_cast<float>(_len[i][s]);
            ring::window(m.ring[i], static_cast<float>(S) / L, static_cast<float>(Lw) / L, back, bri);
            ring::playhead(m.ring[i], static_cast<float>(_read[i][s] / static_cast<double>(_len[i][s])),
                           tv.live ? 1.f : 0.55f);
        } else {
            // Empty track: a full-ring idle glow so it still reads as "on, ready".
            ring::progress(m.ring[i], 0.999f, kTrackHue[s], breathe);
        }

        // Cycle LED: wow/flutter modulation activity - the glow pulses at the MODFREQ (rate) knob's
        // speed, scaled by the MOD_AMT (depth) knob, so dialed-in modulation is visible on the panel.
        const float depth = _fx_n[i][s][2];
        if (depth > 1e-3f) {
            const uint32_t period = static_cast<uint32_t>(2000.f - 1850.f * _fx_n[i][s][3]); // ~2 s..150 ms
            led::cycle(m, i, depth * motion::breathe(now, 0.f, 1.f, period), kTrackHue[s]);
        }
        m.ring[i].set_updated();
    }
    // Global A/B crossfade on the fader LEDs (brightness = each deck's share of the mix).
    led::fader_balance(m, _xfade);

    // Routing switch position on the mode L/C/R LED (matches the tape engine).
    if      (_route == Route::DoubleMono) m.mode_left   = { 0xffffff, 0.8f };
    else if (_route == Route::Stereo)     m.mode_center = { 0xffffff, 0.8f };
    else                                  m.mode_right  = { 0xffffff, 0.8f };
}

const char* ShuttleEngine::_path(DeckRef::Ref d, int slot) {
    char* p = _pbuf;
    for (const char* s = "shuttle/tape_"; *s; ) *p++ = *s++;
    *p++ = (d == DeckRef::A) ? 'a' : 'b';
    *p++ = '_';
    *p++ = static_cast<char>('1' + slot);
    for (const char* s = ".wav"; *s; ) *p++ = *s++;
    *p = '\0';
    return _pbuf;
}

int      ShuttleEngine::active_track(DeckRef::Ref d) const { return _active[idx(d)]; }
uint32_t ShuttleEngine::buffer_frames(DeckRef::Ref d, int t) const { return _len[idx(d)][t]; }
uint32_t ShuttleEngine::loop_start(DeckRef::Ref d, int t) const { uint32_t s, l; _window(idx(d), t, s, l); return s; }
uint32_t ShuttleEngine::loop_len(DeckRef::Ref d, int t)   const { uint32_t s, l; _window(idx(d), t, s, l); return l; }
float    ShuttleEngine::read_position(DeckRef::Ref d, int t) const { return static_cast<float>(_read[idx(d)][t]); }
float    ShuttleEngine::track_speed(DeckRef::Ref d, int t) const { return _speed[idx(d)][t]; }
bool     ShuttleEngine::is_rolling(DeckRef::Ref d, int t) const { return _rolling[idx(d)][t]; }
bool     ShuttleEngine::is_recording(DeckRef::Ref d, int t) const { return _recording[idx(d)][t]; }

} // namespace spotykach
