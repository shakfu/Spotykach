#pragma once

#include <cstdint>   // uint*_t (transitive-include hygiene; host build)
#include <cstddef>   // size_t (transitive-include hygiene; host build)
#include <random>
#include <algorithm>
#include <array>
#include <bitset>
#include "xfade.h"

namespace spotykach {

class Panner {
public:
    enum class Mode: uint8_t {
        off,
        smooth,
        step
    };
    Panner();
    ~Panner() = default;

    void init(const float sample_rate);

    void set_mode(const Mode, const uint8_t chan);

    float speed() const { return _speed; }
    void set_speed(const float);
    
    float range() const { return _range; }
    void set_range(const float);

    void tick();

    void process(float* in[2], float* out[2]);

private:
    static constexpr auto kMinDeviation = .05f;
    static constexpr auto kDeviationRange = .95f;
    static constexpr auto kMinSecToChange = .5f;
    static constexpr auto kSecToChangeRange = 4.5f;
    static constexpr auto kMinTicksToChange = 1;
    static constexpr auto kTicksToChangeRange = 15;
    static constexpr auto kStage = 1e6;
    static constexpr auto kMid = kStage / 2;
    static constexpr auto kStageKof = 1.f / kStage;

    void _schedule_stage(const uint8_t chan);
    void _schedule_step(const uint8_t chan);
    void _schedule_smooth(const uint8_t chan, const bool fast = false);
    bool _transition(const uint8_t chan);
    float _real_stage(const size_t stage) const
    {  
        return std::clamp(static_cast<float>(stage) / static_cast<float>(kStage),  0.f, 1.f);
    }
    
    std::array<XFade, 2> _xfade;
    std::array<Mode, 2>  _mode;

    std::array<int32_t, 2> _prev_stage;
    std::array<int32_t, 2> _target_stage;

    std::array<int32_t, 2> _sample_count;
    std::array<int32_t, 2> _samples_to_change;
    std::array<float, 2>   _smooth_change_kof;
    
    std::array<int32_t, 2> _ticks_count;
    std::array<int32_t, 2> _ticks_to_change;

    std::bitset<2> _pending_step;

    std::random_device _rand;
    std::uniform_real_distribution<float> _dice;

    float _sample_rate;
    float _speed;
    float _range;
    
};

};