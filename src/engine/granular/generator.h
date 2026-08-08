#pragma once
#include <cstdint>   // uint*_t (transitive-include hygiene; host build)
#include <cstddef>   // size_t (transitive-include hygiene; host build)

#include <array>
#include <bitset>
#include <functional>
#include "buffer.h"
#include "vox.h"
#include "event.h"
#include "nocopy.h"

namespace spotykach {

#if SPK_GRAIN_GF
// ENGINE=graincloud only. graincloud is this same engine tree compiled with SPK_GRAIN_GF, where the
// per-sample Vox array is replaced by a per-block GrainflowLib cloud; the only source file it
// substitutes is generator.cpp (see src/engine/graincloud/). This forward declaration keeps the
// GrainflowLib templates out of every other translation unit - only graincloud's generator.cpp and
// gf_cloud.cpp ever see the definition. Nothing here is compiled into a granular image.
class GfCloud;
#endif

class Generator {

friend class Deck;
friend class Drifter;

public:
  enum Param {
    Start,
    Speed,
    Count
  };

  static constexpr uint8_t kVoxCount = 3;

  Generator();
  ~Generator() = default;

  uint8_t ref;

  bool is_generating() const { return _is_active.any(); }

  void init(Buffer*, size_t* slice_points);

  // float start() { return static_cast<float>(_start); }
  void pitch_speed_mod_in(const float value);
  
  void set_shape(const float);
  void set_win_size(const float);
  void set_win_spread(const float);
  float win_spread() const { return _input_spread; }

  void set_pitch(const float);

  void set_is_wide(const bool);

  bool set_speed(float);
  void apply_speed() { set_speed(_norm_pitch_speed); }

  void slice();
  void auto_slice(const size_t slice_size, const size_t slice_count);
  void clear_slices();
  void set_snap_to_slice(const bool value) { _snap_to_slice = value; }

  float increment() const { return _increment; }

  bool is_speed_mode(const SpeedMode mode) const { return _speed_mode == mode; }
  void set_speed_mode(const SpeedMode);

  void stop(uint8_t slice_idx);
  void set_on_vox_stop(std::function<void(const uint8_t)>&& on_stop) { _on_vox_stop = on_stop; }

  bool read_reset_is_triggered() {
    auto is_triggered = _is_triggered;
    _is_triggered = false;
    return is_triggered;
  }

  void process(float& out0, float& out1);

#if SPK_GRAIN_GF
  // graincloud: the Play pad gates the cloud. Deck sets this from its _is_playing state; process()
  // emits silence (and clears the "generating" flag) when not playing. Without it the cloud would
  // granulate the buffer continuously - including a preloaded sample - with Play off.
  void set_playing(bool p) { _playing = p; }
#endif

protected:
  void set_mode(const Vox::Mode);

  float start() const { return _input_start; }
  void set_start(float);
  void set_start_offset(const float);
  void set_cont_start_mod(const bool val) { _cont_start_mod = val; }; // If set to false (default), start is applied once at the beginning of the slice

  float size() const { return _input_size; }
  void set_size(float);
  void set_size_offset(const float);

  bool is_reverse() const { return _reverse; }
  void set_reverse(const bool);

  void apply_pitch() { set_pitch(_norm_pitch_speed); }
  void set_cont_pitch_mod(const bool val) { _cont_speed_mod = val; }
  
  void trigger(const uint8_t vox_idx, const Event* event);

  void apply_shape() { set_shape(_norm_shape); }

  float playhead_at(const uint8_t idx) const { return _voxs[idx].playhead(); }
  float envelope_at(const uint8_t idx) const { return _voxs[idx].envelope(); }

  bool is_suspended() const { return _voxs[0].is_suspended(); }

private:
  NOCOPY(Generator)

  size_t _abs_start();
  size_t _abs_size();
  size_t _abs_spread();
  size_t _snap(const float norm_value);

  void _apply_start();
  void _apply_size();
  void _apply_spread();

  Buffer* _buffer;
  std::array<Vox, kVoxCount> _voxs;
#if SPK_GRAIN_GF
  GfCloud* _gf = nullptr; // process() sums this GrainflowLib cloud instead of the Vox array
  bool _playing = false;  // Play-pad gate (set by Deck::play/stop); the cloud is silent when false
#endif

  std::function<void(const uint8_t)> _on_vox_stop;

  float _input_start;
  bool _cont_start_mod;
  float _norm_start;
  float _norm_start_offset;

  float _input_size;
  float _norm_size;
  float _norm_size_offset;
  float _norm_spread;
  float _input_spread;

  size_t* _slice_points;
  size_t  _slice_size;
  size_t  _auto_slice_max_idx;
  uint8_t _slice_points_count;
  bool    _is_auto_slice;
  bool    _snap_to_slice;

  float _norm_pitch_speed;

  float _increment;
  float _target_increment;

  float _speed;
  float _speed_mod_mult;
  float _trig_speed_mod_mult;
  bool _cont_speed_mod;

  float _norm_shape;

  Vox::Mode _vox_mode;
  SpeedMode _speed_mode;

  std::bitset<kVoxCount> _is_active;

  bool _reverse;
  bool _is_triggered;
};
};
