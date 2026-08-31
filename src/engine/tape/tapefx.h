#pragma once

#include "engine/tape/faust_kernel_tapefx.h" // the cyfaust-generated kernel: tfx_tapefx::mydsp
#include "engine/faust/faust_capture.h"      // shared faustgen::CaptureUI / Bind / Role

namespace spotykach {

// Shared tape-FX kernel wrapper: wow/flutter (modulated fractional delay) -> Jiles-Atherton hysteresis
// -> post-FX resonant low-pass. The generated kernel is mono (1 in, 1 out); compute() is allocation-free
// and in-place safe (Faust reads each input sample into a local before writing the output). The ~16 KB
// delay buffer lives inside the object, so the whole struct is placement-new'd into the SDRAM arena.
// Used by the streaming `tape` engine (one per deck) and the buffer-based `shuttle` engine (one per
// track); the .dsp source + generated kernel live under src/engine/tape/.
//
// The six sliders all carry native range [0,1] (the .dsp does the musical scaling), so the shared
// Role::set linear-map (0 + v*(1-0) = v) writes the normalized value straight through, exactly as the
// old hand-written Cap did. The zone-capture machinery is the shared faustgen:: one.
struct TapeFx {
    tfx_tapefx::mydsp dsp;
    faustgen::Role role[6]; // drive, char, wow, rate, cutoff, reso

    void init(int sr) {
        dsp.init(sr);
        static const faustgen::Bind kBinds[] = {
            { nullptr, "drive",  0, 0, false }, { nullptr, "char",   1, 0, false },
            { nullptr, "wow",    2, 0, false }, { nullptr, "rate",   3, 0, false },
            { nullptr, "cutoff", 4, 0, false }, { nullptr, "reso",   5, 0, false },
        };
        faustgen::CaptureUI<false> ui; ui.roles = role; ui.binds = kBinds; // tape doesn't read defaults
        ui.nbinds = static_cast<int>(sizeof(kBinds) / sizeof(kBinds[0]));
        dsp.buildUserInterface(&ui);
    }
    void set(int which, float v) { role[which].set(v); }                // which: 0..5, v in [0,1]
    // In-place: the same sample buffer is both source and destination, which is safe here because the
    // generated loop reads input0[i] into a temporary before writing output0[i] at the same index.
    // The two POINTER ARRAYS must still be distinct: compute() takes `FAUSTFLOAT** RESTRICT` for both,
    // so handing it one array twice is a restrict violation (caught by -Wrestrict) even though the
    // sample aliasing itself is fine. Two arrays, one buffer.
    void process(float* buf, int n) {
        FAUSTFLOAT* in[1]  = { buf };
        FAUSTFLOAT* out[1] = { buf };
        dsp.compute(n, in, out);
    }
};

} // namespace spotykach
