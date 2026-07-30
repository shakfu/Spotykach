#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>

// WSOLA time-scaling for the `bard` engine: change how fast a book reads WITHOUT changing the narrator's
// pitch. This is the deferred "PITCH-KEEP" experiment of the design doc (decision #3), on the reserved ENV
// knob.
//
// WHY NOT THE pstretch FFT. bard already links a streaming stack and pstretch already owns a
// phase-randomizing PaulStretch, so reusing it looks free. It is the wrong algorithm: PaulStretch's
// mechanism IS phase randomization, which destroys exactly the phase coherence that makes consonants
// intelligible. It is right for turning a voice into an ambient wash and wrong for playing a book at 1.5x.
// WSOLA (Verhelst & Roelands, "An overlap-add technique based on waveform similarity", ICASSP 1993) is a
// time-domain overlap-add that picks each next frame by WAVEFORM SIMILARITY, so periodicity - and therefore
// pitch and intelligibility - survives. It is also far cheaper.
//
// COST. The naive similarity search is the whole cost: kSearch lags x kFrame taps per hop. Searching on a
// 4x-DECIMATED copy and then refining locally cuts that ~10x, which is what makes this affordable on the
// M7 (roughly 3 MMAC/s per deck at unity, vs ~46 MMAC/s naive - an estimate, not a measurement).
//
// BIT-EXACT BYPASS. At scale 1.0 (PITCH-KEEP at zero) the class is a pure passthrough - input frames go
// straight to the output FIFO with no windowing, so turning the knob to zero restores the shipped
// varispeed sound exactly rather than approximately.
//
// Buffers are plain members (SRAM on target, not the SDRAM arena) because the similarity search is
// scattered access over a small window, and scattered SDRAM access on the H7 is ~10x slower - the lesson
// docs/dev/pstretch-impl.md paid for.

namespace spotykach {
namespace bard {

class Wsola {
public:
    static constexpr int kFrame  = 1024;   // ~21 ms at 48 kHz: >= 2 pitch periods of a low male voice
    static constexpr int kHop    = kFrame / 2;      // synthesis hop (50% overlap)
    static constexpr int kSearch = 256;    // +/- similarity search range, in input frames
    static constexpr int kDecim  = 4;      // decimation factor for the coarse search
    static constexpr int kRing   = 4096;   // power of two; >= kFrame + kSearch + max analysis hop + margin
    static constexpr int kOut    = 2048;   // output FIFO (power of two)

    void init() {
        for (int i = 0; i < kFrame; i++) {
            // Hann: two overlapped at 50% sum to exactly 1, so no output normalization is needed.
            _win[i] = 0.5f - 0.5f * std::cos(6.28318530718f * static_cast<float>(i)
                                             / static_cast<float>(kFrame));
        }
        reset();
    }

    void reset() {
        for (int i = 0; i < kRing; i++) _ring[i] = 0.f;
        for (int i = 0; i < kOut; i++)  _out[i] = 0.f;
        for (int i = 0; i < kFrame; i++) _ola[i] = 0.f;
        _wpos = 0; _apos = 0; _ola_have = 0;
        _out_r = _out_w = 0;
        _primed = false;
    }

    // Time-scale factor. `scale` is output duration / input duration: 1.0 = passthrough, < 1 = consume the
    // input faster than real time (the book reads quicker) with pitch held.
    void set_scale(float scale) {
        if (scale < 0.05f) scale = 0.05f;
        if (scale > 20.f)  scale = 20.f;
        _scale  = scale;
        _bypass = (scale > 0.9995f && scale < 1.0005f);
    }

    bool bypassed() const { return _bypass; }

    // How many input frames to feed so that `want_out` output frames can be drained. Deliberately
    // conservative: over-feeding only costs ring space, whereas under-feeding starves the output.
    uint32_t want(uint32_t want_out) const {
        const uint32_t have = _out_w - _out_r;
        if (have >= want_out) return 0;
        const uint32_t need = want_out - have;
        if (_bypass) return need;
        const int    hops = static_cast<int>((need + kHop - 1) / kHop);
        const float  ha   = static_cast<float>(kHop) / _scale;          // analysis hop
        const uint32_t consume = static_cast<uint32_t>(ha * static_cast<float>(hops)) + 1u;
        // Keep the analysis window plus its search range available ahead of the read position.
        const uint32_t buffered = _wpos - _apos;
        const uint32_t reserve  = static_cast<uint32_t>(kFrame + kSearch);
        const uint32_t target   = consume + reserve + static_cast<uint32_t>(kHop);
        return (buffered >= target) ? 0u : (target - buffered);
    }

