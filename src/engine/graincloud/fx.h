#pragma once
#include <cstdint>   // uint*_t (transitive-include hygiene; host build)
#include <cstddef>   // size_t (transitive-include hygiene; host build)

#include "../../nocopy.h"
#include "fx.drive.h"
#include "fx.reduce.h"
#include "dsp/biquad.h"
#include "echo.h"
#include "softswitch.h"
#include "engine/mode.h"

namespace spotykach {

class Fx {
public:
    using GritMode = spotykach::GritMode; // moved to the contract (engine/mode.h) in item 5b

    struct Params {
        float sample_rate;
        float** delay_buf;
    };

    static constexpr size_t kFeedbackDelayBufferLength  { 10000 }; 
    static constexpr uint8_t kDelayMaxSeconds           { 5 };
    static constexpr size_t kEchoDelayBufferLength      { 48000 * kDelayMaxSeconds };

    Fx();
    ~Fx() = default;

    void init(const Params);
    void process(float& inout0, float& inout1);

    // GRIT ///////////////////////////////////////////
    GritMode grit_mode() const { return _grit_mode; }
    void switch_grit_mode();
    
    bool is_grit_on() const { return _grit_switch.is_on(); }
    void set_grit_on(const bool);
    void toggle_grit_lock();

    float grit_intensity();
    void set_grit_intensity(const float norm);
    
    float grit_mix();
    void set_grit_mix(const float norm);
    
    // FLUX ///////////////////////////////////////////
    bool is_flux_on() const { return _flux_switch.is_on(); }
    void set_flux_on(const bool);
    void toggle_flux_lock();

    float flux_intensity() const { return _flux_int; };
    void set_flux_intensity(const float norm);
    
    float flux_mix() const { return _flux_mix_norm; };
    void set_flux_mix(const float norm);
    
    float flux_fb() const { return _flux_fb; };
    void set_flux_fb(const float norm);
    
private:
    NOCOPY(Fx)

    void _apply_flux_fb();
    void _apply_flux_mix();
    void _apply_flux_int(const bool hard = false);

    Drive _drive;
    Reduce _reduce;
    infrasonic::EchoDelay<kEchoDelayBufferLength> _echo_delay[2];
    SoftSwitch _flux_switch;
    SoftSwitch _grit_switch;
    
    float _flux_int;
    float _flux_mix;
    float _flux_mix_norm;
    float _flux_fb;
    bool _flux_on;
    bool _flux_lock;

    GritMode _grit_mode;
    bool _grit_on;
    bool _grit_lock;
    
};

};