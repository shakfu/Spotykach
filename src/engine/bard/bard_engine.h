#pragma once

#include "engine/iengine.h"
#include "engine/engine_params.h"
#include "engine/display_model.h"
#include "engine/istreamdeck.h"
#include "engine/bard/bookmarks.h"
#include "engine/bard/resume_table.h"
#include "engine/bard/room.h"
#include "engine/bard/wsola.h"
#include "engine/arena.h"
#include "dsp/biquad.h"
#include "nocopy.h"

#include <cstddef>
#include <cstdint>

namespace spotykach {

// The STORYTELLER. Each deck (A/B) plays a spoken-word recording - an audiobook, a lecture, a radio play
// - from a shelf on the SD card ("/bard/0".."/bard/15", each a folder of 16-bit-mono .wav/.raw books) and
// navigates it by BOOKMARKS read from a plain-text sidecar next to the audio (NAME.TXT beside NAME.WAV).
//
// It reuses the radio engine's whole structure (the StreamDeck streaming service, scan_bank, the
// selector-knob hysteresis + settle guards) and INVERTS its defining behaviour. radio's signature is a
// free-running virtual playhead you cannot hold; a book needs an owned, resumable position. So:
//   - the playhead is tracked, seekable and persisted (radio's is derived from a global frame clock);
//   - the Play pad is PLAY/PAUSE (radio has no pause - a radio cannot pause);
//   - the Rev pad is JUMP BACK (inert on radio);
//   - the sidecar's LINE ORDER is a written-down recitation order (radio is deliberately non-deterministic).
//
// Per-deck control map (see docs/engines/bard.md for the full table):
//   PITCH (Speed)      -> BOOK select, summed with the V/oct CV jack.
//   POS   (Pos)        -> BOOKMARK select, summed with the size/pos CV jack.
//   SIZE  (Size)       -> RATE 0.5x..2.5x (varispeed: pitch follows speed). Unity at centre.
//   ENV   (Env)        -> PITCH-KEEP 0..1: at 0 the rate change is plain varispeed (pitch follows speed),
//                         at 1 it is WSOLA time-scaled with the pitch held. Output pitch = rate^(1-keep).
//   MIX   (Mix)        -> deck volume.   Crossfade -> A/B blend.   Route switch -> stereo topology.
//   Alt+PITCH (Aux)    -> SHELF select.  Alt+POS (AltPos) -> SCRUB.  Alt+SOS (Feedback) -> SEAM fade.
//   Cycle/Glow         -> duck release / duck depth (Mod Type = Follow ducks the OTHER deck).
//   Mode switch        -> SEQUENCE: Reel = Read, Slice = Recite, Drift = Wander.
//   Play pad           -> play/pause.        Alt+Play -> drop a session mark.
//   Tap-hold + Play    -> COMMIT the current mark list to the book's sidecar (the only other card write).
//   Rev pad            -> jump back 15 s.    Alt+Rev  -> re-roll the auto-marks.
//   Seq pad            -> next entry.        Alt+Seq  -> arm to the clock.  Alt+Seq held -> loop/hold.
//   Gate in            -> next bookmark.     Gate out -> a pulse per bookmark crossing.
//   Flux pad + PITCH/SOS -> VOICE COLOUR (drive + band-limit).
//   Grit pad + PITCH/SOS -> ROOM (plate / hall / slap; double-tap Grit cycles the character).
//
// Threading: process() is the audio ISR and only ever drains the per-deck ring (int16 -> float), runs the
// varispeed resampler, applies the seam fade / colour / duck, and mixes. EVERY seek, open, directory scan,
// sidecar read and resume write happens in prepare() on the main loop. Pads, gates and the transport tick
// only ever set request flags that prepare() acts on.
class BardEngine : public IEngine {
public:
    BardEngine() = default;
    ~BardEngine() override = default;

    void init(const EngineContext& ctx) override;
    void prepare() override;
    void process(const float* const* in, float** out, size_t size) override;

    // NOTE: no CapStepSequencer. The Seq-pad hooks (on_seq_trigger / on_seq_toggle_arm / clear_sequence)
    // are called unconditionally by src/ui/core.ui.pads.cpp, so bard gets its three Seq gestures without
    // advertising a step sequencer it does not have.
    Capabilities capabilities() const override {
        return CapOwnDisplay | CapDualDeck | CapAux | CapAltPos | CapTransport;
    }

