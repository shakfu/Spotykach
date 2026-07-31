#pragma once
#include <cstdint>   // uint*_t (transitive-include hygiene; host build)
#include <cstddef>   // size_t (transitive-include hygiene; host build)

#include <array>
#include <algorithm>
#include "nocopy.h"
#include "config.h"               // kTempoMin/MaxBpm + tempo_norm_to_abs (the shared tempo range)
#include "engine/itimesource.h"

namespace spotykach {

class Tempo {
public:
    Tempo();
    ~Tempo() = default;

    void set_time_source(const ITimeSource* time) { _time = time; }

    void set_norm(const float value) {
        _bpm = tempo_norm_to_abs(value);
    }
    float bpm() const { return _bpm; }

    void tap();

private:
    NOCOPY(Tempo)

    const ITimeSource* _time = nullptr;
    std::array<size_t, 4> _times;
    size_t _prev_time;
    float _avg;
    float _bpm;
    uint8_t _pointer;
    bool _full;
};

};