    void feed(const float* in, uint32_t n) {
        for (uint32_t i = 0; i < n; i++) _ring[(_wpos + i) & (kRing - 1)] = in[i];
        _wpos += n;
        if (_bypass) {                       // passthrough: straight to the FIFO, no windowing at all
            for (uint32_t i = 0; i < n; i++) _push(_ring[(_apos + i) & (kRing - 1)]);
            _apos += n;
        }
    }

    void feed1(float x) { feed(&x, 1); }

    // Produce up to `n` output frames; returns how many were actually written (short if starved).
    uint32_t drain(float* out, uint32_t n) {
        if (!_bypass) while ((_out_w - _out_r) < n && _can_hop()) _do_hop();
        uint32_t got = 0;
        while (got < n && _out_r != _out_w) out[got++] = _out[_out_r++ & (kOut - 1)];
        return got;
    }

private:
    void _push(float v) {
        if ((_out_w - _out_r) >= static_cast<uint32_t>(kOut)) _out_r++;   // drop oldest rather than block
        _out[_out_w++ & (kOut - 1)] = v;
    }

    // A hop needs the analysis frame plus the whole search range readable ahead of _apos.
    bool _can_hop() const {
        return (_wpos - _apos) >= static_cast<uint32_t>(kFrame + kSearch + kHop);
    }

    inline float _at(uint32_t pos) const { return _ring[pos & (kRing - 1)]; }

    // One synthesis hop: choose the input offset whose waveform best continues what was already emitted,
    // window it, overlap-add, and emit kHop finished samples.
    void _do_hop() {
        uint32_t take = _apos;
        if (!_primed) {
            _primed = true;                       // first frame: no continuation to match yet
        } else {
            take = _best_offset();
        }

        // Overlap-add the windowed frame onto the accumulator, then emit the first kHop samples.
        for (int i = 0; i < kFrame; i++) {
            const float w = _win[i] * _at(take + static_cast<uint32_t>(i));
            if (i < kHop) _ola[i] += w; else _ola[i] = w;   // tail becomes the next hop's head
        }
        for (int i = 0; i < kHop; i++) _push(_ola[i]);
        for (int i = 0; i < kHop; i++) _ola[i] = _ola[i + kHop];
        _ola_have = kHop;

        // The natural continuation of what we just took, for the next hop's similarity match.
        for (int i = 0; i < kFrame - kHop; i++) _tmpl[i] = _at(take + static_cast<uint32_t>(kHop + i));

        // Advance the nominal analysis position by the analysis hop.
        const float ha = static_cast<float>(kHop) / _scale;
        _apos_frac += ha;
        const uint32_t step = static_cast<uint32_t>(_apos_frac);
        _apos_frac -= static_cast<float>(step);
        _apos = take + step;
    }

    // Coarse-to-fine similarity search: cross-correlate the template against candidate offsets around the
    // nominal analysis position, on a 4x-decimated signal, then refine within +/- kDecim.
    uint32_t _best_offset() {
        const int tmpl_n = kFrame - kHop;
        const uint32_t base = _apos;
        const int lo = -kSearch / 2, hi = kSearch / 2;

        int   best = 0;
        float best_score = -1e30f;
        for (int off = lo; off <= hi; off += kDecim) {
            float acc = 0.f;
            for (int i = 0; i < tmpl_n; i += kDecim)
                acc += _tmpl[i] * _at(base + static_cast<uint32_t>(off + i));
            if (acc > best_score) { best_score = acc; best = off; }
        }
        const int rlo = best - kDecim + 1, rhi = best + kDecim - 1;
        for (int off = rlo; off <= rhi; off++) {
            if (off == best) continue;
            float acc = 0.f;
            for (int i = 0; i < tmpl_n; i += kDecim)
                acc += _tmpl[i] * _at(base + static_cast<uint32_t>(off + i));
            if (acc > best_score) { best_score = acc; best = off; }
        }
        // Never step backwards past what the ring still holds, and never outrun the written data.
        int64_t take = static_cast<int64_t>(base) + best;
        if (take < 0) take = 0;
        return static_cast<uint32_t>(take);
    }

    float _ring[kRing] = { 0.f };
    float _out[kOut]   = { 0.f };
    float _ola[kFrame] = { 0.f };
    float _tmpl[kFrame] = { 0.f };
    float _win[kFrame] = { 0.f };

    uint32_t _wpos = 0;          // ring write position (free-running)
    uint32_t _apos = 0;          // nominal analysis position (free-running)
    float    _apos_frac = 0.f;
    int      _ola_have = 0;
    uint32_t _out_r = 0, _out_w = 0;
    float    _scale = 1.f;
    bool     _bypass = true;
    bool     _primed = false;
};

} // namespace bard
} // namespace spotykach