    void  set_param(ParamId id, DeckRef::Ref d, float v) override;
    float param(ParamId id, DeckRef::Ref d) const override;
    void  set_mod_speed(DeckRef::Ref d, float value, bool sync) override;
    void  set_aux_active(DeckRef::Ref d, bool held) override;
    bool  set_config(ConfigId id, DeckRef::Ref d, int value) override;
    Route route() const override { return _route; }

    void  cv_voct(DeckRef::Ref d, float value) override;      // BOOK CV
    void  cv_size_pos(DeckRef::Ref d, float value) override;  // BOOKMARK CV
    void  cv_mix(DeckRef::Ref d, float value) override;       // volume CV
    void  cv_crossfade(float value) override;

    void  set_fx(DeckRef::Ref d, FxKind k, bool on) override;
    void  toggle_fx_lock(DeckRef::Ref d, FxKind k) override;
    GritReseed toggle_grit_mode(DeckRef::Ref d) override;        // cycle the room: plate / hall / slap

    void  stop_if_generating(DeckRef::Ref d) override;          // tap-hold Play: commit marks to the sidecar
    bool  on_play_pad(DeckRef::Ref d, bool reverse) override;   // play/pause | jump back
    void  on_record_pad(DeckRef::Ref d, bool reverse) override; // drop mark  | re-roll auto-marks
    void  on_seq_trigger(DeckRef::Ref d) override;              // next entry in sequence order
    void  on_seq_toggle_arm(DeckRef::Ref d) override;           // arm segment advance to the clock
    void  clear_sequence(DeckRef::Ref d) override;              // toggle loop/hold at a segment end

    void  on_gate_trigger(DeckRef::Ref d) override;             // next bookmark
    bool  gate_out_triggered(DeckRef::Ref d) override;          // a pulse per bookmark crossing

    void  process_cv(float* cv0, float* cv1, size_t n) override;
    void  render(DisplayModel& m) override;

private:
    NOCOPY(BardEngine)

    enum class Seq : uint8_t { Read, Recite, Wander };

    static constexpr int      kMaxShelves = 16;      // "/bard/0".."/bard/15"
    static constexpr int      kMaxBooks   = 32;      // books per shelf
    static constexpr size_t   kMaxFrames  = 128;     // platform block is 96
    static constexpr int      kRingLeds   = 32;
    static constexpr int      kTextMax    = 4096;    // sidecar cap (see bookmarks.h) / shared text scratch
    static constexpr uint32_t kErrColor   = 0xff2000;
    static constexpr uint32_t kErrFlashMs = 1200;
    static constexpr uint32_t kBootScanMs = 5000;    // the card mounts ~1 s in; keep retrying the scan
    static constexpr uint32_t kDebounceMs = 180;     // pad / gate retrigger debounce
    static constexpr uint32_t kSettleMs   = 180;     // selector settle (radio's anti-stutter guard)
    static constexpr float    kSelHyst    = 0.25f;   // selector deadband, in fractions of one step
    static constexpr uint32_t kJumpBackSec = 15;
    static constexpr uint32_t kCheckpointMs = 30000; // resume write cadence while playing
    static constexpr float    kSeamMaxMs  = 500.f;
    static constexpr float    kHalfPi     = 1.57079632679f;
    static constexpr float    kCenterGain = 0.70710678f;
    static constexpr float    kDuckKnee   = 4.f;     // envelope -> duck curve steepness

    // --- audio ------------------------------------------------------------------------------------
    void  _render_deck(DeckRef::Ref d, float* mono, size_t n);
    bool  _pull(DeckRef::Ref d, float& out);         // one source frame; false on ring underrun
    float _colour(int i, float x);                   // Flux voice colour (drive + band-limit)
    void  _update_rate_chain(int i);                 // RATE + PITCH-KEEP -> resampler step & WSOLA scale

