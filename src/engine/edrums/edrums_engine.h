// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

#include "engine/iengine.h"
#include "engine/engine_params.h"
#include "engine/display_model.h"
#include "dsp/cpattern.h"
#include "dsp/lutsinosc.h"
#include "dsp/biquad.h"
#include "nocopy.h"

#include <cstddef>
#include <cstdint>

namespace spotykach {

// A dual Euclidean drum machine - the fourth engine, and the first to *sequence* off the shared
// platform transport (it subscribes to the clock's ticks rather than just reading tempo like the
// delay). Two independent drum tracks (deck A / deck B), each a Euclidean pattern (`dsp/CPattern`)
// driving a synthesized voice. Per-deck pattern length AND clock division make the two tracks run
// independently (polymeter); a transport grid reset realigns them to the bar.
//
// Audio is synthesized (no SD/samples): each Voice is a pitched sine body with a fast downward pitch
// sweep plus a band-passed noise component, shaped by an exponential amp envelope. Deck A defaults
// kick-ish (low, body), deck B snare/clap-ish (noise). The Voice is a small abstraction so sample
// playback could slot in later behind the same trigger()/process() seam.
//
// Knob map (the platform's per-deck direct knobs): POS = density (onsets), SIZE = length, ENV =
// rotation, PITCH = pitch + noise colour, SOS = decay, MOD_AMT = randomness (skip probability),
// MODFREQ = clock division. capabilities() = CapOwnDisplay | CapDualDeck.
class EdrumsEngine : public IEngine {
public:
    EdrumsEngine() = default;
    ~EdrumsEngine() override = default;

    void init(const EngineContext& ctx) override;
    void prepare() override;   // main-loop: debounced QSPI auto-save of the kit when dirty
    void process(const float* const* in, float** out, size_t size) override;

    Capabilities capabilities() const override { return CapOwnDisplay | CapDualDeck | CapAux; }

#if SPK_TERMINAL
    // Liveness masks for `describe` (docs/dev/terminal-dispatch.md). set_param stores EVERY id into
    // _param[] before its switch, so the default all-live mask would advertise all 24 and a host sweep
    // would "pass" on ids the switch drops - reading back the value it just wrote while nothing in the
    // engine changed. These are the ids the switch acts on (edrums_engine.cpp:536-554): POS=density,
    // SIZE=pattern length, ENV=rotation, SPEED=pitch, MIX=per-drum gain, MODAMP=onset probability,
    // AUX=model, plus the four grit/flux macros and grit decay.
    ParamMask live_params() const override {
        return (1u << static_cast<uint32_t>(ParamId::Pos))
             | (1u << static_cast<uint32_t>(ParamId::Size))
             | (1u << static_cast<uint32_t>(ParamId::Env))
             | (1u << static_cast<uint32_t>(ParamId::Speed))
             | (1u << static_cast<uint32_t>(ParamId::Mix))
             | (1u << static_cast<uint32_t>(ParamId::ModAmp))
             | (1u << static_cast<uint32_t>(ParamId::Aux))
             | (1u << static_cast<uint32_t>(ParamId::GritMix))
             | (1u << static_cast<uint32_t>(ParamId::GritIntensity))
             | (1u << static_cast<uint32_t>(ParamId::FluxIntensity))
             | (1u << static_cast<uint32_t>(ParamId::FluxMix))
             | (1u << static_cast<uint32_t>(ParamId::FluxFb));
    }
    // Route is the only switch set_config acts on; Mode is ignored here.
    ConfigMask live_configs() const override {
        return static_cast<ConfigMask>(1u << static_cast<uint32_t>(ConfigId::Route));
    }
    // Layer-3 names for `describe` (docs/dev/terminal-osc.md). The address stays the stable layer-2
    // slot, so one control-surface layout still binds to every build; only the printed name changes.
    // A drum sequencer: POS is onset density and SIZE is pattern length, so almost every slot
    // is renamed. The four flux/grit slots are the voice macros.
    const char* param_label(ParamId id) const override {
        switch (id) {
            case ParamId::Pos:            return "density";
            case ParamId::Size:           return "pattern length";
            case ParamId::Env:            return "rotation";
            case ParamId::Speed:          return "pitch";
            case ParamId::Mix:            return "level";
            case ParamId::ModAmp:         return "probability";
            case ParamId::Aux:            return "model";
            case ParamId::GritMix:        return "decay";
            case ParamId::GritIntensity:  return "drive";
            case ParamId::FluxIntensity:  return "pitch sweep";
            case ParamId::FluxMix:        return "body/noise";
            case ParamId::FluxFb:         return "brightness";
            default: return nullptr;   // fall back to the layer-2 slot name
        }
    }

#endif

