// SPDX-License-Identifier: GPL-3.0-only
//
// This engine is GPLv3, NOT MIT like the rest of this repository: it incorporates glitch_voice.h, whose
// algorithms are ported from the GPLv3 Noisferatu (https://github.com/rob-scape/noisferatu), so a build
// with ENGINE=glitch is a combined work distributed under GPLv3. See src/engine/glitch/{NOTICE.md,LICENSE}.
// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

#include "engine/iengine.h"
#include "engine/engine_params.h"
#include "engine/display_model.h"
#include "engine/glitch/glitch_voice.h"
#include "nocopy.h"

#include <cstddef>
#include <cstdint>

namespace spotykach {

// glitch - a dual-deck lo-fi / circuit-bent noise voice. Each deck (A/B) runs ONE of 12 curated
// algorithms ported from Rob Scape's Noisferatu (https://github.com/rob-scape/noisferatu): buffer-mangling glitch
// (sparse-glitch / wandering-window / address bit-mangle), logic-noise (tri XOR / square NAND / FM
// XOR / ring mod), generative scale blips (Phrygian / pentatonic / Bernoulli), and rhythmic noise
// (dust / clock-divided bursts). The two decks are independent voices, blended by the platform
// crossfader and placed by the routing switch - so it plays like a pair of glitch oscillators.
//
// Per-deck control map:
//   Alt+PITCH (Aux) -> ALGORITHM select (held selector, ring dots).
//   SIZE            -> param 1 (algorithm-specific: playback speed / osc 1 freq / trigger rate / density).
//   POS             -> param 2 (algorithm-specific: silence / osc 2 freq / walk rate / division / tone).
//   PITCH (Speed)   -> master pitch (+/- 2 octaves) for the pitched algorithms.
//   ENV             -> output tone (a one-pole low-pass tilt; lo-fi material gets harsh otherwise).
//   MIX             -> deck volume.   Crossfade -> A/B blend.   Routing switch -> stereo topology.
//   Play pad        -> regenerate the glitch buffer (new sparse pattern for the buffer-player algorithms).
//
// The audio ISR is thin: per sample it pulls one Voice::process() per deck, applies the tone low-pass and
// volume, and mixes to the stereo bus per the routing switch. No SD / arena / allocation - the two Voices
// (one ~8 KB glitch buffer each) live directly in the engine object.
class GlitchEngine : public IEngine {
public:
    GlitchEngine() = default;
    ~GlitchEngine() override = default;

    void init(const EngineContext& ctx) override;
    void prepare() override {}
    void process(const float* const* in, float** out, size_t size) override;

    Capabilities capabilities() const override { return CapOwnDisplay | CapDualDeck | CapAux; }

#if SPK_TERMINAL
    // Liveness masks for `describe` (docs/dev/terminal-dispatch.md): the ids this engine actually
    // consumes, so a host sweep exercises only real parameters instead of the whole ParamId enum.
    // Derived from the engine's own ParamId/ConfigId use; NOT yet verified on hardware.
    ParamMask live_params() const override {
        return (1u << static_cast<uint32_t>(ParamId::Pos))
             | (1u << static_cast<uint32_t>(ParamId::Size))
             | (1u << static_cast<uint32_t>(ParamId::Speed))
             | (1u << static_cast<uint32_t>(ParamId::Mix))
             | (1u << static_cast<uint32_t>(ParamId::Env))
             | (1u << static_cast<uint32_t>(ParamId::Aux))
             | (1u << static_cast<uint32_t>(ParamId::Crossfade));
    }
    ConfigMask live_configs() const override {
        return static_cast<ConfigMask>(1u << static_cast<uint32_t>(ConfigId::Route));
    }
    // Layer-3 names for `describe` (docs/dev/terminal-osc.md). The address stays the stable layer-2
    // slot, so one control-surface layout still binds to every build; only the printed name changes.
    // Twelve lo-fi algorithms. SIZE and POS are the algorithm's own two macros, so they are
    // named generically on purpose - their meaning changes with the selected algorithm and a
    // fixed label would be wrong eleven times out of twelve.
    const char* param_label(ParamId id) const override {
        switch (id) {
            case ParamId::Size:   return "param 1";
            case ParamId::Pos:    return "param 2";
            case ParamId::Speed:  return "pitch";
            case ParamId::Env:    return "tone";
            case ParamId::Mix:    return "volume";
            case ParamId::Aux:    return "algorithm";
            default: return nullptr;   // fall back to the layer-2 slot name
        }
    }

#endif

    void  set_param(ParamId id, DeckRef::Ref d, float v) override;
    float param(ParamId id, DeckRef::Ref d) const override;
    void  set_aux_active(DeckRef::Ref d, bool held) override;
    bool  set_config(ConfigId id, DeckRef::Ref, int value) override;
    Route route() const override { return _route; }

    bool  on_play_pad(DeckRef::Ref d, bool reverse) override;   // regenerate the glitch buffer

    void  render(DisplayModel& m) override;

private:
    NOCOPY(GlitchEngine)

    static constexpr int   kRingLeds   = 32;
    static constexpr float kCenterGain = 0.70710678f;

    int  _algo_index(DeckRef::Ref d) const;
    void _roll_random_pans();

    glitch::Voice _voice[2];

    // Per-deck cached control values (for param readback) and the output tone/volume state.
    float _p1[2]   = { 0.5f, 0.5f };
    float _p2[2]   = { 0.5f, 0.5f };
    float _pitch[2] = { 0.5f, 0.5f };
    float _aux[2]  = { 0.f, 0.f };     // ALGO selector knob position (0..1)
    float _tone[2] = { 1.f, 1.f };     // ENV -> one-pole low-pass coefficient (1 = open)
    float _lp[2]   = { 0.f, 0.f };     // tone filter state
    float _gain[2] = { 1.f, 1.f };     // MIX volume
    bool  _aux_held[2] = { false, false };

    Route _route = Route::Stereo;
    float _xfade = 0.5f, _gA = 1.f, _gB = 1.f;
    float _rndL[2] = { kCenterGain, kCenterGain };
    float _rndR[2] = { kCenterGain, kCenterGain };
    uint32_t _rng = 0x9e3779b9u;
};

} // namespace spotykach