    // --- main loop (FatFs) ------------------------------------------------------------------------
    void  _load_config();
    void  _load_resume();
    void  _save_resume(uint32_t now);
    void  _rescan_shelf(int i);
    void  _open_book(DeckRef::Ref d, int book, uint32_t now, bool use_resume);
    bool  _seek(DeckRef::Ref d, uint32_t frame);     // re-open the current book at `frame`
    void  _load_marks(int i);
    void  _apply_selectors(DeckRef::Ref d, uint32_t now);
    void  _advance(DeckRef::Ref d, int steps);       // step the play order (Wander / Seq pad / gate)
    void  _enter_segment(int i, int mark_idx);
    void  _request_jump(int i, uint32_t frame);
    const char* _shelf_dir(int i);
    const char* _book_path(int i, int book);
    const char* _sidecar_path(int i, int book);
    const char* _resume_key(int i, int book);

    IStreamDeck*       _stream = nullptr;
    const ITimeSource* _time   = nullptr;
    ITransport*        _transport = nullptr;

    Route _route = Route::Stereo;

    // Card-side config + the persisted resume table (one shared table, LRU over the 64 most recent books).
    bard::Config      _cfg;
    bard::ResumeTable _resume;
    bool     _cfg_loaded    = false;
    bool     _resume_loaded = false;
    bool     _resume_dirty  = false;
    bool     _resume_writable = true;    // cleared for the session on the first failed write
    uint32_t _resume_ms     = 0;

    // Per-deck shelf index.
    bard::MarkList _marks[2];
    BankEntry _books[2][kMaxBooks];
    int  _nbooks[2]   = { 0, 0 };
    int  _shelf[2]    = { 0, 0 };
    bool _rescan[2]   = { true, true };
    int  _open_book_i[2] = { -1, -1 };   // book currently open (-1 = none)
    int  _pending_book[2] = { -1, -1 };
    uint32_t _pending_book_ms[2] = { 0, 0 };

    // The open book: length, source rate, and the OWNED playhead (source frames).
    uint32_t _book_frames[2] = { 0, 0 };
    uint32_t _src_rate[2]    = { 48000, 48000 };
    float    _rate_ratio[2]  = { 1.f, 1.f };
    volatile uint32_t _pos[2] = { 0, 0 };   // written by the ISR, read by the main loop (single 32-bit
                                            // aligned word -> no torn read on the M7; may be one block stale)
    bool     _paused[2] = { true, true };

    // Sequence state.
    Seq  _seq[2]        = { Seq::Read, Seq::Read };
    int  _seg[2]        = { -1, -1 };       // current mark index (-1 = none / whole book)
    uint32_t _seg_end[2] = { 0, 0 };        // frame the current segment ends at
    int  _pending_seg[2] = { -1, -1 };
    uint32_t _pending_seg_ms[2] = { 0, 0 };
    bool _armed[2]      = { false, false }; // advance on the transport key boundary
    bool _loop_seg[2]   = { false, false }; // segment-end policy: true = loop, false = hold
    bool _tick_advance[2] = { false, false };// set by the transport tick, consumed in prepare()
    uint32_t _reroll[2] = { 0, 0 };

    // Pending explicit jump (pads / gate / scrub / selector), applied in prepare().
    bool     _req[2]       = { false, false };
    uint32_t _req_frame[2] = { 0, 0 };
    // Pad/gate debounce. The "seen" flags matter: now_ms() is ~0 at boot, so a bare `now - last < window`
    // test would swallow the very first press of a session.
    uint32_t _last_pad_ms[2] = { 0, 0 };    bool _pad_seen[2]  = { false, false };
    uint32_t _last_gate_ms[2] = { 0, 0 };   bool _gate_seen[2] = { false, false };
    bool     _gate_out[2]  = { false, false };
    bool     _rescan_marks[2] = { false, false };
    bool     _commit_marks[2] = { false, false };  // tap-hold Play: write the sidecar back, from prepare()
    uint32_t _commit_flash[2] = { 0, 0 };          // ring/LED confirmation window after a commit
    int      _seen_mark[2] = { -1, -1 };    // last mark the playhead was inside (gate-out edge detect)

