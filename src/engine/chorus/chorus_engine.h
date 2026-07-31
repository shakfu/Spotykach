// GENERATED from chorus.dsp + chorus.json by scripts/gen_faust_engine.py.
// Edit the manifest (chorus.json), not this file; then re-run the generator / `make faust-engine`.
// Chorus - stereo chorus
#pragma once
#include <cstdint>   // uint*_t (transitive-include hygiene; host build)

#include "engine/faust/faust_fx.h"
#include "engine/chorus/faust_kernel_chorus.h"

namespace spotykach {

struct ChorusEngineTraits {
    using Kernel = fx_chorus::mydsp;

    // Platform knob -> Faust slider (keyed by ParamId; the wrapper captures each slider's range from the
    // kernel and linear-maps the 0..1 knob into it).
    static const faustgen::Bind* binds() {
        static const faustgen::Bind b[] = {
            { nullptr, "rate", static_cast<int>(ParamId::ModSpeed), 0, false },  // -> ModSpeed
            { nullptr, "depth", static_cast<int>(ParamId::ModAmp), 0, false },  // -> ModAmp
            { nullptr, "delay", static_cast<int>(ParamId::Size), 0, false },  // -> Size
            { nullptr, "mix", static_cast<int>(ParamId::Mix), 0, false },  // -> Mix
        };
        return b;
    }
    static int nbinds() { return 4; }

    static constexpr Capabilities caps         = CapOwnDisplay;
    static constexpr int          decks        = 1;
    static constexpr int          wet_dry_role = -1;
    static constexpr bool         soft_limit   = false;
    static constexpr bool         meter        = true;
    static constexpr uint32_t     color        = 0x33ccff;
};

using ChorusEngine = FaustEngine<ChorusEngineTraits>;

} // namespace spotykach
