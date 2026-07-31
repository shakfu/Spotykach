#pragma once
#include <cstdint>   // uint*_t (transitive-include hygiene; host build)

#ifndef LFO_H
#define LFO_H

#include "dsp/lutsinosc.h"
#include "daisysp.h"
#include "random.lfo.h"

namespace spotykach {

static float curved_value(float norm)
{
    if (norm < .5f) norm = 1.5f * norm / (norm + 1.f); 
    else norm = norm * norm * 0.6666666667f + .334f;
    return std::clamp(norm, 0.f, 1.f);
}

class LFO {
public:
    enum class Type: uint8_t {
        sine,
        square,
        saw,
        random
    };

    LFO() = default;
    ~LFO() = default;

    void init(const float sample_rate) { 
        _sine.init(sample_rate); 
        _osc.Init(sample_rate);
        _osc.SetAmp(1.f);
        _random.init(sample_rate);
    }
    
    void set_speed(const float val) { 
        _sine.set_freq(val); 
        _osc.SetFreq(val);
        _random.set_freq(val);
    }

    void set_glow(const float val) {
        _amp = curved_value(val);
    }

    void set_type(const Type type) {
        _type = type;

        switch (_type) {
            case Type::saw: _osc.SetWaveform(daisysp::Oscillator::WAVE_RAMP); break;
            case Type::square: _osc.SetWaveform(daisysp::Oscillator::WAVE_SQUARE); break;
            default: break;
        }
    }

    float process() {
        return (_sample() + 1.f) * .5f * _amp;
     }

     void reset()
     {
        _sine.reset();
        _random.reset();
        _osc.Reset();
     }

private:
    
     float _sample() {
        switch (_type) {
            case Type::saw: return _osc.Process();
            case Type::square: return _osc.Process();
            case Type::random: return _random.process();
            default: return _sine.process();
        }
     } 

    LUTSinOsc _sine;
    RandomLFO _random;
    daisysp::Oscillator _osc;

    Type _type;

    float _amp;
};
};

#endif