    // Knob + CV caches (0..1).
    float _book_n[2]  = { 0.f, 0.f };   float _book_cv[2] = { 0.f, 0.f };
    float _mark_n[2]  = { 0.f, 0.f };   float _mark_cv[2] = { 0.f, 0.f };
    // The BOOKMARK selector acts only when the knob (or its CV) actually MOVES, not whenever it disagrees
    // with the current segment. Anything else that changes the segment - the Seq pad, the gate, an armed
    // clock tick, Wander's auto-advance - immediately puts the idle knob "out of agreement", and a selector
    // that re-quantized every main loop would drag the playhead back and silently undo the advance. The
    // BOOK selector needs no such guard: nothing but that knob ever changes which book is open.
    float _mark_x[2]  = { -1.f, -1.f };  // last knob+CV position the selector acted on (-1 = unseeded)
    bool  _mark_moved[2] = { false, false };
    float _size_n[2]  = { 0.5f, 0.5f }; float _rate[2]    = { 1.f, 1.f };
    float _env_n[2]   = { 0.f, 0.f };   // PITCH-KEEP amount (0 = varispeed, 1 = pitch held)
    float _gain_n[2]  = { 1.f, 1.f };   float _mix_cv[2]  = { 0.f, 0.f };
    float _shelf_n[2] = { 0.f, 0.f };
    float _scrub_n[2] = { 0.f, 0.f };   bool  _scrub_touched[2] = { false, false };
    uint32_t _scrub_ms[2] = { 0, 0 };
    float _seam_n[2]  = { 0.f, 0.f };
    float _duck_n[2]  = { 0.f, 0.f };   float _duck_rel_n[2] = { 0.5f, 0.5f };
    bool  _follow[2]  = { false, false };  // Mod Type: false = LFO (unused target), true = Follow (duck)

    // The rate chain, recomputed only when RATE or PITCH-KEEP moves (both main-loop writes) so the ISR
    // never pays for a powf: resample by k = rate^(1-keep), then WSOLA time-scale by alpha = rate^-keep.
    // Composed, that is speed x rate and pitch x rate^(1-keep) - so keep=0 is pure varispeed (and WSOLA
    // bypasses bit-exactly) while keep=1 changes speed with the pitch untouched.
    float _res_k[2]  = { 1.f, 1.f };
    float _wsola_a[2] = { 1.f, 1.f };
    bard::Wsola _wsola[2];

    // Varispeed resampler (2-frame linear interp), per deck.
    float _phase[2] = { 0.f, 0.f };
    float _cur[2]   = { 0.f, 0.f };
    float _next[2]  = { 0.f, 0.f };
    bool  _primed[2] = { false, false };

    // Seam: a short fade-in after every jump. The ring is flushed by a re-open, so there is no old
    // stream left to cross-fade against - this is a declick ramp, not a true crossfade.
    float _seam_gain[2] = { 1.f, 1.f };
    float _seam_inc[2]  = { 1.f, 1.f };

    // Duck (Mod Type = Follow): each deck's block envelope attenuates the OTHER deck.
    float _env_follow[2] = { 0.f, 0.f };
    float _duck_gain[2]  = { 1.f, 1.f };

    // Flux voice colour: drive + a band-limiting HPF/LPF pair per deck.
    infrasonic::HPF12 _hp[2];
    infrasonic::LPF12 _lp[2];
    bool  _flux_held[2] = { false, false };
    bool  _flux_lock[2] = { false, false };
    float _flux_int[2]  = { 0.f, 0.f };
    float _flux_mix[2]  = { 0.f, 0.f };
    float _colour_drive[2] = { 1.f, 1.f };

    // Grit ROOM: one mono room per deck, delay lines sub-allocated from the SDRAM arena. MIT, written from
    // the classic Schroeder/Moorer structure rather than reusing the GPLv3 src/dsp/diffuser.h - see room.h.
    bard::Room _room[2];
    bool  _grit_held[2] = { false, false };
    bool  _grit_lock[2] = { false, false };
    float _grit_int[2]  = { 0.f, 0.f };
    float _grit_mix[2]  = { 0.f, 0.f };

    // Crossfade + routing.
    float _xfade = 0.5f, _gA = 1.f, _gB = 1.f, _xfade_cv = 0.f;
    float _rndL[2] = { kCenterGain, kCenterGain };
    float _rndR[2] = { kCenterGain, kCenterGain };

    bool     _aux_held[2]  = { false, false };
    uint32_t _err_until[2] = { 0, 0 };
    uint32_t _rng = 0x9e3779b9u;

    char _text[kTextMax];   // shared main-loop scratch: sidecar / bard.cfg / resume.txt (never concurrent)
    char _dbuf[24];         // "bard/<shelf>"
    char _pbuf[48];         // "bard/<shelf>/<name>"
    char _kbuf[24];         // "<shelf>/<NAME.WAV>" resume key
};

} // namespace spotykach
