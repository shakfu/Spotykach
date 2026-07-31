#pragma once

#include "engine/iengine.h"
#include "engine/engine_params.h"
#include "engine/display_model.h"
#include "engine/istreamdeck.h"
#include "nocopy.h"

#include <cstddef>
#include <cstdint>

namespace spotykach {

struct TapeFx; // shared FX kernel (wow/flutter + Jiles-Atherton + resonant LP), defined in engine/tape/tapefx.h

// ShuttleEngine - a buffer-based "varispeed shuttle" tape, the bipolar/reverse counterpart to the
// streaming TapeEngine. Two decks (A/B), and like the edrums kit each deck holds TWO tracks (a pair),
// so there are FOUR independent mono tape buffers in all. The Rev pad swaps which of a deck's two
// tracks is FOCUSED (edited + shown); the platform reseeds its pot pickup from param() on the swap, so
// the absolute knobs catch the newly-focused track without a jump. Both tracks of a deck keep playing
// and sum into that deck's audio - so each deck is a two-layer tape you flip between to tweak.
//
//   PITCH knob = capstan speed of the FOCUSED track, BIPOLAR about noon:
//     - noon (12 o'clock)  -> speed 0: the capstan is stopped, output is SILENCE (a small deadzone
//       about centre guarantees a reliable full stop).
//     - CW from noon  -> forward play, accelerating to +2x at fully CW.
//     - CCW from noon -> REVERSE play, accelerating to -2x at fully CCW.
//   Unity (+1x) lands off-centre (~3 o'clock) with no detent, so the Play pad SNAPS the focused track
//   to unity and reseeds the pickup (CapPitchPickup) so the pot must be swept across unity to retake
//   the speed. (This needs PITCH on the pickup-gated path for this engine; see CoreUI.)
//
// Per track the audio comes from RECORDing the live input (Alt+Play) or LOADing a tape slot from SD
// (Alt+PITCH selects the slot; the load drains the platform stream into RAM over a few main-loop
// passes). Playback then shuttles the in-RAM buffer bidirectionally - reverse and freeze are trivial
// random access, which is why this engine is buffer-based not SD-streamed (trade: a finite, RAM-capped
// tape length). At the buffer ends the read pointer WRAPS (a tape loop), both directions.
//
// Each track plays a LOOP WINDOW within its buffer, set by two knobs: POS = the window start (slides
// the loop through the recorded audio) and SIZE = the window length (full buffer down to a short
// stutter). The varispeed read pointer wraps within [start, start+length) instead of the whole buffer;
// the window always stays inside the recording (start slides over the unused tail as size shrinks).
//
// Per-track knobs (repoint on a Rev swap): PITCH=speed, MIX=volume, POS=loop start, SIZE=loop length,
// Alt+POS=pan, Alt+PITCH=tape slot. Per-DECK: none. Global: the MIX fader A/B blend.
//
// ROUTING SWITCH (set_config ConfigId::Route, mirrors the panel L/C/R) - the four tracks are panned
// individually into the stereo bus, and the switch picks where each track's pan comes from:
//   - LEFT  (DoubleMono):       each track at its own manual Alt+POS pan.
//   - CENTRE(Stereo):           auto-spread - a deck's two tracks hard L/R, for instant stereo width.
//   - RIGHT (GenerativeStereo): each track at a random pan (re-rolled on entering the mode).
// In CENTRE/RIGHT the manual Alt+POS pan is overridden (but retained, so LEFT restores it).
//
// The four tracks free-run at independent speeds, so they drift out of phase (the point - organic tape
// phasing). The SEQ pad re-aligns all four to their loop start at once (one atomic gesture), so they
// snap back to a common downbeat; from there they drift again. Either deck's Seq pad does the align.
class ShuttleEngine : public IEngine {
public:
    ShuttleEngine() = default;
    ~ShuttleEngine() override = default;

    void init(const EngineContext& ctx) override;
    void prepare() override;
    void process(const float* const* in, float** out, size_t size) override;