    void  set_param(ParamId id, DeckRef::Ref deck, float value) override;
    float param(ParamId id, DeckRef::Ref deck) const override;
    void  set_mod_speed(DeckRef::Ref deck, float value, bool sync) override; // MODFREQ -> division
    bool  set_config(ConfigId id, DeckRef::Ref deck, int value) override;    // routing switch -> output mode
    Route route() const override { return _route; }                         // mode L/C/R LED
    void  set_aux_active(DeckRef::Ref deck, bool active) override;           // Alt+PITCH held -> model selector

    bool  on_play_pad(DeckRef::Ref deck, bool reverse) override;       // Play: stop/start deck; Rev: swap drum
    void  on_record_pad(DeckRef::Ref deck, bool reverse) override;     // Alt+Play: toggle the deck's mute
    bool  take_param_reseed(DeckRef::Ref deck) override;              // platform polls this after a swap
    void  clear_sequence(DeckRef::Ref deck) override;                 // hold Alt+Seq: reset this deck's drums to defaults

    void render(DisplayModel& m) override;

private:
    NOCOPY(EdrumsEngine)

    // One synthesized drum voice. process() runs per sample on the audio path; trigger() (from the
    // transport tick) just re-arms the envelopes, so it is allocation/lock free.
    struct Voice {
        LUTSinOsc osc;                 // pitched body
        LUTSinOsc osc2;                // 2nd body partial (snare/tom; silent when partial_mix = 0)
        infrasonic::BiquadSection noise_filt; // colours the noise: band-pass, or high-pass for the hat
        float sr          = 48000.f;
        float base_hz     = 60.f;      // PITCH (x base_scale) -> body frequency
        float pitch_norm  = 0.5f;      // last PITCH value, so set_model can rescale the base
        // Model character (set by set_model from kModels):
        float tone        = 0.f;       // body <-> noise balance (0 = body, 1 = noise)
        float sweep       = 3.f;       // pitch-drop amount (start freq = base*(1+sweep), decays to base)
        float base_scale  = 1.f;       // per-model frequency offset, so each drum tunes to its own range
        float decay_scale = 1.f;       // body amp-decay multiplier
        float noise_decay = 1.f;       // noise decay relative to the body (<1 snappier, >1 longer tail)
        float noise_mult  = 8.f;       // noise filter centre/corner = base_hz * this
        float noise_q     = 1.2f;      // noise filter Q
        bool  noise_hp    = false;     // high-pass the noise (hats) instead of band-pass it
        float partial_ratio = 0.f;     // 2nd body partial frequency ratio (0 = unused)
        float partial_mix   = 0.f;     // 2nd partial level
        float drive         = 0.f;     // body saturation amount (0 = clean)
        float click_level   = 0.f;     // attack-click amount (0 = none)
        // Live performance macros (the grit/flux modifier knobs): bipolar offsets on the model baseline,
        // 0.5 = neutral (the model exactly as voiced), so a fresh kit sounds unchanged until tweaked.
        float drive_macro  = 0.5f;     // grit+PITCH: saturation amount
        float sweep_macro  = 0.5f;     // flux+PITCH: pitch-drop amount (x the model's sweep)
        float tone_macro   = 0.5f;     // flux+SOS:   body <-> noise balance offset
        float bright_macro = 0.5f;     // flux+POS:   noise filter cutoff offset (+/- 2 octaves)
        float decay_norm  = 0.5f;      // last SOS value, so set_model can recompute the decay
        // Envelope + render state:
        float amp         = 0.f;       // body amp-env level (0 = idle)
        float amp_coef    = 0.f;       // body amp-env per-sample decay
        float namp        = 0.f;       // noise amp-env level (its own decay - snap vs ring)
        float namp_coef   = 0.f;       // noise amp-env per-sample decay
        float pitch_env   = 0.f;       // pitch-sweep level
        float pitch_coef  = 0.f;       // pitch-sweep per-sample decay (per-model time)
        float click_amp   = 0.f;       // attack-click env level
        float click_coef  = 0.f;       // attack-click per-sample decay (~2 ms)
        uint32_t rng      = 0x1234567u;// LCG state for the noise component
        // Multi-burst (the Clap model re-arms the envelopes a few times in quick succession).
        int   burst_count = 0;         // extra bursts after the first (model)
        float burst_gap   = 0.f;       // samples between bursts (model)
        int   burst_left  = 0;         // bursts still pending this hit
        float burst_timer = 0.f;       // samples until the next burst

