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

// reso - a dual resonator / pluck voice (engine #1 in docs/engine-ideas.md), built on the Mutable
// Instruments *Rings* DSP (thirdparty/rings) - the gold-standard physical-modelling resonator (modal
// bodies, sympathetic strings, plucked strings, FM). Each deck wraps one rings::Part (mono).
//
// The reel/slice/drift mode switch (ConfigId::Mode, per deck) selects how the resonator is excited;
// Alt+PITCH (ParamId::Aux, CapAux) selects the Rings model - two orthogonal axes:
//   Reel  - the resonator is fed CONTINUOUSLY (live input + internal noise): a sympathetic body / drone.
//   Slice - discrete PLUCKS: each trigger (pad / gate / MIDI / arp) strums Rings' internal exciter.
//   Drift - a scatter cloud: an internal scheduler auto-strums at randomized intervals/notes.
//
// Knob map (per deck): PITCH = note, SIZE = damping (decay), POS = position, ENV = brightness,
// MOD_AMT = structure, SOS = dry/wet, MODFREQ = Drift density / Slice arp rate, Alt+PITCH = model.
//
// PIMPL: all Rings/stmlib types live in reso_engine.cpp. The header must stay free of them because the
// composition root (app.cpp, via engine_select.h) includes it, and stmlib.h declares a global
// `namespace impl` that collides with app.cpp's `impl` instance. The Impl object (and the two ~108 KB
// Parts + 64 KB reverb buffers it owns) is placement-new'd in the injected SDRAM arena at init().
class ResoEngine : public IEngine {
public:
    ResoEngine() = default;
    ~ResoEngine() override = default;

    void init(const EngineContext& ctx) override;
    void prepare() override {}
    void process(const float* const* in, float** out, size_t size) override;

    Capabilities capabilities() const override {
        return CapOwnDisplay | CapDualDeck | CapAux | CapTransport;
    }

#if SPK_TERMINAL
    // Liveness masks for `describe` (docs/dev/terminal-dispatch.md). The impl's set_param caches EVERY
    // id into param_cache[] before its switch, so the default all-live mask would advertise all 24 and a
    // host sweep would "pass" on ids the switch drops - reading back its own write while the resonator
    // never moved. These are the ids the switch acts on (reso_engine.cpp:231-243): PITCH=note,
    // SIZE/POS/ENV/MODAMP=resonator shape, MIX, AUX=model select.
    ParamMask live_params() const override {
        return (1u << static_cast<uint32_t>(ParamId::Speed))
             | (1u << static_cast<uint32_t>(ParamId::Size))
             | (1u << static_cast<uint32_t>(ParamId::Pos))
             | (1u << static_cast<uint32_t>(ParamId::Env))
             | (1u << static_cast<uint32_t>(ParamId::ModAmp))
             | (1u << static_cast<uint32_t>(ParamId::Mix))
             | (1u << static_cast<uint32_t>(ParamId::Aux));
    }
    // Mode (Slice/Reel/Drift) is the only switch set_config acts on.
    ConfigMask live_configs() const override {
        return static_cast<ConfigMask>(1u << static_cast<uint32_t>(ConfigId::Mode));
    }
#endif

    void  set_param(ParamId id, DeckRef::Ref deck, float value) override;
    float param(ParamId id, DeckRef::Ref deck) const override;
    void  set_mod_speed(DeckRef::Ref deck, float value, bool sync) override;
    void  set_aux_active(DeckRef::Ref deck, bool active) override;
    bool  set_config(ConfigId id, DeckRef::Ref deck, int value) override;

    DeckRef::Ref handle_midi_note(uint8_t channel, uint8_t note) override;
    void  cv_voct(DeckRef::Ref deck, float value) override;
    void  on_gate_trigger(DeckRef::Ref deck) override;
    bool  on_play_pad(DeckRef::Ref deck, bool reverse) override;
    void  on_seq_trigger(DeckRef::Ref deck) override;

    void render(DisplayModel& m) override;

private:
    NOCOPY(ResoEngine)

    struct Impl;        // defined in reso_engine.cpp (owns the Rings DSP)
    Impl* _p = nullptr; // placement-new'd in the arena at init()
};

};