    Capabilities capabilities() const override {
        return CapOwnDisplay | CapDualDeck | CapAux | CapAltPos | CapPitchPickup;
    }

    void  set_param(ParamId id, DeckRef::Ref d, float v) override;
    float param(ParamId id, DeckRef::Ref d) const override;
    bool  set_config(ConfigId id, DeckRef::Ref, int value) override;  // routing switch -> per-track pan
    Route route() const override { return _route; }                   // mode L/C/R LED

#if SPK_TERMINAL
    // Liveness masks for `describe` (docs/dev/terminal-dispatch.md): the ids this engine actually
    // consumes, so a host sweep exercises only real parameters instead of the whole ParamId enum.
    // Derived from the engine's own ParamId/ConfigId use; NOT yet verified on hardware.
    ParamMask live_params() const override {
        return (1u << static_cast<uint32_t>(ParamId::Pos))
             | (1u << static_cast<uint32_t>(ParamId::Size))
             | (1u << static_cast<uint32_t>(ParamId::Speed))
             | (1u << static_cast<uint32_t>(ParamId::Mix))
             | (1u << static_cast<uint32_t>(ParamId::ModAmp))
             | (1u << static_cast<uint32_t>(ParamId::AltPos))
             | (1u << static_cast<uint32_t>(ParamId::Aux))
             | (1u << static_cast<uint32_t>(ParamId::Crossfade))
             | (1u << static_cast<uint32_t>(ParamId::FluxIntensity))
             | (1u << static_cast<uint32_t>(ParamId::FluxMix))
             | (1u << static_cast<uint32_t>(ParamId::GritIntensity))
             | (1u << static_cast<uint32_t>(ParamId::GritMix));
    }
    ConfigMask live_configs() const override {
        return static_cast<ConfigMask>(1u << static_cast<uint32_t>(ConfigId::Route));
    }
#endif
    void  set_aux_active(DeckRef::Ref d, bool held) override;   // Alt held -> show the slot selector
    void  set_mod_speed(DeckRef::Ref d, float v, bool sync) override; // MODFREQ -> FX wow/flutter rate
    bool  take_param_reseed(DeckRef::Ref d) override;           // Play-snap / Rev-swap soft-takeover

    bool  on_play_pad(DeckRef::Ref d, bool reverse) override;   // Play: roll+snap focused; Rev: swap track
    void  on_record_pad(DeckRef::Ref d, bool reverse) override; // Alt+Play: record the focused track
    void  on_seq_trigger(DeckRef::Ref d) override;              // Seq pad: re-align ALL four tracks to 0

    void render(DisplayModel& m) override;

    static constexpr int kTracks = 2;   // tracks per deck (the Rev-swapped pair) -> 4 buffers total

    // --- Test/host seams (address a specific track; the record path is fed through process()) ---
    int      active_track(DeckRef::Ref d) const;
    uint32_t buffer_frames(DeckRef::Ref d, int track) const;
    uint32_t loop_start(DeckRef::Ref d, int track) const;   // loop window start (frames) from POS/SIZE
    uint32_t loop_len(DeckRef::Ref d, int track) const;     // loop window length (frames) from SIZE
    float    read_position(DeckRef::Ref d, int track) const;
    float    track_speed(DeckRef::Ref d, int track) const;
    bool     is_rolling(DeckRef::Ref d, int track) const;
    bool     is_recording(DeckRef::Ref d, int track) const;

private:
    NOCOPY(ShuttleEngine)

    enum class Load : uint8_t { Idle, Priming, Draining };  // SD-into-RAM load state machine

    static int idx(DeckRef::Ref d) { return (d == DeckRef::A) ? 0 : 1; }

    void  _render_track(DeckRef::Ref d, int s, const float* const* in, int ch, float* mono, size_t n);
    void  _window(int i, int s, uint32_t& start, uint32_t& len) const;  // POS/SIZE -> loop window frames
    void  _recompute_blend();                      // crossfade -> per-deck A/B gains
    void  _recompute_pan();                        // route + pan positions -> per-track L/R gains
    void  _roll_random_pans();                     // fresh random equal-power pans (GenerativeStereo)
    void  _request_load(DeckRef::Ref d, int s);    // start an SD->RAM load of a track's selected slot
    const char* _path(DeckRef::Ref d, int slot);   // slot path, e.g. "shuttle/tape_a_1.wav"

