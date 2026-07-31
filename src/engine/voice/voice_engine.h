// GENERATED from voice.json (deck_mode=series) by scripts/gen_faust_engine.py.
// Edit the manifest (voice.json), not this file; then re-run the generator / `make faust-engine`.
// Voice - drone oscillator (deck A) into a resonant filter (deck B)
#pragma once
#include <cstdint>   // uint*_t (transitive-include hygiene; host build)

#include "engine/faust/faust_chain.h"
#include "engine/voice/faust_kernel_osc.h"
#include "engine/voice/faust_kernel_filter.h"

namespace spotykach {

struct VoiceEngineTraits {
    using StageA = fx_voice_osc::mydsp;   // deck A -> stage 1 (osc)
    using StageB = fx_voice_filter::mydsp;   // deck B -> stage 2 (filter)

    static const faustgen::Bind* binds_a() {
        static const faustgen::Bind b[] = {
            { nullptr, "freq", static_cast<int>(ParamId::Speed), 0, false },  // -> Speed
            { nullptr, "shape", static_cast<int>(ParamId::Size), 0, false },  // -> Size
            { nullptr, "level", static_cast<int>(ParamId::Mix), 0, false },  // -> Mix
        };
        return b;
    }
    static int nbinds_a() { return 3; }

    static const faustgen::Bind* binds_b() {
        static const faustgen::Bind b[] = {
            { nullptr, "cutoff", static_cast<int>(ParamId::Speed), 0, false },  // -> Speed
            { nullptr, "reso", static_cast<int>(ParamId::Pos), 0, false },  // -> Pos
            { nullptr, "drive", static_cast<int>(ParamId::Size), 0, false },  // -> Size
            { nullptr, "mix", static_cast<int>(ParamId::Mix), 0, false },  // -> Mix
        };
        return b;
    }
    static int nbinds_b() { return 4; }

    static constexpr Capabilities caps       = CapOwnDisplay | CapDualDeck;
    static constexpr bool         soft_limit = false;
    static constexpr bool         meter      = true;
    static constexpr uint32_t     color      = 0x88ff33;
};

using VoiceEngine = FaustChainEngine<VoiceEngineTraits>;

} // namespace spotykach
