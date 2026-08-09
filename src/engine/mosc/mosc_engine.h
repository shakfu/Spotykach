// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

#include "engine/iengine.h"
#include "engine/engine_params.h"
#include "engine/display_model.h"
#include "nocopy.h"

#include <cstddef>
#include <cstdint>

namespace spotykach {

// mosc - a dual macro-oscillator, built on the Mutable Instruments *Plaits* DSP (thirdparty/plaits) -
// the full 24-engine synthesis voice (virtual analog, waveshaping, FM, grain, additive, wavetable,
// chord, speech, swarm, noise, particle, string, modal, drums + the engine2 set: VA-VCF, phase
// distortion, 6-op FM, wave terrain, string machine, chiptune). Each deck wraps one plaits::Voice
// (mono out + aux), so the two decks are two independent macro-oscillators. Sibling of the reso engine
// (MI Rings); both are placement-new'd into the injected SDRAM arena and share the vendored stmlib.
//
// Knob map (per deck): PITCH = note, Alt+PITCH (Aux) = engine/model select, SIZE = harmonics,
// POS = timbre, ENV = morph, MOD_AMT = decay, MODFREQ = LPG colour, SOS/Mix = output level.
// CV map (per deck): V/Oct = note, CV_SIZE_POS = harmonics mod, CV_MIX = timbre mod (signed,
// ~0 when nothing patched, so the knobs rule until a cable is inserted).
// The Mode switch picks the trigger behaviour: Gate (pad/gate/MIDI strikes the LPG envelope) vs
// Drone (LPG bypassed, the engine runs open/continuous).
//
// PIMPL: all Plaits/stmlib types live in mosc_engine.cpp. The header must stay free of them because the
// composition root (app.cpp, via engine_select.h) includes it, and stmlib.h declares a global
// `namespace impl` that collides with app.cpp's `impl` instance (see reso_engine.h for the same trap).
// The Impl object (and the two plaits::Voice + their 16 KB scratch arenas) is placement-new'd in the
// injected SDRAM arena at init().
class MoscEngine : public IEngine {
public:
    MoscEngine() = default;
    ~MoscEngine() override = default;

    void init(const EngineContext& ctx) override;
    void prepare() override {}
    void process(const float* const* in, float** out, size_t size) override;

    Capabilities capabilities() const override {
        return CapOwnDisplay | CapDualDeck | CapAux;
    }

#if SPK_TERMINAL
    // Liveness masks for `describe` (docs/dev/terminal-dispatch.md). The impl's set_param caches EVERY
    // id into param_cache[] before its switch, so the default all-live mask would advertise all 24 and a
    // host sweep would "pass" on ids the switch drops - reading back its own write while the oscillator
    // never moved. These are the ids the switch acts on (mosc_engine.cpp): PITCH=note, SIZE=harmonics,
    // POS=timbre, ENV=morph, MODAMP=decay, MIX=level, AUX=Plaits engine select.
    //
    // ModSpeed (-> the Plaits `color` control) is bound but stays out: it is platform-owned, delivered
    // through set_mod_speed rather than set_param, and `describe` filters it either way.
    ParamMask live_params() const override {
        return (1u << static_cast<uint32_t>(ParamId::Speed))
             | (1u << static_cast<uint32_t>(ParamId::Size))
             | (1u << static_cast<uint32_t>(ParamId::Pos))
             | (1u << static_cast<uint32_t>(ParamId::Env))
             | (1u << static_cast<uint32_t>(ParamId::ModAmp))
             | (1u << static_cast<uint32_t>(ParamId::Mix))
             | (1u << static_cast<uint32_t>(ParamId::Aux));
    }
    ConfigMask live_configs() const override {
        return static_cast<ConfigMask>((1u << static_cast<uint32_t>(ConfigId::Route))
                                     | (1u << static_cast<uint32_t>(ConfigId::Mode)));
    }
    // Layer-3 names for `describe` (docs/dev/terminal-osc.md). The address stays the stable layer-2
    // slot, so one control-surface layout still binds to every build; only the printed name changes.
    // Mutable Instruments Plaits. The labels are Plaits' own front-panel names.
    const char* param_label(ParamId id) const override {
        switch (id) {
            case ParamId::Speed:   return "pitch";
            case ParamId::Size:    return "harmonics";
            case ParamId::Pos:     return "timbre";
            case ParamId::Env:     return "morph";
            case ParamId::ModAmp:  return "decay";
            case ParamId::Mix:     return "level";
            case ParamId::Aux:     return "engine";
            default: return nullptr;   // fall back to the layer-2 slot name
        }
    }

#endif

    void  set_param(ParamId id, DeckRef::Ref deck, float value) override;
    float param(ParamId id, DeckRef::Ref deck) const override;
    void  set_mod_speed(DeckRef::Ref deck, float value, bool sync) override;
    void  set_aux_active(DeckRef::Ref deck, bool active) override;
    bool  set_config(ConfigId id, DeckRef::Ref deck, int value) override;
    Route route() const override;   // global A/B->L/R routing (Stereo / DoubleMono / GenerativeStereo)

    DeckRef::Ref handle_midi_note(uint8_t channel, uint8_t note) override;
    void  cv_mix(DeckRef::Ref deck, float value) override;       // -> timbre modulation
    void  cv_size_pos(DeckRef::Ref deck, float value) override;  // -> harmonics modulation
    void  cv_voct(DeckRef::Ref deck, float value) override;
    void  on_gate_trigger(DeckRef::Ref deck) override;
    bool  on_play_pad(DeckRef::Ref deck, bool reverse) override;
    void  on_seq_trigger(DeckRef::Ref deck) override;

    void render(DisplayModel& m) override;

private:
    NOCOPY(MoscEngine)

    struct Impl;        // defined in mosc_engine.cpp (owns the Plaits DSP)
    Impl* _p = nullptr; // placement-new'd in the arena at init()
};

};
