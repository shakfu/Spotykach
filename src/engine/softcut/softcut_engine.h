#pragma once

#include "engine/iengine.h"
#include "engine/engine_params.h"
#include "engine/display_model.h"
#include "engine/istreamdeck.h"
#include "nocopy.h"

#include "softcut/Voice.h"   // vendored monome softcut-lib core (src/engine/softcut/vendor)

#include <cstddef>
#include <cstdint>

namespace spotykach {

// SoftcutEngine - a dual-deck crossfaded OVERDUB looper built on monome's softcut. Each deck holds two
// softcut voices (a Rev-swapped pair, like the shuttle engine), so there are FOUR voices in all. Unlike
// the shuttle/tape engines, softcut's read/write head is a single crossfaded head: it plays AND records
// the same loop at once, with subsample-accurate click-free loop crossfades and interpolated overdub.
// That overdub - layering live input onto a running loop with a feedback control - is the whole reason
// to port softcut; shuttle/tape cannot do sound-on-sound.
//
// Per FOCUSED track (knobs repoint on a Rev swap, platform reseeds the pickup):
//   PITCH  = rate, BIPOLAR about noon (noon = stop, CW = forward to +2x, CCW = reverse to -2x).
//   POS    = loop start (slides the loop window through the buffer).
//   SIZE   = loop length (a short stutter up to the whole buffer).
//   MIX    = track volume.
//   ENV    = OVERDUB FEEDBACK (softcut preLevel): 1.0 = infinite sound-on-sound layering, lower = old
//            layers decay as you add new ones, 0 = overwrite. Only acts while overdub is armed.
//   Alt+POS   = pan.
//   Alt+PITCH = SD tape slot (load a clip into the loop buffer; boot-preloads like shuttle).
//   MOD_AMT   = loop crossfade (fade) time - short = tight, long = lush/smeared loops.
//   MODFREQ   = rate slew time (glide on varispeed sweeps).
//   FLUX pad  = post filter: flux+PITCH = cutoff, flux+MIX = resonance (boots open -> sweep down).
//
// PADS: Play = roll/stop the focused track (snaps rate to unity on engage). Alt+Play = arm/disarm
// OVERDUB on the focused track (auto-starts the loop rolling if stopped). Rev = swap the deck's focused
// track. Seq = realign ALL voices to their loop start at once - a click-free crossfaded cut (softcut
// cutToPos), so drifted free-running loops snap back to a common downbeat (the v1 voice-sync gesture).
//
// ROUTING SWITCH (set_config Route, mirrors the panel L/C/R) - per-track pan into the stereo bus:
//   LEFT (DoubleMono) manual Alt+POS pan; CENTRE (Stereo) auto-spread a deck's two voices hard L/R;
//   RIGHT (GenerativeStereo) random pans re-rolled on entry.
//
// 4 voices is the hardware-measured safe budget (~62% avg / 79% peak worst case). A future 6-voice
// upgrade is two changes: land the std::function-per-sample removal in vendored Voice.cpp, then bump
// kTracks to 3. See docs/dev/softcut-spike.md.
class SoftcutEngine : public IEngine {
public:
    SoftcutEngine() = default;
    ~SoftcutEngine() override = default;

    void init(const EngineContext& ctx) override;
    void prepare() override;
    void process(const float* const* in, float** out, size_t size) override;

    Capabilities capabilities() const override {
        return CapOwnDisplay | CapRecording | CapDualDeck | CapAux | CapAltPos | CapPitchPickup;
    }

    void  set_param(ParamId id, DeckRef::Ref d, float v) override;
    float param(ParamId id, DeckRef::Ref d) const override;
    bool  set_config(ConfigId id, DeckRef::Ref, int value) override;     // routing switch -> per-track pan
    Route route() const override { return _route; }
    void  set_aux_active(DeckRef::Ref d, bool held) override;            // Alt held -> show slot selector
    void  set_mod_speed(DeckRef::Ref d, float v, bool sync) override;    // MODFREQ -> rate slew time
    bool  take_param_reseed(DeckRef::Ref d) override;                    // Play-snap / Rev-swap takeover

    bool  on_play_pad(DeckRef::Ref d, bool reverse) override;            // Play: roll+snap; Rev: swap track
    void  on_record_pad(DeckRef::Ref d, bool reverse) override;          // Alt+Play: record/overdub; Alt+Rev: save trimmed loop
    void  on_seq_trigger(DeckRef::Ref d) override;                       // Seq: realign ALL voices (sync)
    void  on_seq_toggle_arm(DeckRef::Ref d) override;                    // Alt+Seq (tap): save the full take to SD
    void  clear_sequence(DeckRef::Ref d) override;                       // Alt+Seq (hold ~1.5s): erase the voice

