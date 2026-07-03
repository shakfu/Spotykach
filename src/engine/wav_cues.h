// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

#include <cstddef>
#include <cstdint>

namespace spotykach {

// Platform-parsed WAV cue markers, expressed as sample-frame offsets into the loaded audio (so they
// are bit-depth agnostic - the parser converts from the file's byte layout). The shared WAV loader
// (memory/wav.h find_cue_points) fills this, and the platform hands it to any engine advertising
// CapWavCues via IEngine::on_wav_cues. The engine assigns the meaning: granular slice starts, a
// shuttle's jump markers, softcut loop points, a slicer's pad regions, etc. Parsing is generic and
// engine-agnostic; only the interpretation is per-engine.
struct WavCues {
    static constexpr uint8_t kMax = 32;   // WAV `cue ` chunks are small; 32 covers musical use
    size_t  frames[kMax];
    uint8_t count = 0;
};

};
