// Host micro-benchmark for the grain-cloud SCATTERED-READ access pattern (engine #11, GrainflowLib
// port). This is the decision gate the idea-doc (docs/engine-ideas.md, #11) mandates BEFORE committing
// to the port: "Benchmark N scattered grain reads/block headless in host/ ... that single measurement
// decides feasibility and the max grain count."
//
// What it models: N independent grains, each holding its own fractional read position and rate, reading
// `kBlock` interpolated stereo samples per audio block from a shared multi-MB buffer, applying a Hann
// window, and summing (with a per-grain pan) into the stereo bus. At each grain's window boundary the
// read position is re-scattered to a random location across the whole buffer - that random restart is
// the access pattern the H7's SDRAM punishes. The interpolation math (Catmull-Rom cubic / linear) and
// the int16 storage mirror src/engine/granular/buffer.cpp, so the per-sample cost is representative.
//
// IMPORTANT CAVEAT (stated in the idea-doc): a desktop CANNOT reproduce the H7 external-SDRAM random-
// access penalty - this buffer may live in the desktop's L3 cache. So the numbers here are a CPU/
// algorithmic LOWER BOUND and a SCALING signal (how cost grows with N), NOT a device load %. The
// decisive SDRAM measurement is an on-device METER run before final commit. Use this to (a) confirm the
// cost scales sanely with grain count and (b) reject the port early if even the CPU cost blows budget.

#include <chrono>
#include <cmath>
#include <initializer_list>   // the block-size sweep deduces from a braced-init-list
#include <cstdint>
#include <cstdio>

namespace {

constexpr float  kSampleRate = 48000.f;
constexpr int    kBlock      = 96;
constexpr double kPeriodUs   = 1e6 * kBlock / kSampleRate; // 2000 us @ 96 smp / 48 kHz

// 16 MB stereo int16 buffer (~87 s): deliberately larger than a typical desktop L2 (and most of L3) so
// random restarts actually miss cache, approximating - imperfectly - the SDRAM random-access cost.
constexpr int kFrames = 4 * 1024 * 1024;     // 4,194,304 frames
struct Frame { int16_t l, r; };
Frame g_buf[kFrames];

inline float dec(int16_t s) { return static_cast<float>(s) * (1.f / 32767.f); }

struct LCG {
    uint32_t s;
    inline uint32_t u32() { s = s * 1664525u + 1013904223u; return s; }
    inline float f01()  { return static_cast<float>(u32() >> 8) * (1.f / 16777216.f); }
};

// Catmull-Rom cubic read at a fractional frame, mirroring Buffer::read_cubic. Wraps via mask (kFrames is
// a power of two) so the scattered restart can land anywhere without a branch.
inline void read_cubic(float frame, float& o0, float& o1) {
    const uint32_t i  = static_cast<uint32_t>(frame);
    const float    t  = frame - static_cast<float>(i);
    const uint32_t m  = kFrames - 1;
    const Frame& a = g_buf[(i - 1) & m];
    const Frame& b = g_buf[ i      & m];
    const Frame& c = g_buf[(i + 1) & m];
    const Frame& d = g_buf[(i + 2) & m];
    const float a0 = dec(a.l), b0 = dec(b.l), c0 = dec(c.l), d0 = dec(d.l);
    const float a1 = dec(a.r), b1 = dec(b.r), c1 = dec(c.r), d1 = dec(d.r);
    const float c0_0 = b0, c1_0 = .5f * (c0 - a0),
                c2_0 = a0 - 2.5f * b0 + 2.f * c0 - .5f * d0,
                c3_0 = .5f * (d0 - a0) + 1.5f * (b0 - c0);
    const float c0_1 = b1, c1_1 = .5f * (c1 - a1),
                c2_1 = a1 - 2.5f * b1 + 2.f * c1 - .5f * d1,
                c3_1 = .5f * (d1 - a1) + 1.5f * (b1 - c1);
    o0 = (((c3_0 * t + c2_0) * t + c1_0) * t + c0_0);
    o1 = (((c3_1 * t + c2_1) * t + c1_1) * t + c0_1);
}

inline void read_linear(float frame, float& o0, float& o1) {
    const uint32_t i = static_cast<uint32_t>(frame);
    const float    t = frame - static_cast<float>(i);
    const uint32_t m = kFrames - 1;
    const Frame& b = g_buf[ i      & m];
    const Frame& c = g_buf[(i + 1) & m];
    o0 = dec(b.l) + t * (dec(c.l) - dec(b.l));
    o1 = dec(b.r) + t * (dec(c.r) - dec(b.r));
}

struct Grain {
    float    pos;        // fractional read head
    float    rate;       // increment per sample (pitch)
    int      remaining;  // samples until window restart
    int      win;        // window length in samples
    float    panL, panR; // equal-power pan gains
    uint32_t phase;      // Hann phase counter
    LCG      rng;
};

// One grain renders `kBlock` samples into the accumulators. On window end it re-scatters to a random
// buffer position (the cache-missing event) and re-randomizes rate/pan/window - the cloud behaviour.
template <bool Cubic>
inline void render_grain(Grain& gr, float* accL, float* accR) {
    for (int n = 0; n < kBlock; n++) {
        if (gr.remaining <= 0) {
            gr.win       = 1024 + static_cast<int>(gr.rng.f01() * 8192.f); // ~21..190 ms
            gr.remaining = gr.win;
            gr.pos       = gr.rng.f01() * (kFrames - 4);                   // scatter across whole buffer
            gr.rate      = 0.5f + gr.rng.f01() * 1.5f;                     // 0.5x..2x
            const float p = gr.rng.f01();
            gr.panL = std::cos(p * 1.5707963f);
            gr.panR = std::sin(p * 1.5707963f);
            gr.phase = 0;
        }
        float s0, s1;
        if (Cubic) read_cubic(gr.pos, s0, s1); else read_linear(gr.pos, s0, s1);
        // Hann window over the grain lifetime.
        const float w = 0.5f - 0.5f * std::cos(6.2831853f * static_cast<float>(gr.phase) /
                                               static_cast<float>(gr.win));
        accL[n] += s0 * w * gr.panL;
        accR[n] += s1 * w * gr.panR;
        gr.pos += gr.rate;
        gr.phase++;
        gr.remaining--;
    }
}

template <bool Cubic>
double bench(int n_grains, int blocks) {
    static Grain grains[256];
    LCG seed{ 0xC0FFEEu };
    for (int g = 0; g < n_grains; g++) { grains[g] = Grain{}; grains[g].rng = LCG{ 0x1234u + 2654435761u * static_cast<uint32_t>(g) }; (void)seed; }
    float accL[kBlock], accR[kBlock];
    volatile double checksum = 0;
    // Warm up (let grains restart at least once so we time steady-state scatter).
    for (int b = 0; b < 200; b++) {
        for (int n = 0; n < kBlock; n++) { accL[n] = accR[n] = 0.f; }
        for (int g = 0; g < n_grains; g++) render_grain<Cubic>(grains[g], accL, accR);
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; b++) {
        for (int n = 0; n < kBlock; n++) { accL[n] = accR[n] = 0.f; }
        for (int g = 0; g < n_grains; g++) render_grain<Cubic>(grains[g], accL, accR);
        checksum = checksum + accL[0] + accR[kBlock - 1];
    }
    const auto t1 = std::chrono::steady_clock::now();
    (void)checksum;
    return std::chrono::duration<double, std::micro>(t1 - t0).count() / blocks;
}

bool finite_check() {
    static Grain grains[64];
    for (int g = 0; g < 64; g++) { grains[g] = Grain{}; grains[g].rng = LCG{ 7u + 99u * static_cast<uint32_t>(g) }; }
    float accL[kBlock], accR[kBlock];
    for (int b = 0; b < 5000; b++) {
        for (int n = 0; n < kBlock; n++) { accL[n] = accR[n] = 0.f; }
        for (int g = 0; g < 64; g++) render_grain<true>(grains[g], accL, accR);
        for (int n = 0; n < kBlock; n++) if (!std::isfinite(accL[n]) || !std::isfinite(accR[n])) return false;
    }
    return true;
}

} // namespace

