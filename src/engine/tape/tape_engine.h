#pragma once

#include "engine/iengine.h"
#include "engine/engine_params.h"
#include "engine/display_model.h"
#include "engine/istreamdeck.h"
#include "nocopy.h"

#include <cstddef>
#include <cstdint>

namespace spotykach {

struct TapeFx; // defined in tape_engine.cpp: a per-deck Faust kernel (wow/flutter + J-A hysteresis)

// Dual streaming tape deck: two INDEPENDENT mono decks (A/B), each playing or recording its own
// arbitrarily long file on the SD card, bypassing the in-SDRAM loop-length cap. A deck is play-XOR-
// record (no overdub), so the two run like a pair of record decks - e.g. play deck A while recording
// deck B, then play both together and beat-match by ear with each deck's PITCH (varispeed).
//
// Audio I/O (per the hardware: two MONO inputs A/B, two MONO outputs, stereo headphone monitor):
//   - Deck A records INPUT A (in[0]); deck B records INPUT B (in[1]). Independent, never summed.
//   - The two decks are mixed to a stereo bus (out[0] = L, out[1] = R) that drives the headphone, and
//     the individual outputs tap the same bus. The routing switch + Alt+POS pan + mix fader place them:
//
//   ROUTING SWITCH (set_config ConfigId::Route, mirrors the panel L/C/R):
//     - LEFT  (DoubleMono):       each deck panned by ITS OWN Alt+POS knob (equal-power).
//     - CENTRE(Stereo):           both decks centered (summed equally to both outputs).
//     - RIGHT (GenerativeStereo): each deck at a random pan position (re-rolled on entering the mode).
//   MIX FADER (ParamId::Crossfade): blends deck A vs deck B in all modes (centre = both full).
//
// Controls: each deck's Play pad toggles playback, Alt+Play toggles recording; Alt+PITCH selects the
// tape slot (8 per deck, /tapes/tape_<a|b>_<n>.wav); MIX = volume; ENV = loop mode; Alt+POS = pan.
//
// The engine is thin - in process() (the audio ISR) it only moves float frames between the platform's
// lock-free per-deck rings and the audio buffers, then applies the (per-block) pan/blend gains; the slow
// FatFs I/O happens in the platform's StreamDeck pump (main loop).
class TapeEngine : public IEngine {
public:
    TapeEngine() = default;
    ~TapeEngine() override = default;

    void init(const EngineContext& ctx) override;
    void prepare() override;
    void process(const float* const* in, float** out, size_t size) override;

    Capabilities capabilities() const override { return CapOwnDisplay | CapDualDeck | CapAux | CapAltPos; }

    void  set_param(ParamId id, DeckRef::Ref d, float v) override;
    float param(ParamId id, DeckRef::Ref d) const override;
    void  set_mod_speed(DeckRef::Ref d, float v, bool sync) override; // MODFREQ -> tape wow/flutter rate
    void  set_aux_active(DeckRef::Ref d, bool held) override;   // Alt held -> show the slot selector
    bool  set_config(ConfigId id, DeckRef::Ref, int value) override;  // routing switch -> pan topology
    Route route() const override { return _route; }                   // mode L/C/R LED

    bool  on_play_pad(DeckRef::Ref d, bool reverse) override;   // play toggle (Alt+Play -> record)
    void  on_record_pad(DeckRef::Ref d, bool reverse) override; // record toggle

    void render(DisplayModel& m) override;

private:
    NOCOPY(TapeEngine)

    enum class Loop : uint8_t { None, Plain, Faded, Fripp };  // ENV-knob loop modes (CCW -> CW)

    // Fill `mono` with deck `d`'s output this block: varispeed playback, the live input on channel `ch`
    // while recording (also pushed to the record ring), or silence.
    void _render_deck(DeckRef::Ref d, const float* const* in, int ch, float* mono, size_t n);
    // Toggle play (record=false) or record (record=true) on deck `d` (play XOR record; debounced).
    void _toggle(DeckRef::Ref d, bool record);
    // Draw the transient knob-turn overlay for deck `i` (param-aware: value bar / selector / marker).
    void _render_edit(LEDRing& r, int i, uint32_t hue);
    float _pull(DeckRef::Ref d);          // one mono source frame from a deck's play ring (0 on underrun)
    void  _roll_random_pans();            // fresh random equal-power pans (GenerativeStereo routing)
    static float _fade_env(uint32_t pos, uint32_t L);  // faded-loop seam envelope
    const char* _path(DeckRef::Ref d, int slot);  // a slot's path, e.g. "tapes/tape_a_1.wav"
    void  _scan_slots(DeckRef::Ref d);    // f_stat each slot file -> _slot_used (recorded vs empty)

    static constexpr size_t   kMaxFrames  = 128;        // platform block is 96
    static constexpr uint32_t kErrFlashMs = 1200;       // how long the amber rejection flash lasts
    static constexpr uint32_t kDebounceMs = 300;        // ignore a same-deck retrigger within this
    static constexpr float    kHalfPi     = 1.57079632679f;
    static constexpr float    kCenterGain = 0.70710678f;  // equal-power centre (-3 dB), = cos/sin(pi/4)
    static constexpr uint32_t kFadeFrames = 2400;       // ~50 ms seam fade at 48 kHz (Faded loop)
    static constexpr float    kFrippDecay = 0.6f;       // per-pass gain multiplier (Frippertronics)
    static constexpr float    kFrippFloor = 0.02f;      // below this the loop has faded out -> stop
    static constexpr int      kSlots      = 8;          // tape slots per deck (single digit = 8.3-safe name)
    static constexpr uint32_t kEditShowMs = 700;        // how long a knob-turn value overlay lingers on the ring
    static constexpr float    kMeterDecay = 0.92f;      // per-block release of the output-level peak meter
    static constexpr uint32_t kBootScanMs = 4000;       // give the SD card this long to mount for the boot slot scan