        void  init(float sample_rate, float default_hz, int default_model, uint32_t seed);
        void  set_model(int idx);      // 0..4: kick / snare / clap / closed-hat / tom
        void  set_pitch(float norm);   // PITCH -> body freq + noise centre
        void  set_decay(float norm);   // grit+SOS -> body + noise decay times
        void  set_macro(int which, float v); // 0 drive, 1 sweep, 2 tone, 3 brightness (bipolar, 0.5 = neutral)
        void  trigger();               // re-arm amp/noise/pitch/click envelopes
        float process();               // render one sample
        void  _set_noise_filter();     // noise filter coeffs (centre/corner from base_hz, type per model)
        void  _recompute_pitch();      // base_hz from pitch_norm * base_scale; refresh the noise filter
        void  _recompute_decay();      // body + noise amp coefs from decay_norm * the model scales
    };

    // One drum track = a Euclidean pattern + its voice + a per-track randomness RNG.
    struct Track {
        CPattern pattern;
        Voice    voice;
        uint32_t prob_rng = 0xC0FFEEu;
    };

    static DeckRef::Ref _safe(DeckRef::Ref d) { return d < DeckRef::Count ? d : DeckRef::A; }

    void  _on_tick(const TransportTick& e);          // transport sink: step patterns, trigger voices
    void  _mark_dirty();                             // a kit param changed -> schedule a debounced QSPI save
    void  _apply_density(DeckRef::Ref d, int slot);  // re-derive onsets from the stored POS fraction + length
    // Fully seed one drum (voice + pattern + param cache) so it sounds without waiting on the platform.
    // Slot 1 of each deck is never touched by the platform's knob apply (which only writes the active
    // slot), so it must be self-sufficient from init; slot 0 is seeded the same way for symmetry.
    void  _init_slot(DeckRef::Ref d, int slot, float sr, int model, uint32_t seed,
                     float pos, float pitch, float decay);
    // Re-derive one drum's voice + pattern from its cached `_param` norms (+ _model/_gain/_prob/_div).
    // Shared by _init_slot and apply(), so a loaded preset reconstructs exactly like a fresh seed.
    void  _rebuild_slot(DeckRef::Ref d, int slot);
    static float _rand01(uint32_t& rng) { rng = rng * 1664525u + 1013904223u; return static_cast<float>(rng >> 8) * (1.f / 16777216.f); }

    // Four drums = two decks x two slots (item: 4-drum edrums). The platform stays 2-deck (DeckRef);
    // the slot lives only here. Both slots of a deck always sequence and sound; the Rev pad toggles
    // which slot _active_slot points at - that slot is the one the knobs edit and the ring shows.
    static constexpr int     kSlots           = 2;
    static constexpr int     kModelCount      = 5;  // kick / snare / clap / closed-hat / tom
    static constexpr uint8_t kFlashFrames     = 6;  // play-LED hold (~render frames) after a hit
    static constexpr uint8_t kModelShowFrames = 45; // ring shows the model number for ~0.7 s after a change
    static constexpr uint8_t kDivTable[3] = { 1, 2, 4 }; // MODFREQ -> ticks/step: 1/16, 1/8, 1/4
    static constexpr float   kBusTrim = 0.6f;       // pre-limiter trim: 4 voices sum hotter than 2 did

public:
    // Whole-kit preset: the 4 drums' voice + pattern params plus the route/focus, as a flat POD blob for
    // QSPI auto-persist. serialize()/apply() are pure (no I/O), so they are host-testable; the QSPI
    // read/write lives in the platform wiring. Bump kVersion on any layout change (an old blob is then
    // rejected -> defaults). Fields are the normalized 0..1 knob values (apply() re-derives all state).
    static constexpr char kKitSlug[4] = "edk";   // QSPI PersistentStorage slug for the edrums kit blob
    struct KitData {
        static constexpr uint8_t kVersion = 1;
        // PersistentStorage's StoreSettingsIfChanged needs operator!=. Always-true = "write whenever
        // Save() is called"; that is fine here because Save() only runs post-debounce on a real change.
        bool operator!=(const KitData&) const { return true; }
        uint8_t version = kVersion;
        uint8_t route   = 0;                          // Route enum as int (0 Stereo / 1 DoubleMono / 2 Generative)
        uint8_t active[DeckRef::Count] = { 0, 0 };    // focused slot per deck
        struct Drum {
            uint8_t model  = 0;
            float   pos    = 0.f,  size  = 1.f, env    = 0.f, pitch = 0.5f; // sequencer + pitch
            float   gain   = 0.8f, prob  = 1.f, div    = 0.f;               // gain, probability, division-norm
            float   decay  = 0.5f, drive = 0.5f, sweep = 0.5f, tone = 0.5f, bright = 0.5f; // envelope + macros
        } drum[DeckRef::Count][kSlots];
    };
    void serialize(KitData& kd) const;   // current state -> blob
    void apply(const KitData& kd);       // blob -> current state (rebuilds every drum + route/focus)

private:

