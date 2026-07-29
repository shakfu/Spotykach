// The single definition of the shared Hann lookup table for a graincloud firmware image.
//
// graincloud is a self-contained copy of the granular tree and is never linked alongside it, so it
// carries its own definition. See the rationale in dsp/hann.h.
#include "dsp/hann.h"

namespace spotykach {

const std::array<float, kHannCurveSize> HannCurve = build_hann_curve();

};
