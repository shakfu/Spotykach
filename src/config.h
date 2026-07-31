#pragma once
#include <cstddef>   // size_t (transitive-include hygiene; host build)

#include <string.h>
#include <stdint.h>
#include <algorithm>

namespace spotykach {

class Config
{
public:
    struct Values {
        uint8_t midi_channel_a = 0; // Actual 1
        uint8_t midi_channel_b = 1; // Actual 2
        uint8_t midi_play_stop_a = 0;
        uint8_t midi_play_stop_b = 0;
        bool is_preload_on = true;
    };

    uint8_t midi_channel_a() const { return _vals.midi_channel_a; }
    uint8_t midi_channel_b() const { return _vals.midi_channel_b; }
    uint8_t midi_play_stop_a() const { return _vals.midi_play_stop_a; }
    uint8_t midi_play_stop_b() const { return _vals.midi_play_stop_b; }
    bool is_preload_on() const { return _vals.is_preload_on; }

    bool is_loaded() const { return _is_loaded; }

    void fill(const uint8_t* data, const size_t size)
    {
        if (data == nullptr || size == 0) return;

        constexpr int line_size = 8;   // fixed bound (clang 21 rejects the non-const VLA form)
        char prop[line_size];
        size_t cursor = 0;
        while (cursor < size) {
            char line[line_size];
            int8_t line_idx = -1;

            // Read the line
            while (cursor < size && data[cursor] != '\n' && data[cursor] != '\r') {
                if (data[cursor] != ' ') {
                    line_idx ++;
                    if (line_idx < line_size) line[line_idx] = data[cursor];
                }
                cursor++;
            }

            while (cursor < size && (data[cursor] == '\n' || data[cursor] == '\r')) {
                cursor++;
            }

            if (line_idx < 0) continue;

            auto is_numeric = true;
            for (int8_t j = 0; j <= line_idx; j++) {
                if (j == 0 && line[j] == '-') {
                    continue;
                }
                if (line[j] < '0' || line[j] > '9') {
                    is_numeric = false;
                    break;
                }
            }

            if (!is_numeric) {
                memcpy(prop, line, line_size);
                continue;
            }

            int32_t val = 0;
            int32_t sign = 1;
            size_t start = 0;

            if (line[0] == '-') {
                sign = -1;
                start = 1;
            }

            for (int8_t j = start; j <= line_idx; j++) {
                val = val * 10 + (line[j] - '0');
            }
            val *= sign;

                 if (memcmp(prop, "mid_ch_a", line_size) == 0) { _vals.midi_channel_a = val - 1; _is_loaded = true; }
            else if (memcmp(prop, "mid_ch_b", line_size) == 0) { _vals.midi_channel_b = val - 1; _is_loaded = true; }
            else if (memcmp(prop, "mid_ps_a", line_size) == 0) { _vals.midi_play_stop_a = val;   _is_loaded = true; }
            else if (memcmp(prop, "mid_ps_b", line_size) == 0) { _vals.midi_play_stop_b = val;   _is_loaded = true; }
            else if (memcmp(prop, "pre_load", line_size) == 0) { _vals.is_preload_on = val;      _is_loaded = true; }
        }
    }

    static Config& dynamic()
    {
        static Config instance;
        return instance;
    }
    Config(Config const&)           = delete;
    void operator=(Config const&)   = delete;

private:
    Config() {}
    Values _vals;
    bool _is_loaded = false;
};


// STATIC CONFIG ///////////////////////////////////////////////
// Clock ........................................
static constexpr uint8_t kPPQNIntern = 48;

// Buffer
static constexpr size_t kRecordFade = 192; // 4ms

// Grain ........................................
static constexpr size_t kWindowSlope = 960; //20ms @ 48K 1x
static constexpr size_t kMinimumWindowSize = 2 * kWindowSlope; //40ms @ 48k 1x
static constexpr size_t kDefaultWindowSize = 2880; //60ms @ 48k 1x

// Slice ........................................
static constexpr size_t kSliceSlope = 192; //4ms
static constexpr size_t kSliceMinSize = 2 * kSliceSlope + 960; //+20ms sustain @ 48K 1x

// LFO ..........................................
static constexpr float kLFOFreqMin = .01f;
static constexpr float kLFOFreqRange = 11.99f;

// Overdub ......................................
static constexpr float kDefaultFeedback = 0.95f; //-3db at -60...0dB scale

// Drift .........................................
static constexpr float kDriftStartOffsetL1  = .08f;
static constexpr float kDriftStartOffsetL2  = .15f;
static constexpr float kDriftStartKofL1     = 1.42f;
static constexpr float kDriftStartKofL2     = 1.85f;
static constexpr float kDriftSizeKofL1      = .62f;
static constexpr float kDriftSizeKofL2      = .38f;

// Slice points ..................................
static constexpr uint8_t kMaxSlicePointCount = 32;

// Loop buffer length, in seconds (the granular engine sizes its per-deck source buffer from this and
// the sample rate). Moved here from the SDRAM pool so the engine owns its buffer sizing, not the HAL.
// 16-bit storage halves bytes/frame, so the same SDRAM holds twice the seconds.
#if LOFI_INT16
static constexpr unsigned kSourceMaxSeconds = 84;
#else
static constexpr unsigned kSourceMaxSeconds = 42;
#endif

// Tempo control range (BPM). The platform's tempo knob spans this range; the granular Tempo class and
// the UI's tempo MValue display both convert through the helpers below. Lives here (not on Tempo) so
// the platform has no granular dependency for tempo display - Phase 5 R4.
static constexpr float kTempoMinBpm = 20.f;
static constexpr float kTempoMaxBpm = 250.f;
inline float tempo_abs_to_norm(const float bpm)
{
    return (bpm - kTempoMinBpm) / (kTempoMaxBpm - kTempoMinBpm);
}
inline float tempo_norm_to_abs(const float norm)
{
    return kTempoMinBpm + std::clamp(norm, 0.f, 1.f) * (kTempoMaxBpm - kTempoMinBpm);
}

}