#pragma once
#include <cstddef>   // size_t (transitive-include hygiene; host build)

#include "daisysp.h"

namespace spotykach {

class Click {
public:
  Click();
  ~Click() = default;

  void init(const float sample_rate);

  float process();

  void trigger(const bool is_key_quarter) { 
    _trigger = true; 
    _is_key = is_key_quarter;
  }

  void reset() {
    _counter = _kBeat_counts;
  }

private:
  daisysp::Adsr _env;
  daisysp::Oscillator _osc;
  size_t _counter = _kBeat_counts;
  bool _trigger;
  bool _is_key;
  static constexpr size_t _kBeat_counts = 3;
};
};
