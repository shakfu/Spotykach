#pragma once
#include <cstdint>   // uint*_t (transitive-include hygiene; host build)

#include "nocopy.h"
#include <array>
#include <algorithm>
#include <math.h>

namespace spotykach {

/**
 * @brief
 * Pattern generator
 * Generates equaly distributed patterns of 1...16 onsets over a variable active length (1..16
 * steps). This produces both "regular" and "euclidean" patterns. The active length lets two
 * instances run at different lengths (polymeter), realigned by reset().
 */
class CPattern {
public:
    static constexpr uint8_t kSize = 16;

    CPattern();
    ~CPattern() {}

    // Active pattern length in steps (1..kSize). Changing it regenerates the distribution.
    void set_length(uint8_t steps);
    uint8_t steps() const { return _steps; }

    void set_shift(const float frac_offset) {
        // 0...one full active length of rotation
        if (_steps == 0) { _shift = 0; return; }
        _shift = static_cast<uint8_t>(std::roundf(frac_offset * _steps)) % _steps;
    }
    void set_max_onsets(uint8_t max_onsets) {
        //0...16
        _max_onsets = std::min(max_onsets, kSize);
    }

    void plus_onset();
    void minus_onset();
    void set_onsets(const uint8_t onsets);
    uint8_t onsets() const { return _onsets; }

    bool trigger();

    uint8_t next_point() {
        return _next_point;
    }

    uint8_t length() const { return _current_length; }

    void reset() { _next_point = 0; }

    // Read-only views for display (no side effects, unlike trigger()). step_is_onset applies the
    // same shift trigger() uses, so the ring shows the pattern as it actually plays; position() is
    // the next step trigger() will read. Both operate over the active length.
    bool step_is_onset(uint8_t step) const {
        if (_steps == 0) return false;
        int point = static_cast<int>(step % _steps) - static_cast<int>(_shift);
        while (point < 0) point += _steps;
        return _pattern[point] != 0;
    }
    uint8_t position() const { return _next_point; }

private:
    NOCOPY(CPattern)

    void _build(); // (re)generate _pattern + _length for the current _onsets over _steps

    std::array<uint8_t, kSize> _pattern;
    std::array<uint8_t, kSize> _length;
    uint8_t _current_length;
    uint8_t _steps;
    uint8_t _onsets;
    uint8_t _max_onsets;
    uint8_t _next_point;
    uint8_t _shift;
};

};