int main() {
    LCG rng{ 424242u };
    for (int i = 0; i < kFrames; i++) {
        g_buf[i].l = static_cast<int16_t>((static_cast<int32_t>(rng.u32() >> 16) - 32768));
        g_buf[i].r = static_cast<int16_t>((static_cast<int32_t>(rng.u32() >> 16) - 32768));
    }

    std::printf("grain-cloud scattered-read benchmark (16 MB int16 stereo buffer, %d-frame)\n", kFrames);
    std::printf("block period = %.0f us @ %d smp / %.0f Hz\n", kPeriodUs, kBlock, kSampleRate);
    std::printf("NOTE: desktop host CANNOT show the H7 SDRAM random-access penalty; these are a CPU\n");
    std::printf("      lower bound + a SCALING signal, not a device load %%. Confirm on-device (METER).\n\n");
    std::printf("  %-7s | %-26s | %-26s\n", "grains", "CUBIC (us/blk, host-load)", "LINEAR (us/blk, host-load)");
    std::printf("  --------+----------------------------+---------------------------\n");
    const int kBlocks = 20000;
    for (int n : { 8, 16, 24, 32, 48, 64, 96, 128 }) {
        const double cub = bench<true >(n, kBlocks);
        const double lin = bench<false>(n, kBlocks);
        std::printf("  %-7d | %10.3f  (%6.2f%%)        | %10.3f  (%6.2f%%)\n",
                    n, cub, 100.0 * cub / kPeriodUs, lin, 100.0 * lin / kPeriodUs);
    }
    std::printf("\nper-grain cubic cost ~= (us/blk at N) / N; pick max grains where host-load leaves\n");
    std::printf("generous margin (device is ~15-25x slower per op AND adds SDRAM latency on top).\n");
    std::printf("\nfinite-output check (64 grains, 5000 blocks): %s\n", finite_check() ? "PASS" : "FAIL");
    return 0;
}
