#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace spotykach {

inline constexpr size_t kHannCurveSize = 192;

// Quarter-cycle raised-cosine (sin^2) lookup table.
//
// Declared here, defined exactly once per firmware image by the engine that uses it (see
// `hann.curve.cpp` in each consuming engine tree). Previously the table was defined *in this
// header* as `static auto HannCurve = hannCurve();`, so every translation unit that included it
// got a private 768-byte copy plus a private ~108-byte startup initialiser - three of each in a
// granular build, with the initialisers landing in the tight 186 KB SRAM_EXEC budget.
//
// The definition deliberately lives in the engine trees rather than in a `dsp/hann.cpp`: the build
// globs `src/dsp/*.cpp` into *every* engine, and because the linker script does
// `KEEP(*(.init_array*))`, a static initialiser here would survive `--gc-sections` and charge the
// table to the ~20 engines that never touch it. An engine that includes this header without
// providing the definition fails loudly at link time with an undefined reference.
extern const std::array<float, kHannCurveSize> HannCurve;

// Builds the table. Inline so the logic is not duplicated across engine trees; only the single
// translation unit that defines `HannCurve` ever emits it.
inline std::array<float, kHannCurveSize> build_hann_curve() {
  constexpr float kPiOver2 = 3.1415927410125732421875f * .5f;
  std::array<float, kHannCurveSize> slope { 0 };
  for (size_t i = 0; i < kHannCurveSize; i++) {
    auto s = std::sin(kPiOver2 * static_cast<float>(i) / static_cast<float>(kHannCurveSize - 1));
    slope[i] = std::clamp(s * s, 0.f, 1.f);
  }
  return slope;
}

// Kept inline: this runs per-sample per-voice from Window::_attenuation() and Vox::process(), so it
// must fold into its callers rather than become a cross-translation-unit call.
inline float Hann_Value_At(const float norm_pos)
{
  auto pos = (kHannCurveSize - 1) * norm_pos;
  auto int_pos = static_cast<size_t>(pos);
  auto frac = pos - static_cast<float>(int_pos);
  auto n_pos = int_pos + 1;
  if (n_pos >= kHannCurveSize) n_pos = kHannCurveSize - 1;
  auto v = HannCurve[int_pos];
  auto n = HannCurve[n_pos];
  return std::clamp(v + frac * (n - v), 0.f, 1.f);
}

};