    static float speed_from_knob(float v);         // knob 0..1 -> signed speed [-kMaxSpeed..+kMaxSpeed]
    static float knob_for_speed(float s);          // inverse for s in (0..kMaxSpeed]: the Play snap target

    static constexpr size_t   kMaxFrames  = 128;
    static constexpr float    kBufSeconds = 30.f;      // per-TRACK RAM tape length (4 of these ~= 23 MB)
    static constexpr float    kMaxSpeed   = 2.f;
    static constexpr float    kDead       = 0.03f;     // deadzone half-width about noon -> guaranteed stop
    static constexpr float    kHalfPi     = 1.57079632679f;
    static constexpr float    kCenterGain = 0.70710678f;
    static constexpr uint32_t kErrFlashMs = 1200;      // error/rejected-action flash duration (color = pal::kErr)
    static constexpr uint32_t kDebounceMs = 300;
    static constexpr int      kTapeSlots  = 8;         // SD files per deck (Alt+PITCH selector)
    static constexpr uint32_t kLoadChunk  = 8192;      // frames pulled from the stream per prepare() pass
    static constexpr int      kDeclickRamp = 64;       // realign fade half-length (~1.3 ms/side at 48 kHz)
    static constexpr uint32_t kMinLoopFrames = 64;     // shortest SIZE window (avoids a degenerate loop)
    static constexpr uint32_t kPreloadDeadlineMs = 3000; // stop retrying the boot preload after this (no card / empty)

    IStreamDeck*       _stream = nullptr;
    const ITimeSource* _time   = nullptr;

    // ---- Per-track state [deck][track] -------------------------------------------------------
    float*   _buf[2][kTracks]   = {};         // SDRAM mono buffers (4 of them)
    uint32_t _cap               = 0;          // capacity in frames per buffer (kBufSeconds * sr)
    uint32_t _len[2][kTracks]   = {};         // recorded/loaded frames available to play
    double   _read[2][kTracks]  = {};         // signed fractional read index in [0, _len)
    float    _speed[2][kTracks] = {};         // signed source frames per output frame (0 = stopped)
    float    _speed_n[2][kTracks] = { { 0.5f, 0.5f }, { 0.5f, 0.5f } };  // PITCH knob value readback
    float    _gain[2][kTracks]  = { { 1.f, 1.f }, { 1.f, 1.f } };        // MIX: per-track volume
    float    _pos_n[2][kTracks]  = {};                                  // POS knob: loop start (0..1)
    float    _size_n[2][kTracks] = { { 1.f, 1.f }, { 1.f, 1.f } };       // SIZE knob: loop length (1=full)
    bool     _rolling[2][kTracks]   = {};
    bool     _recording[2][kTracks] = {};
    uint32_t _wpos[2][kTracks]      = {};     // record / load write head
    int      _tape_slot[2][kTracks] = {};     // SD slot selected per track (0..kTapeSlots-1)
    Load     _load[2][kTracks]      = {};
    bool     _realign[2][kTracks]   = {};     // main-loop -> ISR: snap this track's read pointer to 0
    int      _declick[2][kTracks]   = {};     // realign declick: 2*kDeclickRamp..0, jump at kDeclickRamp
    float    _pan[2][kTracks]  = { { 0.5f, 0.5f }, { 0.5f, 0.5f } };  // Alt+POS: manual per-track pan (0..1)
    float    _rnd[2][kTracks]  = { { 0.5f, 0.5f }, { 0.5f, 0.5f } };  // GenerativeStereo random pan positions
    float    _panL[2][kTracks] = { { kCenterGain, kCenterGain }, { kCenterGain, kCenterGain } };  // effective L/R
    float    _panR[2][kTracks] = { { kCenterGain, kCenterGain }, { kCenterGain, kCenterGain } };  // (per route)