    IStreamDeck*       _stream = nullptr;
    const ITimeSource* _time   = nullptr;

    Route _route = Route::DoubleMono;  // routing switch position (set each loop via set_config)

    // Per-deck varispeed playback resampler state (index 0 = deck A, 1 = deck B).
    float _speed[2]   = { 1.f, 1.f };    // source frames advanced per output frame
    float _speed_n[2] = { 0.5f, 0.5f };  // PITCH knob value (0.5 = unity) for param() readback
    float _phase[2]   = { 0.f, 0.f };    // fractional position in [0,1) between _cur and _next
    float _cur[2]     = { 0.f, 0.f };
    float _next[2]    = { 0.f, 0.f };
    bool  _primed[2]  = { false, false };

    // Per-deck pan (Alt+POS, equal-power) and the random-mode pan; mix-fader A/B blend gains.
    float _pan[2]  = { 0.5f, 0.5f };
    float _panL[2] = { kCenterGain, kCenterGain };
    float _panR[2] = { kCenterGain, kCenterGain };
    float _rndL[2] = { kCenterGain, kCenterGain };
    float _rndR[2] = { kCenterGain, kCenterGain };
    float _xfade = 0.5f;                  // mix-fader value (0.5 = both decks full)
    float _gA = 1.f, _gB = 1.f;           // mix-fader blend gains for deck A / deck B
    uint32_t _rng = 0x9e3779b9u;          // LCG state for the random-pan routing

    // Per-deck playback volume (MIX knob) and loop mode (ENV knob) + its shaping state.
    float _gain[2]   = { 1.f, 1.f };      // MIX knob: per-deck playback volume
    float _env_n[2]  = { 0.f, 0.f };      // ENV knob value (readback); selects the loop mode
    Loop  _loop_mode[2] = { Loop::None, Loop::None };
    uint32_t _src_pos[2]   = { 0, 0 };    // source-frame position within the current loop (fade/decay)
    float    _loop_gain[2] = { 1.f, 1.f };// Frippertronics per-pass decay gain
    bool     _want_stop[2] = { false, false }; // ISR -> prepare(): a Fripp loop faded out; stop it

    // Tape-slot selection (Alt+PITCH via ParamId::Aux) - one file per slot per deck, /tapes/tape_<d>_<n>.wav.
    int  _slot[2]     = { 0, 0 };          // selected slot per deck (0-indexed)
    bool _aux_held[2] = { false, false };  // Alt held -> show the slot selector on that deck's ring
    bool _slot_used[2][kSlots] = {};       // cache: each slot's file exists (recorded vs empty dot)
    bool _rescan[2]   = { false, false };  // selector just opened -> re-probe slots in prepare()
    bool _boot_scan   = true;              // re-probe all slots each prepare() until the card mounts (then rely on the cache)
    char _pbuf[20];                        // scratch for the current slot's path (built in _path)

    uint32_t _err_until[2]    = { 0, 0 };  // now_ms() deadline of each ring's error flash (0 = none)
    bool     _err_fmt[2]      = { false, false };  // that flash is a wrong-format reject (strobe), not a miss
    uint32_t _last_trig_ms[2] = { 0, 0 };  // now_ms() of each deck's last accepted toggle (debounce)

    // Display-only state, all read in render(). _peak is written in process() (audio ISR) - a benign
    // cross-thread scalar read for metering, like _read/_speed; the _edit_* trio is written in
    // set_param/set_mod_speed (same main-loop thread as render).
    float    _peak[2]       = { 0.f, 0.f };  // per-deck output-level peak meter (fed each audio block)
    uint32_t _edit_until[2] = { 0, 0 };      // now_ms() deadline of each deck's knob-turn value overlay
    float    _edit_val[2]   = { 0.f, 0.f };  // the 0..1 value that overlay shows (for the value-bar params)
    ParamId  _edit_param[2] = { ParamId::Speed, ParamId::Speed };  // which param the overlay is currently for

    // Per-deck tape FX (Faust kernel: wow/flutter + Jiles-Atherton hysteresis + post-FX resonant
    // low-pass), placement-new'd in the SDRAM arena at init(); only applied to the playback signal.
    // Knobs: POS=drive, SIZE=character, MOD_AMT=wow/flutter depth, MODFREQ=wow/flutter rate; the filter
    // rides the grit-modifier pad (held grit + PITCH=cutoff, held grit + MIX=resonance). _fx_n caches the
    // six 0..1 values per deck (order: drive, char, wow, rate, cutoff, reso) for param() readback.
    TapeFx* _fx[2] = { nullptr, nullptr };
    // Boot the colouring FX OFF (drive/char/wow/rate = 0): a non-zero default both colours the sound at
    // boot and, because the platform seeds the knob pickup from these, can soft-takeover-lock a param
    // above zero (a pot below the seed never crosses it, so it can't be turned down). Zero = neutral and
    // freely reducible. The filter is the inverse: cutoff boots OPEN (1.0) so the low-pass is inert until
    // swept down, and seeding the grit+PITCH pickup at 1.0 means you turn DOWN to engage it; reso boots 0.
    float   _fx_n[2][6] = { { 0.f, 0.f, 0.f, 0.f, 1.f, 0.f }, { 0.f, 0.f, 0.f, 0.f, 1.f, 0.f } };  // drive, char, wow, rate, cutoff, reso
};

} // namespace spotykach
