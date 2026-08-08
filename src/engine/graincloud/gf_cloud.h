// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once
//
// GfCloud - a GrainflowLib grain-cloud DSP core that plugs into the granular engine's Generator seam
// (ENGINE=graincloud, the SPK_GRAIN_GF build of the granular engine). It reads the granular `Buffer`
// (the recorded audio, in SDRAM) and produces a continuous polyphonic grain cloud: dozens of grains
// with independent pitch/pan/position, the thing the granular looper itself can't do.
//
// It is included ONLY by generator.cpp under SPK_GRAIN_GF, so the GrainflowLib templates stay out of
// the rest of the granular tree. Granular's process path is per-SAMPLE; GrainflowLib is per-BLOCK, so
// GfCloud computes a 96-sample stereo block on demand and serves it one sample at a time. Two decks
// process interleaved per-sample, so each deck owns its own GfCloud instance (acquire() by deck index).
// The grain scratch lives in fast regular RAM (static instances); only the audio Buffer is SDRAM.

// <cstdint> BEFORE the vendored headers: gfParam.h declares `enum class ... : std::uint8_t` but never
// includes <cstdint>, relying on a transitive include that arm-none-eabi happens to provide and the
// host toolchain does not. Without this, host/test_graincloud.cpp fails to compile on the enum's
// underlying type - which is why that test sat broken and excluded from `make -C host test`.
#include <cstdint>

#include "grainflow/gfGrainCollection.h"
#include "buffer.h"   // shared: graincloud is the granular tree built with SPK_GRAIN_GF

namespace spotykach {

class GfCloud {
public:
    static constexpr int kBlock     = 96;
    static constexpr int kMaxGrains = 8;   // per deck

    GfCloud();

    void init(Buffer* buf, float sample_rate);

    // Cloud knobs, each taken straight from the engine's raw knob value (normalized 0..1), bypassing
    // granular's mode-dependent param routing so the cloud always has direct, consistent control:
    void set_center(float v);     // POS     - cloud centre position in the buffer
    void set_spray(float v);      // SIZE    - position scatter of grain start points
    void set_transpose(float v);  // PITCH   - grain transpose (0.5 = unity, +/- ~2 octaves)
    void set_duration(float v);   // ENV     - grain duration (8 ms .. 1.5 s)
    void set_density(float v);    // MODFREQ - onset rate / overlap
    void set_spread(float v);     // MOD_AMT - per-grain pitch + pan spread

    // GrainflowLib character params, on the cloud's spare control surfaces:
    void set_direction(int mode); // Mode switch - 0 forward, 1 reverse, 2 random per grain
    void set_scan_speed(float v); // Alt+PITCH   - playhead speed: 0 = freeze, noon = 1x, up to 4x
    void set_glisson(float v);    // Alt+SOS     - per-grain pitch glide (0.5 = none, bipolar)
    void set_vibrato(float v);    // Alt+POS     - per-grain vibrato depth (0 = off)
    void set_pong(bool on);       // loop/read mode: normal vs pong (fold) - currently unmapped

    // Per-sample: returns the next cloud sample (recomputes a block at each 96-sample boundary).
    void process(float& out0, float& out1);

    // Current playhead position in the buffer, 0..1 (for the ring display).
    float playhead() const;

private:
    using Grain      = Grainflow::gf_grain<Buffer, kBlock, float>;
    using Collection = Grainflow::gf_grain_collection<Buffer, kBlock, float>;

    void compute_block();

    Buffer*    _buf = nullptr;
    float      _sr  = 48000.f;
    Collection _col;
    Grain      _grains[kMaxGrains];

    // io_config scratch (one stream channel; auto_overlap staggers the active grains).
    float _gc[kBlock], _tr[kBlock], _fm[kBlock], _am[kBlock];
    float _o_out[kMaxGrains][kBlock], _o_state[kMaxGrains][kBlock], _o_prog[kMaxGrains][kBlock],
          _o_play[kMaxGrains][kBlock], _o_amp[kMaxGrains][kBlock], _o_env[kMaxGrains][kBlock],
          _o_bch[kMaxGrains][kBlock], _o_sch[kMaxGrains][kBlock];
    float* _in_gc[1]; float* _in_tr[1]; float* _in_fm[1]; float* _in_am[1];
    float* _p_out[kMaxGrains]; float* _p_state[kMaxGrains]; float* _p_prog[kMaxGrains];
    float* _p_play[kMaxGrains]; float* _p_amp[kMaxGrains]; float* _p_env[kMaxGrains];
    float* _p_bch[kMaxGrains]; float* _p_sch[kMaxGrains];
    Grainflow::gf_io_config<float> _io;

    float _blk_l[kBlock], _blk_r[kBlock]; // computed stereo block
    int   _idx = kBlock;                  // forces a recompute on the first process()

    float _panH[kMaxGrains];              // per-grain pan hash (0..1, stable); spread scales it live

    // live cloud params (set via the knob setters, consumed at block boundaries)
    float _center = 0.3f, _dur = 0.5f, _rate = 1.f, _density = 0.4f, _spray = 0.25f, _spread = 0.3f;
    float _glisson = 0.5f, _vibrato = 0.f; // 0.5 glisson = no glide; 0 vibrato = off
    int   _dir = 0;        // 0 fwd, 1 reverse, 2 random
    bool  _pong = false;
    int   _active = kMaxGrains;
    float _scan_speed = 1.f;  // playhead speed multiplier (0 = freeze, 1 = 1x)
    double _gc_ph = 0.0;
    double _scan_phase = 0.0; // playhead position (0..1), auto-advances through the buffer
};

// Per-deck GfCloud instances (regular RAM, not the SDRAM arena). ref 0 = deck A, 1 = deck B.
GfCloud* gf_cloud_acquire(int ref);

} // namespace spotykach
