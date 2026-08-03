declare name "chorus";
declare description "Stereo chorus - sk-engines Faust demo engine (generated wrapper).";

import("stdfaust.lib");

// Flat hsliders (no group boxes), each 0..1 like the platform knobs; the wrapper linear-maps the
// platform 0..1 into these ranges automatically. Labels are the manifest binding keys.
rate  = hslider("rate",  0.30, 0, 1, 0.001) : si.smoo;   // LFO rate
depth = hslider("depth", 0.50, 0, 1, 0.001) : si.smoo;   // modulation depth
del   = hslider("delay", 0.40, 0, 1, 0.001) : si.smoo;   // base delay
mix   = hslider("mix",   0.50, 0, 1, 0.001) : si.smoo;   // dry/wet

rateHz = 0.05 + rate * 5.0;                  // 0.05 .. 5 Hz
baseSamp = (0.005 + del * 0.015) * ma.SR;    // 5 .. 20 ms, in samples
modSamp  = depth * 0.005 * ma.SR;            // +/- 5 ms, in samples

// Two slightly-detuned LFOs for stereo width.
//
// os.m_oscsin, NOT os.osc. Both are sine oscillators of identical waveform and phase, but os.osc is
// os.oscsin - an rdtable over the platform library's tablesize, which is 1<<16. That table is a
// STATIC 65536-float array in the generated kernel (262144 B), and it does not live in the dsp object,
// so the arena placement-new that keeps the rest of the kernel state out of SRAM cannot move it. It
// landed in .bss and overflowed the SRAM data region by 98808 B - the whole engine failed to link.
//
// os.m_oscsin(f) = lf_sawpos(f) : *(2*ma.PI) : sin - a phase accumulator through the sin function,
// with no table at all. Same waveform, and CONTINUOUS rather than quantized to a table step, which
// matters here: these LFOs modulate a fractional delay, where a stepped control signal is audible as
// zipper noise. A smaller table would have traded the memory back for exactly that artifact.
//
// The cost is two sinf() per sample (~1.6% of one core at 48 kHz), spending compute - which this M7
// has in abundance - to buy back SRAM, which is the resource this engine actually ran out of.
delL = baseSamp + modSamp * (0.5 + 0.5 * os.m_oscsin(rateHz));
delR = baseSamp + modSamp * (0.5 + 0.5 * os.m_oscsin(rateHz * 1.03));

wet(d, x) = de.fdelay(2048, d, x);           // 2048-sample max (compile-time), fractional delay

chL(x) = x * (1.0 - mix) + wet(delL, x) * mix;
chR(x) = x * (1.0 - mix) + wet(delR, x) * mix;

process = chL, chR;
