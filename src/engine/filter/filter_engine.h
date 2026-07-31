// GENERATED from filter.dsp + filter.json by scripts/gen_faust_engine.py.
// Edit the manifest (filter.json), not this file; then re-run the generator / `make faust-engine`.
// Dual filter - independent resonant low-pass per deck
#pragma once
#include <cstdint>   // uint*_t (transitive-include hygiene; host build)

#include "engine/faust/faust_fx.h"
#include "engine/filter/faust_kernel_filter.h"

namespace spotykach {

struct FilterEngineTraits {
    using Kernel = fx_filter::mydsp;

    // Platform knob -> Faust slider (keyed by ParamId; the wrapper captures each slider's range from the
    // kernel and linear-maps the 0..1 knob into it).
    static const faustgen::Bind* binds() {
        static const faustgen::Bind b[] = {
            { nullptr, "cutoff", static_cast<int>(ParamId::Speed), 0, false },  // -> Speed
            { nullptr, "reso", static_cast<int>(ParamId::Pos), 0, false },  // -> Pos
            { nullptr, "drive", static_cast<int>(ParamId::Size), 0, false },  // -> Size
            { nullptr, "mix", static_cast<int>(ParamId::Mix), 0, false },  // -> Mix
        };
        return b;
    }
    static int nbinds() { return 4; }

    // decks=2: parallel DoubleMono - two instances of this mono kernel, deck A=left, deck B=right.
    static constexpr Capabilities caps         = CapOwnDisplay | CapDualDeck;
    static constexpr int          decks        = 2;
    static constexpr int          wet_dry_role = -1;
    static constexpr bool         soft_limit   = false;
    static constexpr bool         meter        = true;
    static constexpr uint32_t     color        = 0xff8833;
};

using FilterEngine = FaustEngine<FilterEngineTraits>;

} // namespace spotykach