    void render(DisplayModel& m) override;

    static constexpr int kTracks = 2;   // voices per deck (the Rev-swapped pair). 2 -> 4 voices; 3 -> 6.

    // --- Test/host seams ---------------------------------------------------------------------------
    int      active_track(DeckRef::Ref d) const;
    bool     is_rolling(DeckRef::Ref d, int track) const;
    bool     is_overdubbing(DeckRef::Ref d, int track) const;
    bool     is_saving(DeckRef::Ref d) const;
    bool     is_empty(DeckRef::Ref d, int track) const;
    float    track_rate(DeckRef::Ref d, int track) const;
    float    loop_start_sec(DeckRef::Ref d, int track) const;
    float    loop_len_sec(DeckRef::Ref d, int track) const;

private:
    NOCOPY(SoftcutEngine)

    enum class Load : uint8_t { Idle, Priming, Draining };   // SD-into-RAM load state machine

    static int idx(DeckRef::Ref d) { return (d == DeckRef::A) ? 0 : 1; }

    void  _apply_window(int i, int s);             // POS/SIZE -> softcut loop start/end (seconds)
    void  _apply_filter(int i, int s);             // cutoff/reso knobs -> post SVF Fc/Rq
    float _extent_sec(int i, int s) const;         // loop-window span: loaded content, else full buffer
    float _loop_start_sec(int i, int s) const;     // current loop start (for cutToPos realign)
    void  _recompute_blend();
    void  _recompute_pan();
    void  _roll_random_pans();
    void  _request_load(DeckRef::Ref d, int s);
    void  _start_save(DeckRef::Ref d, bool full);  // full = whole take [0,_len]; else the POS/SIZE window
    const char* _path(DeckRef::Ref d, int slot);

    static float rate_from_knob(float v);          // knob 0..1 -> signed rate [-kMaxSpeed..+kMaxSpeed]
    static float knob_for_rate(float s);           // inverse for s in (0..kMaxSpeed]: the Play snap target

    static constexpr size_t   kMaxFrames  = 256;
    static constexpr uint32_t kBufFrames  = 1u << 19;  // power-of-2 (softcut requirement); 10.9 s = 2 MB
    static constexpr float    kMaxSpeed   = 2.f;
    static constexpr float    kDead       = 0.03f;     // capstan deadzone half-width -> guaranteed stop
    static constexpr float    kMinLoopSec = 0.05f;     // shortest loop window
    static constexpr float    kFadeMaxSec = 0.20f;     // MOD_AMT full -> 200 ms loop crossfade
    static constexpr float    kSlewMaxSec = 0.20f;     // MODFREQ full -> 200 ms rate slew
    static constexpr float    kFcMin      = 80.f;      // flux-pad post-filter cutoff range (Hz)
    static constexpr float    kFcSpan     = 250.f;     // Fc = kFcMin * kFcSpan^cutoff -> ~20 kHz at full
    static constexpr float    kRqOpen     = 4.0f;      // softcut Rq = reciprocal-Q: 4 = damped (no reso)
    static constexpr float    kRqSpan     = 0.05f;     // Rq = kRqOpen * kRqSpan^reso -> 0.2 at full reso
    static constexpr float    kHalfPi     = 1.57079632679f;
    static constexpr float    kCenterGain = 0.70710678f;
    static constexpr float    kMonGain    = 0.7f;       // dry input monitor level while overdubbing
    static constexpr uint32_t kErrColor   = 0xff6000;
    static constexpr uint32_t kErrFlashMs = 1200;
    static constexpr uint32_t kDebounceMs = 250;
    static constexpr int      kTapeSlots  = 8;
    static constexpr int      kRingLeds   = 32;
    static constexpr uint32_t kLoadChunk  = 8192;
    static constexpr uint32_t kSaveChunk  = 4096;       // frames the ISR offers the record ring per block
    static constexpr uint32_t kSaveArmMs  = 1600;       // Alt+Seq tap-save commit delay (> the 1500ms hold)
    static constexpr uint32_t kPreloadDeadlineMs = 3000;

    IStreamDeck*       _stream = nullptr;
    const ITimeSource* _time   = nullptr;
    float              _sr     = 48000.f;

