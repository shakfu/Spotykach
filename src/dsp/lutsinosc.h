#pragma once
#include <cstddef>   // size_t (transitive-include hygiene; host build)
#include <cmath>   // std::math (transitive-include hygiene; host build)

#include <array>
#include <algorithm>

namespace spotykach {

static constexpr size_t kLUTSinSize = 128;
static std::array<float, kLUTSinSize> lut_sin_table() {
  auto pi = 3.14159265358979323846264338327950288f;
  static std::array<float, kLUTSinSize> table  { 0 };
  for (size_t i = 0; i < kLUTSinSize; i++) {
    auto s = static_cast<float>(std::sin(2.f * pi * static_cast<float>(i) / static_cast<float>(kLUTSinSize)));
    table[i] = std::clamp(s, -1.f, 1.f); 
  }
  return table;
};

static const std::array<float, kLUTSinSize> k_lut_sin_table = lut_sin_table();

static float LUT_Sin_Value_At(const float phase)
{
  //Do linear interpollation a + k * (b - a)
  auto int_phase = static_cast<size_t>(phase);
  auto frac_phase = phase - int_phase;
  auto next_phase = int_phase + 1;
  while (next_phase >= kLUTSinSize) next_phase -= kLUTSinSize;
  return k_lut_sin_table[int_phase] + frac_phase * (k_lut_sin_table[next_phase] - k_lut_sin_table[int_phase]);
}

class LUTSinOsc {
public:
    LUTSinOsc():
    _freq_kof         { 0 },
    _phase            { 0 },
    _phase_increment  { 0 },
    _phase_offset     { 0 },
    _amp              { 1.f }
    {}

    ~LUTSinOsc() {}

    void init(float sample_rate) {
      _freq_kof = static_cast<float>(kLUTSinSize) / sample_rate;
    }

    void set_freq(const float freq) {
        _phase_increment = freq * _freq_kof;
    }

    void set_amp(const float amp) { 
      _amp = amp; 
    }

    void set_phase_offset(const float value) {
      _phase_offset = static_cast<float>(kLUTSinSize) * value;
    }

    void reset() {
      _phase = 0;
    }

    float process() {
      auto size = static_cast<float>(kLUTSinSize);
      auto phase = _wrap_min_max(_phase + _phase_offset, 0.f, size);
      auto sample = LUT_Sin_Value_At(phase);

      //Advance phase
      _phase += _phase_increment;
      while (_phase >= size) _phase -= size;
      
      return sample * _amp;
    }

private:
    LUTSinOsc(const LUTSinOsc &other) = delete;
    LUTSinOsc(LUTSinOsc &&other) = delete;
    LUTSinOsc& operator=(const LUTSinOsc &other) = delete;
    LUTSinOsc& operator=(LUTSinOsc &&other) = delete;

    // https://stackoverflow.com/questions/4633177/how-to-wrap-a-float-to-the-interval-pi-pi
    float _wrap_max(const float x, const float max) {
      return fmodf(max + fmodf(x, max), max);
    }
    float _wrap_min_max(const float x, const float min, const float max) {
      return min + _wrap_max(x - min, max - min);
    }

    float _freq_kof;
    float _phase;
    float _phase_increment;
    float _phase_offset;
    float _amp;
};

};
