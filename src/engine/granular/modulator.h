#pragma once
#include <cstdint>   // uint*_t (transitive-include hygiene; host build)

#include <array>
#include "lfo.h"
#include "dsp/follower.h"
#include "nocopy.h"
#include "engine/mode.h"

namespace spotykach {

class Modulator {
public:
    using Type = ModType; // moved to the contract (engine/mode.h) in item 5b

    Modulator() = default;
    ~Modulator() = default;

    void init(const float sample_rate);

    Type type() const { return _type; };
    void set_type(const Type type);
    void set_lfo_type(const LFO::Type);

    void set_speed_norm(const float, const bool sync); //also follower speed
    void set_amp_norm(const float);

    bool is_synced() const { return _is_synced; }
    void tick(const float tempo, const bool is_quarter);

    void follow(const float);
    void process(float& out);

private:
    NOCOPY(Modulator)

    static constexpr std::array<float, 9> kFreqDiv = { 0.5, 1, 2, 4, 8, 16, 32, 48, 64 };

    LFO      _lfo;
    Follower _follower;

    Type _type;
    float _freq_mult;
    uint8_t _reset_count;
    uint8_t _ticks_to_reset;
    bool _is_synced;

};

};