    // Active drum per deck: _track[deck][_active_slot[deck]] is the one the knobs edit + the ring shows.
    int _slot(DeckRef::Ref d) const { return _active_slot[_safe(d)]; }

    ITransport* _transport = nullptr;          // platform clock (subscribe + tempo), injected at init
    const ITimeSource* _time = nullptr;        // for the auto-save debounce clock
    float       _sr = 48000.f;                 // sample rate, kept so clear_sequence can re-seed voices
    void*       _qspi = nullptr;               // opaque daisy::QSPIHandle* (target only); null = no persistence
    bool        _dirty = false;                // a kit param changed since the last save
    uint32_t    _dirty_since_ms = 0;           // now_ms() of the most recent change (debounce window start)
    static constexpr uint32_t kSaveDebounceMs = 1500; // save this long after the last change settles
    Track    _track[DeckRef::Count][kSlots];
    Route    _route = Route::Stereo;            // routing switch: Stereo=mono-sum, DoubleMono=A|B split, Generative=random pan
    uint32_t _step_tick = 0;                    // shared step counter (reset on e.reset); div phase
    uint8_t  _active_slot[DeckRef::Count]    = { 0, 0 }; // Rev pad toggles; selects the editable/shown slot
    bool     _reseed_pending[DeckRef::Count] = { false, false }; // set on swap; platform polls to re-seed knobs
    // Transport/performance state, NOT persisted (like the playback position); all boots running/unmuted.
    // Two scopes, deliberately different so they are not redundant: plain Play stops/starts the FOCUSED
    // drum (per slot, stays grid-locked); Alt+Play mutes the whole DECK (both drums, a broad gesture).
    bool     _running[DeckRef::Count][kSlots] = { { true, true }, { true, true } };
    bool     _muted[DeckRef::Count]           = { false, false };
    uint8_t  _div[DeckRef::Count][kSlots]   = { { 1, 1 }, { 1, 1 } };       // ticks per step per drum (MODFREQ)
    float    _prob[DeckRef::Count][kSlots]  = { { 1.f, 1.f }, { 1.f, 1.f } }; // probability an onset fires (MOD_AMT)
    float    _pan[DeckRef::Count][kSlots]   = { { 0.5f, 0.5f }, { 0.5f, 0.5f } }; // per-drum pan (Generative mode)
    uint8_t  _flash[DeckRef::Count][kSlots] = { { 0, 0 }, { 0, 0 } };       // per-drum play-LED hit flash
    uint8_t  _model[DeckRef::Count][kSlots]      = { { 0, 4 }, { 1, 3 } };  // A: kick/tom, B: snare/hat
    uint8_t  _model_show[DeckRef::Count][kSlots] = { { 0, 0 }, { 0, 0 } };  // frames left to show the model number
    bool     _aux_held[DeckRef::Count]           = { false, false };        // platform: Alt+PITCH held -> draw model selector
    float    _gain[DeckRef::Count][kSlots]       = { { 0.8f, 0.8f }, { 0.8f, 0.8f } }; // SOS knob: per-drum level
    float    _param[static_cast<size_t>(ParamId::Count)][DeckRef::Count][kSlots] = {};
};

};
