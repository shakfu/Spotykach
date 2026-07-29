// The single definition of the shared Hann lookup table for a granular firmware image.
//
// It lives in the engine tree rather than in src/dsp/ on purpose - see the rationale in dsp/hann.h.
// The build globs src/engine/granular/*.cpp, and only one engine tree is ever linked into an image,
// so this compiles exactly once per binary that needs the table and not at all otherwise.
#include "dsp/hann.h"

namespace spotykach {

const std::array<float, kHannCurveSize> HannCurve = build_hann_curve();

};
