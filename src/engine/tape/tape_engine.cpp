#include "engine/tape/tape_engine.h"
#include "engine/arena.h"
#include "engine/indicators.h" // shared indicator toolkit (docs/dev/indicator-grammar.md §8)
#include "engine/tape/tapefx.h" // shared TapeFx wrapper around the cyfaust-generated tfx_tapefx::mydsp

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new> // placement new

namespace spotykach {

void TapeEngine::init(const EngineContext& ctx) {
    _stream = ctx.stream;
    _time   = ctx.time;
    // Construct a tape-FX kernel per deck in the SDRAM arena (each ~16 KB of delay-line state).
    Arena ar(ctx.arena);
    const int sr = static_cast<int>(ctx.sample_rate);
    for (int i = 0; i < 2; i++) {
        if (void* m = ar.alloc<uint8_t>(sizeof(TapeFx), alignof(TapeFx))) {
            _fx[i] = new (m) TapeFx();
            _fx[i]->init(sr);
            for (int p = 0; p < 6; p++) _fx[i]->set(p, _fx_n[i][p]); // seed from the cached defaults
        }
    }
}

// Main-loop housekeeping: push each deck's loop-enable to the stream (so it seeks to the top vs stops at
// EOF), and action a Frippertronics fade-out that asked to stop (stop() touches FatFs, so it must run
// here, not in the audio ISR that set the flag).
void TapeEngine::prepare() {
    if (!_stream) return;

    // Boot slot scan: render() gates a deck's idle "breathing" ring on whether its current slot has a
    // file (so an unloaded deck reads dark). That cache (_slot_used) is normally filled only when the
    // Alt+PITCH selector opens - too late for a freshly-booted deck. The SD card mounts a few loops in,
    // so re-probe every pass until a file turns up (card up -> cache good) or a deadline passes (no card
    // / empty card -> every slot reads empty, which is the correct "dark" state). Selector-open rescans
    // and play/record successes keep the cache current afterwards.
    if (_boot_scan) {
        _scan_slots(DeckRef::A); _scan_slots(DeckRef::B);
        bool any = false;
        for (int i = 0; i < 2; i++) for (int s = 0; s < kSlots; s++) any |= _slot_used[i][s];
        const uint32_t now = _time ? _time->now_ms() : 0;
        if (any || (_time && now > kBootScanMs)) _boot_scan = false;
    }

    for (DeckRef::Ref d : { DeckRef::A, DeckRef::B }) {
        const int i = (d == DeckRef::A) ? 0 : 1;
        _stream->set_loop(d, _loop_mode[i] != Loop::None);
        if (_want_stop[i]) { _want_stop[i] = false; _stream->stop(d); }
        if (_rescan[i])    { _rescan[i] = false; _scan_slots(d); }  // refresh recorded/empty slot cache
    }
}

void TapeEngine::process(const float* const* in, float** out, size_t size) {
    const size_t n = size > kMaxFrames ? kMaxFrames : size;
    if (!_stream) { for (int c = 0; c < 2; c++) for (size_t i = 0; i < n; i++) out[c][i] = 0.f; return; }

    // Each deck -> a mono stream (playback / record-monitor / silence). Deck A reads input A, B reads B.
    float monoA[kMaxFrames], monoB[kMaxFrames];
    _render_deck(DeckRef::A, in, 0, monoA, n);
    _render_deck(DeckRef::B, in, 1, monoB, n);

    // Tape FX (wow/flutter + hysteresis) on the played-back signal only - not on the record monitor,
    // whose wow/flutter delay would add monitoring latency.
    if (_fx[0] && _stream->is_playing(DeckRef::A)) _fx[0]->process(monoA, static_cast<int>(n));
    if (_fx[1] && _stream->is_playing(DeckRef::B)) _fx[1]->process(monoB, static_cast<int>(n));

    // Per-deck output-level meter (drives the ring arc in render). Peak-hold with a per-block release,
    // taken on each deck's own signal (playback, or the record monitor while recording) BEFORE the
    // pan/mix gains, so the ring shows deck activity - including the incoming level while recording.
    float pkA = 0.f, pkB = 0.f;
    for (size_t i = 0; i < n; i++) {
        const float a = std::fabs(monoA[i]); if (a > pkA) pkA = a;
        const float b = std::fabs(monoB[i]); if (b > pkB) pkB = b;
    }
    _peak[0] = pkA > _peak[0] ? pkA : _peak[0] * kMeterDecay;
    _peak[1] = pkB > _peak[1] ? pkB : _peak[1] * kMeterDecay;

    // Per-block output gains: per-deck pan (selected by the routing switch) scaled by the mix-fader A/B
    // blend. Pan/blend gains are precomputed on knob change, so the ISR loop is just multiplies.
    float pLa, pRa, pLb, pRb;
    switch (_route) {
        case Route::DoubleMono:                          // LEFT: each deck panned by its Alt+POS knob
            pLa = _panL[0]; pRa = _panR[0]; pLb = _panL[1]; pRb = _panR[1]; break;
        case Route::GenerativeStereo:                    // RIGHT: random pan per deck
            pLa = _rndL[0]; pRa = _rndR[0]; pLb = _rndL[1]; pRb = _rndR[1]; break;
        case Route::Stereo: default:                     // CENTRE: both decks centered
            pLa = pRa = pLb = pRb = kCenterGain; break;
    }
    // Total per-deck gains = MIX-knob volume x mix-fader A/B blend x pan (L/R).
    const float La = _gain[0] * _gA * pLa, Ra = _gain[0] * _gA * pRa,
                Lb = _gain[1] * _gB * pLb, Rb = _gain[1] * _gB * pRb;
    // SoftLimit the summed bus: two decks plus the filter's resonant peak (Q up to ~10) can overshoot
    // 0 dBFS, and there is no codec headroom past +/-1. The cubic soft-clip (same one the edrums/granular
    // buses use) is ~transparent below unity and gently tames peaks instead of hard-clipping them.
    for (size_t i = 0; i < n; i++) {
        out[0][i] = daisysp::SoftLimit(monoA[i] * La + monoB[i] * Lb);
        out[1][i] = daisysp::SoftLimit(monoA[i] * Ra + monoB[i] * Rb);
    }
}

// PITCH -> per-deck varispeed. Alt+POS (`AltPos`) -> per-deck pan. MIX -> per-deck volume. ENV -> loop
// mode (4 quadrants: none / plain / faded / Frippertronics). Alt+PITCH (`Aux`) -> tape-slot select.
// Bare POS (`Pos`) is reserved for a future loop-start control - ignored for now.
void TapeEngine::set_param(ParamId id, DeckRef::Ref d, float v) {
    const int i = (d == DeckRef::A) ? 0 : 1;
    // Flag a knob-turn value overlay when a display param ACTUALLY moves (the platform re-sends the
    // current value every loop, so gate on a real change). render() shows it until _edit_until. Aux
    // (slot select) is excluded - it has its own selector picture.
    const uint32_t now = _time ? _time->now_ms() : 0;
    auto edit = [&](float cur) {
        if (_time && std::fabs(v - cur) > 1e-4f) { _edit_val[i] = v; _edit_param[i] = id; _edit_until[i] = now + kEditShowMs; }
    };
    if (id == ParamId::Speed) { _speed_n[i] = v; _speed[i] = std::exp2f((v - 0.5f) * 2.f); }  // no overlay: the varispeed marker already lives in the steady ring, so PITCH just moves it (no dim takeover)
    else if (id == ParamId::AltPos) { edit(_pan[i]); _pan[i] = v; _panL[i] = std::cos(v * kHalfPi); _panR[i] = std::sin(v * kHalfPi); }
    else if (id == ParamId::Crossfade) { edit(_xfade); _xfade = v; _gA = v <= 0.5f ? 1.f : 2.f * (1.f - v);
                                                                   _gB = v >= 0.5f ? 1.f : 2.f * v; }
    else if (id == ParamId::Mix) { edit(_gain[i]); _gain[i] = v; }
    else if (id == ParamId::Env) { edit(_env_n[i]); _env_n[i] = v;
        _loop_mode[i] = v < 0.25f ? Loop::None  : v < 0.5f ? Loop::Plain
                      : v < 0.75f ? Loop::Faded : Loop::Fripp; }
    else if (id == ParamId::Aux) { const int s = static_cast<int>(v * kSlots);
                                   _slot[i] = s < 0 ? 0 : (s >= kSlots ? kSlots - 1 : s); }
    // Tape FX: POS=drive, SIZE=character, MOD_AMT=wow/flutter depth (rate is MODFREQ, set_mod_speed).
    else if (id == ParamId::Pos)    { edit(_fx_n[i][0]); _fx_n[i][0] = v; if (_fx[i]) _fx[i]->set(0, v); }
    else if (id == ParamId::Size)   { edit(_fx_n[i][1]); _fx_n[i][1] = v; if (_fx[i]) _fx[i]->set(1, v); }
    else if (id == ParamId::ModAmp) { edit(_fx_n[i][2]); _fx_n[i][2] = v; if (_fx[i]) _fx[i]->set(2, v); }
    // Post-FX low-pass filter: held grit + PITCH = cutoff, held grit + MIX = resonance. The platform
    // already routes the grit-modifier knobs to these ParamIds (it sends them for any engine), so the
    // tape engine just reinterprets `GritIntensity`/`GritMix` as the filter knobs - no platform change.
    else if (id == ParamId::GritIntensity) { edit(_fx_n[i][4]); _fx_n[i][4] = v; if (_fx[i]) _fx[i]->set(4, v); } // cutoff
    else if (id == ParamId::GritMix)       { edit(_fx_n[i][5]); _fx_n[i][5] = v; if (_fx[i]) _fx[i]->set(5, v); } // resonance
}

void TapeEngine::set_mod_speed(DeckRef::Ref d, float v, bool /*sync*/) {
    const int i = (d == DeckRef::A) ? 0 : 1;
    if (_time && std::fabs(v - _fx_n[i][3]) > 1e-4f) {   // wow/flutter rate moved -> flash its value bar
        _edit_val[i] = v; _edit_param[i] = ParamId::ModSpeed; _edit_until[i] = _time->now_ms() + kEditShowMs;
    }
    _fx_n[i][3] = v; if (_fx[i]) _fx[i]->set(3, v);   // MODFREQ -> wow/flutter rate
}

float TapeEngine::param(ParamId id, DeckRef::Ref d) const {
    const int i = (d == DeckRef::A) ? 0 : 1;
    if (id == ParamId::Speed) return _speed_n[i];
    if (id == ParamId::AltPos) return _pan[i];
    if (id == ParamId::Mix)   return _gain[i];
    if (id == ParamId::Env)   return _env_n[i];
    if (id == ParamId::Aux)   return (static_cast<float>(_slot[i]) + 0.5f) / static_cast<float>(kSlots);
    if (id == ParamId::Pos)     return _fx_n[i][0]; // drive
    if (id == ParamId::Size)    return _fx_n[i][1]; // character
    if (id == ParamId::ModAmp)  return _fx_n[i][2]; // wow/flutter depth
    if (id == ParamId::ModSpeed) return _fx_n[i][3]; // wow/flutter rate
    if (id == ParamId::GritIntensity) return _fx_n[i][4]; // filter cutoff (seeds the grit+PITCH pickup open)
    if (id == ParamId::GritMix)       return _fx_n[i][5]; // filter resonance (seeds the grit+MIX pickup)
    return 0.f;
}

void TapeEngine::set_aux_active(DeckRef::Ref d, bool held) {
    const int i = (d == DeckRef::A) ? 0 : 1;
    if (held && !_aux_held[i]) _rescan[i] = true;   // selector just opened -> re-probe slots in prepare()
    _aux_held[i] = held;
}

// Routing switch (mirrors the granular int mapping so the panel L/C/R reads the same):
// 0 = Stereo (centre), 1 = DoubleMono (left), 2 = GenerativeStereo (right).
bool TapeEngine::set_config(ConfigId id, DeckRef::Ref, int value) {
    if (id == ConfigId::Route) {
        const Route r = (value == 2) ? Route::GenerativeStereo
                      : (value == 1) ? Route::DoubleMono
                                     : Route::Stereo;
        if (r != _route) {   // only act on an actual route transition - the platform calls this every loop
            _route = r;
            if (_route == Route::GenerativeStereo) _roll_random_pans();   // re-rolled on entering the mode, not continuously
        }
    }
    return false;
}

// Per-deck transport. Only the Play pad acts (`reverse == false`); the Rev pad is left inert (free for
// future reverse playback), so the mapping is exactly Play = play, Alt+Play = record.
bool TapeEngine::on_play_pad(DeckRef::Ref d, bool reverse)   { if (!reverse) _toggle(d, /*record=*/false); return false; }
void TapeEngine::on_record_pad(DeckRef::Ref d, bool reverse) { if (!reverse) _toggle(d, /*record=*/true); }

// Per-deck idle/ready hue: the ring's breathing standby glow + the pitch-marker backdrop when a deck is
// loaded but stopped. Distinct per deck so A vs B is obvious at a glance, and picked clear of the
// transport colors (green fwd / red rec / amber err) so an idle glow is never read as a transport state.
static constexpr uint32_t kDeckHue[2] = {
    0x30c0a0,   // deck A: teal
    0xc060ff,   // deck B: violet
};

// A knob is being turned -> a param-aware overlay (the grammar's value-pickup feedback, no-deviation
// form because tape applies knob values immediately). Only params NOT already visible in the steady ring
// get an overlay - PITCH is excluded, since its varispeed marker is drawn in the steady ring and an
// overlay would just dim it. ENV -> a 4-way loop-mode selector; pan -> a bright marker vs a centre tick;
// every scalar (MIX / drive / char / cutoff / reso / wow-rate / crossfade) -> a value bar.
void TapeEngine::_render_edit(LEDRing& r, int i, uint32_t hue) {
    switch (_edit_param[i]) {
        case ParamId::Env:                                    // loop mode: None/Plain/Faded/Fripp
            ring::selector(r, 4, static_cast<int>(_loop_mode[i]), hue);
            break;
        case ParamId::AltPos:                                 // pan: a bright marker vs a centre tick
            ring::level(r, 0.999f, hue, 0.20f);               // dim context ring
            r.set_brightness(0.45f); ring::playhead(r, 0.5f, 1.f);       // centre reference
            r.set_brightness(0.90f); ring::playhead(r, _pan[i], 1.f);    // pan marker (lifted above the ring)
            break;
        default:                                              // scalar params -> a value bar
            ring::value(r, _edit_val[i], hue);
            break;
    }
}

void TapeEngine::render(DisplayModel& m) {
    m.clear();
    const uint32_t now     = _time ? _time->now_ms() : 0;
    const float    breathe = motion::breathe_standby(now);   // slow "loaded & ready" pulse
    for (DeckRef::Ref dk : { DeckRef::A, DeckRef::B }) {
        const int  i         = (dk == DeckRef::A) ? 0 : 1;
        const bool playing   = _stream && _stream->is_playing(dk);
        const bool recording = _stream && _stream->is_recording(dk);
        const bool err       = _time && now < _err_until[i];   // failed start still flashing
        const bool loaded    = _slot_used[i][_slot[i]];        // current slot has a file (reliable from boot via the prepare() scan)
        const uint32_t hue   = kDeckHue[i];
        // Direction-coded transport on the Play pad (the pad you pressed): playing green, recording red,
        // rejected start amber. A wrong-format reject STROBES the amber (~4.5 Hz via motion::blink) to
        // read distinctly from the steady amber of a missing/empty slot. Play and record are mutually
        // exclusive here (see _toggle), so transport_view's recording-first ordering matches the old
        // playing-first ladder. An idle deck BREATHES the deck hue on the pad when its slot is loaded
        // ("ready" != "off"), or stays dark when unloaded. tape has no reverse/frozen state, so speed is +1.
        const bool err_lit = err && (!_err_fmt[i] || motion::blink(now, 220));
        const auto tv = transport_view(playing, recording, /*speed=*/1.f, err_lit,
                                       loaded ? hue : pal::kBlack, loaded ? breathe * 0.4f : 0.f);
        led::transport(m, i, tv);

        if (_aux_held[i]) {
            // Alt+PITCH held: the tape-slot selector - kSlots dots (selected bright, recorded mid, empty
            // dim) over a faint base.
            uint32_t used = 0;
            for (int s = 0; s < kSlots; s++) if (_slot_used[i][s]) used |= (1u << s);
            ring::slots(m.ring[i], kSlots, _slot[i], used);
        } else if (_time && now < _edit_until[i]) {
            _render_edit(m.ring[i], i, hue);                   // knob-turn value overlay
        } else if (recording) {
            // Recording: an OUTPUT-LEVEL meter (red) over a faint backdrop, so you watch the incoming
            // level - the volatile-but-informative record VU (metering earns its keep here).
            ring::level(m.ring[i], 0.999f, tv.rgb, 0.10f);     // dim full-ring baseline
            ring::level(m.ring[i], _peak[i] * 1.5f, tv.rgb);   // bright arc = input level (headroom-scaled)
        } else if (playing) {
            // Playing: a SINGLE steady-color ring - a level meter reads as too volatile during playback,
            // so the ring is a solid fill, with the varispeed marker + unity reference so PITCH stays
            // visible at a glance (the marker only moves when you turn PITCH, so it never flickers).
            ring::level(m.ring[i], 0.999f, tv.rgb);            // solid ring, full brightness
            ring::playhead(m.ring[i], 0.5f, 0.30f);            // unity (1x) reference
            ring::playhead(m.ring[i], _speed_n[i], 1.f);       // varispeed marker
        } else if (err_lit) {
            // Rejected start: strobe the ring amber in step with the Play pad.
            ring::level(m.ring[i], 0.999f, tv.rgb);
        } else if (loaded) {
            // Loaded but stopped: a dim BREATHING "ready" ring + a brighter pitch marker (preview the
            // varispeed before you hit Play), so a deck with a file reads "on, ready" rather than off.
            // (loaded is reliable from boot now - see the prepare() slot scan.)
            ring::level(m.ring[i], 0.999f, hue, breathe);
            m.ring[i].set_brightness(0.7f);                    // lift the marker above the breathing ring
            ring::playhead(m.ring[i], _speed_n[i], 1.f);
        } // else: unloaded slot -> ring stays dark.
        m.ring[i].set_updated();

        // FX on the dedicated per-deck named LEDs (grammar §4 - unused by every own-display engine but
        // granular). The grit LED tracks what the grit PAD itself does: the resonant low-pass FILTER
        // (grit-pad + PITCH = cutoff, + MIX = reso), yellow. flux = tape SATURATION (POS drive / SIZE
        // character), coral. cycle = wow/flutter, glowing at the MODFREQ rate scaled by depth. Each is
        // dark when neutral and brightens as the FX is dialled in.
        led::grit(m, i, std::max(1.f - _fx_n[i][4], _fx_n[i][5]), GritMode::Drive, /*off=*/0.f);
        led::flux(m, i, std::max(_fx_n[i][0], _fx_n[i][1]),                         /*off=*/0.f);
        const float depth = _fx_n[i][2];
        if (depth > 1e-3f) {
            const uint32_t period = static_cast<uint32_t>(2000.f - 1850.f * _fx_n[i][3]); // ~2 s..150 ms
            led::cycle(m, i, depth * motion::breathe(now, 0.f, 1.f, period), hue);
        }
    }
    // A/B crossfade balance on the fader LEDs, and the routing switch on the mode L/C/R LEDs.
    led::fader_balance(m, _xfade);
    led::route_leds(m, _route);
}

void TapeEngine::_render_deck(DeckRef::Ref d, const float* const* in, int ch, float* mono, size_t n) {
    const int i = (d == DeckRef::A) ? 0 : 1;
    if (_stream->is_playing(d)) {
        const Loop     mode = _loop_mode[i];
        // Loop length (source frames) is only needed to shape the faded / Frippertronics modes.
        const uint32_t L    = (mode == Loop::Faded || mode == Loop::Fripp) ? _stream->loop_frames(d) : 0;
        if (!_primed[i]) {
            _cur[i] = _pull(d); _next[i] = _pull(d); _phase[i] = 0.f; _primed[i] = true;
            _src_pos[i] = 2; _loop_gain[i] = 1.f;   // two frames primed into _cur/_next
        }
        for (size_t s = 0; s < n; s++) {
            float v = _cur[i] + (_next[i] - _cur[i]) * _phase[i];
            if (mode == Loop::Faded && L) v *= _fade_env(_src_pos[i], L);  // dip across the seam
            else if (mode == Loop::Fripp) v *= _loop_gain[i];              // per-pass decay
            mono[s] = v;
            _phase[i] += _speed[i];
            while (_phase[i] >= 1.f) {          // advance one (or more) source frames
                _phase[i] -= 1.f;
                _cur[i] = _next[i];
                _next[i] = _pull(d);             // underrun -> 0 (silence) via play_consume
                if (L && ++_src_pos[i] >= L) {  // crossed a loop boundary
                    _src_pos[i] = 0;
                    if (mode == Loop::Fripp) {
                        _loop_gain[i] *= kFrippDecay;
                        if (_loop_gain[i] < kFrippFloor) { _loop_gain[i] = 0.f; _want_stop[i] = true; }
                    }
                }
            }
        }
    } else if (_stream->is_recording(d)) {
        _primed[i] = false;                      // re-prime the resampler next time playback starts
        for (size_t s = 0; s < n; s++) mono[s] = in ? in[ch][s] : 0.f;   // monitor this deck's input
        _stream->record_produce(d, reinterpret_cast<const uint8_t*>(mono),
                                static_cast<uint32_t>(n * sizeof(float)));
    } else {
        _primed[i] = false;
        for (size_t s = 0; s < n; s++) mono[s] = 0.f;
    }
}

// Toggle play (record=false) or record (record=true) on deck `d`; play and record are mutually exclusive
// per deck. A failed start (file missing / SD not mounted / disk full) returns false and arms an amber
// error flash on that deck's ring so a rejected press is not silent.
void TapeEngine::_toggle(DeckRef::Ref d, bool record) {
    if (!_stream) return;
    const int      i   = (d == DeckRef::A) ? 0 : 1;
    const uint32_t now = _time ? _time->now_ms() : 0;
    // Debounce: the capacitive pads can glitch a single press into a release+touch pair, which would
    // toggle a deck straight back off. Drop a repeat toggle on the same deck within kDebounceMs.
    if (_time && now - _last_trig_ms[i] < kDebounceMs) return;
    _last_trig_ms[i] = now;
    _err_until[i]    = 0;                            // clear any stale flash on a fresh press
    _err_fmt[i]      = false;

    if (record) {
        if (_stream->is_recording(d))      _stream->stop(d);
        else if (!_stream->is_playing(d)) {
            if (!_stream->start_record(d, _path(d, _slot[i]))) _err_until[i] = now + kErrFlashMs;
            else _slot_used[i][_slot[i]] = true;   // the slot's file now exists
        }
    } else {
        if (_stream->is_playing(d))        _stream->stop(d);
        else if (!_stream->is_recording(d)) {
            if (!_stream->start_play(d, _path(d, _slot[i]))) {
                _err_until[i] = now + kErrFlashMs;
                _err_fmt[i]   = _stream->exists(_path(d, _slot[i]));  // file present but refused = bad format
            } else {
                _slot_used[i][_slot[i]] = true;   // it plays -> the slot is definitely loaded (keeps the cache honest)
            }
        }
    }
}

float TapeEngine::_pull(DeckRef::Ref d) {
    float f = 0.f;
    _stream->play_consume(d, reinterpret_cast<uint8_t*>(&f), sizeof(float));
    return f;
}

// Assign each deck a fresh random equal-power pan (the GenerativeStereo / RIGHT routing). Uses a small
// LCG so it needs no Math.random (unavailable here) and stays deterministic.
void TapeEngine::_roll_random_pans() {
    for (int i = 0; i < 2; i++) {
        _rng = _rng * 1664525u + 1013904223u;
        const float p = static_cast<float>(_rng >> 8) * (1.f / 16777216.f);  // [0,1)
        _rndL[i] = std::cos(p * kHalfPi);
        _rndR[i] = std::sin(p * kHalfPi);
    }
}

// Faded-loop seam envelope: ramp up over the first `f` frames and down over the last `f` of each loop
// (f bounded to ~kFadeFrames, and never more than 1/8 of the loop), so the wrap is not a click.
float TapeEngine::_fade_env(uint32_t pos, uint32_t L) {
    const uint32_t f = (L >= 8u * kFadeFrames) ? kFadeFrames : (L / 8u);
    if (f == 0) return 1.f;
    if (pos < f)     return static_cast<float>(pos) / static_cast<float>(f);
    if (pos > L - f) return static_cast<float>(L - pos) / static_cast<float>(f);
    return 1.f;
}

// Build the selected slot's path, e.g. "tapes/tape_a_1.wav", by hand (no printf - keeps the printf
// machinery out of the build). Single-digit slot keeps the name 8.3-safe; FatFile creates "tapes/".
const char* TapeEngine::_path(DeckRef::Ref d, int slot) {
    char* p = _pbuf;
    for (const char* s = "tapes/tape_"; *s; ) *p++ = *s++;
    *p++ = (d == DeckRef::A) ? 'a' : 'b';
    *p++ = '_';
    *p++ = static_cast<char>('1' + slot);   // slot 0..kSlots-1 -> '1'..
    for (const char* s = ".wav"; *s; ) *p++ = *s++;
    *p = '\0';
    return _pbuf;
}

// Probe each slot's file (f_stat via the stream) to mark recorded vs empty for the selector. Main-loop
// only (from prepare(), on selector-open) - 8 stats is cheap and rare, and the ring absorbs the latency.
void TapeEngine::_scan_slots(DeckRef::Ref d) {
    const int i = (d == DeckRef::A) ? 0 : 1;
    for (int s = 0; s < kSlots; s++) _slot_used[i][s] = _stream->exists(_path(d, s));
}

} // namespace spotykach