    // ---- Per-track voice + state [deck][track] ----------------------------------------------------
    softcut::Voice _voice[2][kTracks];                  // softcut head: play+record+filters (SRAM state)
    float*   _buf[2][kTracks]   = {};                   // SDRAM mono loop buffers (audio only)
    uint32_t _cap              = 0;                     // frames per buffer (= kBufFrames if allocated)
    uint32_t _len[2][kTracks]  = {};                    // SD-loaded content frames (0 = use full buffer)

    bool  _rolling[2][kTracks]   = {};                  // softcut playFlag mirror
    bool  _overdub[2][kTracks]   = {};                  // softcut recFlag mirror (overdub onto content)
    bool  _defining[2][kTracks]  = {};                  // recording a FRESH loop; length set on the close

    float _rate_n[2][kTracks] = { { 0.5f, 0.5f }, { 0.5f, 0.5f } };  // PITCH knob (noon = stop)
    float _pos_n[2][kTracks]  = {};                                  // POS: loop start (0..1)
    float _size_n[2][kTracks] = { { 0.5f, 0.5f }, { 0.5f, 0.5f } };  // SIZE: loop length (1 = full extent;
                                                                     // boots at half so a fresh loop wraps
                                                                     // in a few seconds, not the whole buffer)
    float _gain[2][kTracks]   = { { 1.f, 1.f }, { 1.f, 1.f } };      // MIX: per-track volume
    float _fb_n[2][kTracks]   = { { 1.f, 1.f }, { 1.f, 1.f } };      // ENV: overdub feedback (preLevel)
    float _fade_n[2][kTracks] = { { 0.10f, 0.10f }, { 0.10f, 0.10f } }; // MOD_AMT: loop crossfade time
    float _slew_n[2][kTracks] = {};                                  // MODFREQ: rate slew time
    float _cut_n[2][kTracks]  = { { 1.f, 1.f }, { 1.f, 1.f } };      // flux+PITCH: post-filter cutoff (open)
    float _res_n[2][kTracks]  = {};                                  // flux+MIX: post-filter resonance
    float _pan[2][kTracks]    = { { 0.5f, 0.5f }, { 0.5f, 0.5f } };  // Alt+POS: manual pan
    float _rnd[2][kTracks]    = { { 0.5f, 0.5f }, { 0.5f, 0.5f } };  // GenerativeStereo random pans
    float _panL[2][kTracks]   = { { kCenterGain, kCenterGain }, { kCenterGain, kCenterGain } };
    float _panR[2][kTracks]   = { { kCenterGain, kCenterGain }, { kCenterGain, kCenterGain } };

    // ---- SD load (mirrors shuttle) ----------------------------------------------------------------
    uint32_t _wpos[2][kTracks]      = {};
    int      _tape_slot[2][kTracks] = {};
    Load     _load[2][kTracks]      = {};
    bool     _slot_used[2][kTapeSlots] = {};
    bool     _preload_armed   = true;
    bool     _preload_mounted = false;
    uint8_t  _preload_next[2] = { 0, 0 };

    // ---- SD save (loop buffer -> card via the streamed record path; per deck, like load) ----------
    enum class Save : uint8_t { Idle, Writing, Finalize };
    Save     _save[2]       = {};
    int      _save_voice[2] = { -1, -1 };   // which voice's loop is being written
    uint32_t _save_pos[2]   = {};           // next buffer frame to push
    uint32_t _save_end[2]   = {};           // one past the loop's last frame
    // Alt+Seq full-save is DEFERRED (Option B): armed on the tap, committed in prepare() once the
    // ~1.5s hold window has passed without an erase, so a hold-to-erase cancels it before it writes.
    bool     _save_arm[2]    = {};
    uint32_t _save_arm_ms[2] = {};

    // ---- Per-deck ---------------------------------------------------------------------------------
    int  _active[2]      = { 0, 0 };                    // focused track per deck (Rev swaps it)
    bool _want_reseed[2] = { false, false };
    bool _aux_held[2]    = { false, false };
    bool _rescan[2]      = { false, false };
    bool _slot_dirty[2]  = { false, false };   // Alt+PITCH selected a new slot; load it on release (in prepare)
    uint32_t _err_until[2]    = { 0, 0 };
    uint32_t _last_trig_ms[2] = { 0, 0 };
    int  _swap_show[2]   = { 0, 0 };

    // ---- Global -----------------------------------------------------------------------------------
    float _xfade = 0.5f;
    float _gA = 1.f, _gB = 1.f;
    Route _route = Route::DoubleMono;
    uint32_t _rng = 0x9e3779b9u;
    char  _pbuf[24];
};

} // namespace spotykach
