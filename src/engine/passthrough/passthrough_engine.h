// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once
#include <cstddef>   // size_t (transitive-include hygiene; host build)

#include "engine/iengine.h"
#include "engine/engine_params.h"
#include "engine/display_model.h"
#include "nocopy.h"

#include <cmath>

namespace spotykach {

// A real, build-time-selectable firmware variant (item 3b-1): `make ENGINE=passthrough`. A
// deliberately-different, non-granular engine - stereo passthrough - that exercises the
// platform<->engine seam with something that is not the granular looper.
//
// It declares only CapOwnDisplay: no recording, tape, sequencer, transport. Everything on IEngine
// beyond the audio lifecycle + render() is left as the Strategy-A no-op default, so the platform
// drives it harmlessly (knobs/pads/MIDI/CV do nothing; Storage skips tape ops because it gates on
// CapTapeStorage). CapOwnDisplay routes the platform's LED render to render(DisplayModel&) below
// (item 3b-2a), so this variant draws its own level meter instead of blank rings. See
// docs/item3b-plan.md.
class PassthroughEngine : public IEngine {
public:
    PassthroughEngine() = default;
    ~PassthroughEngine() override = default;

    void init(const EngineContext&) override { _peak = 0.f; }
    void prepare() override {}

    void process(const float* const* in, float** out, size_t size) override {
        float peak = 0.f;
        for (size_t i = 0; i < size; i++) {
            out[0][i] = in[0][i];
            out[1][i] = in[1][i];
            peak = std::fmax(peak, std::fmax(std::fabs(out[0][i]), std::fabs(out[1][i])));
        }
        _peak = peak;
    }

    Capabilities capabilities() const override { return CapOwnDisplay; }

    // A non-granular display: a symmetric level meter on both rings + lit play indicators.
    // Drawn with LEDRing's primitives (Option A) - exactly how a granular render() will reuse them.
    void render(DisplayModel& m) override {
        m.clear();
        const float level = _peak > 1.f ? 1.f : _peak;
        for (int r = 0; r < 2; r++) {
            if (level > 1e-4f) {
                m.ring[r].set_hex_color(0xffffff);
                m.ring[r].set_segment(0.f, level * 0.999f);
            }
            m.ring[r].set_updated();         // mark ready for the platform blit
            m.play[r] = { 0x00ff00, 1.f };
        }
    }

private:
    NOCOPY(PassthroughEngine)

    float _peak = 0.f;
};

};
