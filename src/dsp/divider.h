#pragma once

#include <cmath>    // std::round
#include <stddef.h> // size_t
#include <stdint.h>

namespace spotykach {

enum class Every: uint8_t {
  _4th  = 1,
  _8th  = 2,
  _16th = 4,
  _32th = 8
};

class Divider {
public:
  Divider(size_t ppqn, Every resolution = Every::_16th);

  Every resolution() const { return _resolution; }

  // NOTE: set_swing/set_triplets_on are implemented and tested but are not currently reached from
  // any engine or UI gesture - no firmware code calls either. They are kept (rather than deleted)
  // because tick() already integrates both correctly and the delay/qdelay division tables advertise
  // triplet divisions, so a future gesture can turn them on without re-deriving the grid maths.
  // See TODO.md. Both default OFF, which is what every current build relies on.
  void set_swing(const float frac_swing) {
      // For 48 ppqn. Conversion to actual ppqn is
      // considered in _swing_kof ////////////////
      // |  0  |  1  |  2  |  3  |  4  |  5  |
      // | 50% | 54% | 58% | 62% | 66% | 70% |
      if (_swing_on) _swing = static_cast<size_t>(std::round(frac_swing * _swing_kof));
  }

  void set_triplets_on(const bool on) { _triplets_on = on; }

  bool tick();

  void reset();

private:
    float _swing_kof;
    size_t _swing;
    size_t _pulses_per_bar;
    size_t _pulses_per_trigger;
    size_t _triggers_per_bar;
    size_t _iterator;
    size_t _trigger_count;
    size_t _next_trigger;
    size_t _odd_count;
    size_t _odd_count_max;
    Every _resolution;
    // Default member initializers, not just constructor initializers: `_triplets_on` was omitted from
    // the constructor's list and read as indeterminate. On target that was masked by .bss zeroing (the
    // only Dividers live inside the file-static AppImpl), but any automatic-storage Divider - the host
    // tests - got garbage, and a garbage `_triplets_on` silently rescales the whole tick grid.
    bool _is_odd      = false;
    bool _swing_on    = false;
    bool _triplets_on = false;
};

};
