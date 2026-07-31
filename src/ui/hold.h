#pragma once
#include <cstdint>   // uint*_t (transitive-include hygiene; host build)

#include <daisy_seed.h>

namespace spotykach {

template<uint32_t duration_ms>
class Hold {
public:
    void init() {
        _timer.Init();
    }
    void begin() {
        if (!_is_holding) {
            _timer.Restart();
            _is_holding = true;
        }
    }
    bool process() {
        if (_passed) {
            return true;
        }
        else {
            _passed = _passed_threshold();
            return _passed;
        }
    }
    bool inidcate() {
        return _indicate;
    }
    bool is_holding() { 
        return _is_holding; 
    }
    bool passed() {
        return _passed;
    }
    void end() {
        _is_holding = false;
        _indicate = false;
        _passed = false;
    }

private:
    bool _passed_threshold() {
        if (!_is_holding) return false;
        if (_timer.HasPassedMs(50)) {
            _indicate = true;
        }
        if (_timer.HasPassedMs(duration_ms)) {
            _indicate = false;
            return true;
        }
        return false;
    }
    daisy::StopwatchTimer _timer;
    bool _is_holding;
    bool _indicate;
    bool _passed;
};

};
