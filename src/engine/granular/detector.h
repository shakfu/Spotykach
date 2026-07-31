#pragma once

#include <array>
#include <cstddef>   // size_t (was relying on transitive include order; broke the host build)

namespace spotykach {

class Detector {
public:
  // Buffer capacity (allocation size). The runtime window is derived from the sample
  // rate in init() and is <= kWindow; at 48 kHz it equals kWindow (480 = 10 ms).
  static constexpr size_t kWindow = 480; //10ms @48k

  Detector();

  void init(float **, const float sample_rate);

  bool is_active() { return _is_armed || _is_open; }
  bool is_open() { return _is_open && !_will_close && !_is_closing; }

  void set_armed(const bool armed);

  void set_treshold(const float value);

  void process(const float in0, const float in1, float& out0, float& out1);

private:
  static constexpr float kKofRise = 0.1;
  static constexpr float kKofFall = 0.99;

  float** _buffer;
  size_t  _window { kWindow };

  float   _average;
  float   _db_threshold;
  size_t  _write_head;
  size_t  _read_head;
  bool    _is_armed;
  bool    _is_open;
  bool    _will_close;
  bool    _is_closing;
};
};

