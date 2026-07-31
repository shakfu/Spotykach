#pragma once
#include <cstdint>   // uint*_t (transitive-include hygiene; host build)
#include <cmath>   // std::math (transitive-include hygiene; host build)

#include "daisysp.h"
#include <array>
#include <algorithm>

namespace spotykach {

template<int32_t half_range>
class SpeedMap {
public:
  void init() {
    _map.fill(0);
    for (int32_t st = 0; st < kSize; st++) {
        auto power = static_cast<float>(st - half_range) / 12.f;
        _map[st] = std::pow(2.f, power); // speed = 2^(pitch/12)
    }
  };

  int32_t range() const {
    return kSize;
  }

  float bipolar_pitch2speed(float pitch) const {
    pitch += half_range;

    auto int_pitch = static_cast<int32_t>(pitch);

    if (int_pitch < 0) return _map.front();
    if (int_pitch >= kSize) return _map.back();

    auto frac_pitch = pitch - int_pitch;
    if (frac_pitch == 0) { return _map[int_pitch]; }

    auto next_pitch = int_pitch + 1;
    if (next_pitch >= kSize) next_pitch = kSize - 1;

    return _map[int_pitch] + frac_pitch * (_map[next_pitch] - _map[int_pitch]);
  };

  float norm2speed(float norm_value) const {
    auto pitch = static_cast<uint8_t>(daisysp::fmap(norm_value, half_range - 12.f, half_range + 12.f));
    return _map[pitch];
  }

private:
  static constexpr int32_t kSize = 2 * half_range + 1;
  std::array<float, kSize> _map; //semitones -5...+5 octaves and zero.
};

};