    // ---- Per-track tape FX (shared TapeFx kernel: wow/flutter + Jiles-Atherton saturation + post-FX
    // resonant low-pass), placement-new'd in the SDRAM arena at init(), applied to each track's mono
    // signal before the pan/sum. Driven from controls that are free on this engine (POS/SIZE are the
    // loop window here, so unlike the tape engine the FX does NOT use them): the GRIT pad modifier =
    // saturation (grit+PITCH=drive, grit+MIX=character), the FLUX pad modifier = filter (flux+PITCH=
    // cutoff, flux+MIX=resonance), MOD_AMT/MODFREQ = wow/flutter depth/rate. Like every other shuttle
    // knob the FX addresses the FOCUSED track; _fx_n caches the six 0..1 values per track (order:
    // drive, char, wow, rate, cutoff, reso) for param() readback + the Rev-swap pickup reseed.
    TapeFx* _fx[2][kTracks] = {};
    // Boot the colouring FX OFF (drive/char/wow/rate = 0) so it is neutral and every param is freely
    // reducible (a non-zero default would also soft-takeover-lock a pot sitting below the seed). The
    // filter is the inverse: cutoff boots OPEN (1.0) so the low-pass is inert until swept down (the
    // flux+PITCH pickup seeds open -> turn DOWN to engage); reso boots 0.
    float _fx_n[2][kTracks][6] = {
        { { 0.f, 0.f, 0.f, 0.f, 1.f, 0.f }, { 0.f, 0.f, 0.f, 0.f, 1.f, 0.f } },
        { { 0.f, 0.f, 0.f, 0.f, 1.f, 0.f }, { 0.f, 0.f, 0.f, 0.f, 1.f, 0.f } },
    };

    // ---- Per-deck state ----------------------------------------------------------------------
    int  _active[2]    = { 0, 0 };            // focused track per deck (Rev swaps it)
    bool _want_reseed[2] = { false, false };  // Play-snap or Rev-swap -> platform reseeds pickup
    bool _aux_held[2] = { false, false };
    bool _rescan[2]   = { false, false };
    // Boot preload: fill all four tracks from the card (track t of each deck <- slot t's file), serialized
    // per deck (one shared stream). `_armed` until done/deadline; `_mounted` once a probe confirms the
    // card is up; `_next[i]` = the next track to load on deck i (kTracks = that deck is finished).
    bool    _preload_armed   = true;
    bool    _preload_mounted = false;
    uint8_t _preload_next[2] = { 0, 0 };
    bool _slot_used[2][kTapeSlots] = {};      // SD file existence (shared by a deck's two tracks)
    uint32_t _err_until[2]    = { 0, 0 };
    uint32_t _last_trig_ms[2] = { 0, 0 };
    int  _swap_show[2] = { 0, 0 };            // frames left to flash the focused-track indicator
    // Knob-edit feedback: set_param stamps the moved knob's value + a short deadline here; render
    // shows it as a ring::value bar while live, so turning PITCH/POS/SIZE/MIX gives visual feedback
    // (previously the ring never reacted to a knob). Shuttle applies values immediately, so this is
    // the value bar only - the red pickup-deviation overlay is a granular-MValue concept and N/A here.
    float    _edit_val[2]     = { 0.f, 0.f };
    ParamId  _edit_param[2]   = { ParamId::Count, ParamId::Count };  // which knob moved -> how to show it
    uint32_t _edit_until[2]   = { 0, 0 };
    static constexpr uint32_t kEditShowMs = 700;

    // ---- Global ------------------------------------------------------------------------------
    float _xfade = 0.5f;
    float _gA = 1.f, _gB = 1.f;
    Route _route = Route::DoubleMono;     // routing switch position (set each loop via set_config)
    uint32_t _rng = 0x9e3779b9u;          // LCG state for the GenerativeStereo random pans
    char  _pbuf[24];   // holds "shuttle/tape_a_1.wav" + NUL
};

} // namespace spotykach
