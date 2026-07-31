// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// In-RAM parameter snapshots for the terminal channel - the storage behind `preset save|load <slot>`.
//
// Deliberately NOT persistent. This is a test-channel facility: its job is "capture the state, perturb
// it, put it back", which a harness wants many times per run. Routing that through QSPI or the SD card
// would wear flash for no benefit and make a test run destructive.
//
// Params only. `IEngine` has `param()` to read a parameter back but there is no config getter -
// `set_config` is write-only - so configs cannot be snapshotted at all. That asymmetry is a gap in the
// engine contract rather than something the terminal can work around; see docs/dev/terminal-dispatch.md.

#include <cstdint>

#include "engine/engine_params.h"

#ifndef SPK_TERMINAL_PRESET_SLOTS
#define SPK_TERMINAL_PRESET_SLOTS 2
#endif

namespace spotykach {

struct PresetSlots {
    static constexpr uint8_t kSlots = SPK_TERMINAL_PRESET_SLOTS;

    struct Slot {
        bool  valid = false;   // false until a `preset save` has written it
        float v[static_cast<size_t>(ParamId::Count)][2] = {};   // [param][deck]
    };

    Slot slots[kSlots];
};

}  // namespace spotykach
