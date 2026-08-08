// Full-engine test for graincloud. Goal: determine whether the GrainflowLib cloud actually reads the
// recorded/loaded Buffer, or granulates garbage. It writes a KNOWN DC value straight into each deck's
// buffer via the storage port (audio_data + audio_apply_loaded) - i.e. simulating a perfect SD load,
// bypassing the card - then runs the engine wet and checks the output tracks the buffer content.
//
//   - buffer = V (constant) -> cloud output non-zero, bounded, and its level scales with V
//   - buffer = 0            -> output ~silent
// If the output does NOT scale with V, the cloud is not reading the buffer (the real bug). If it DOES,
// the cloud read is fine and the fault is in the SD load path.

#include <cmath>
#include <cstdio>
#include <functional>

#include "engine/graincloud/graincloud_engine.h"
#include "engine/granular/buffer.h"   // shared: graincloud is the granular tree built with SPK_GRAIN_GF
#include "engine/itransport.h"
#include "host_setup.h"

using namespace spotykach;

namespace {
int g_failures = 0;
void check(bool c, const char* m) { if (!c) { std::printf("  FAIL: %s\n", m); g_failures++; } }

struct StubTransport : ITransport {
    float               tempo() const override { return 120.f; }
    ClockSource::Source source() const override { return ClockSource::internal; }
    bool                is_external_sync() const override { return false; }
    uint8_t             key_interval() const override { return 4; }
    bool                is_key_sub_quarter() const override { return false; }
    void set_on_tick(std::function<void(const TransportTick&)>) override {}
};

// Write a constant value into both decks' buffers (simulate a loaded sample), N frames.
void fill_buffer(GraincloudEngine& e, float v, size_t n) {
    for (auto d : { DeckRef::A, DeckRef::B }) {
        auto* raw = reinterpret_cast<Buffer::Frame*>(e.audio_data(d));
        size_t cap = e.audio_capacity_bytes(d) / sizeof(Buffer::Frame);
        size_t nn = n < cap ? n : cap;
        for (size_t i = 0; i < nn; i++) { raw[i].l = v; raw[i].r = v; }
        e.audio_apply_loaded(d, nn);
    }
}

// Run blocks wet (in=0), return RMS + peak of the left output.
void measure(GraincloudEngine& e, int blocks, float& rms, float& peak) {
    float il[host::kBlock] = {0}, ir[host::kBlock] = {0}, ol[host::kBlock], orr[host::kBlock];
    const float* in[2] = { il, ir }; float* out[2] = { ol, orr };
    double acc = 0; peak = 0; size_t n = 0;
    for (int b = 0; b < blocks; b++) {
        e.process(in, out, host::kBlock);
        for (size_t i = 0; i < host::kBlock; i++) {
            const float a = std::fabs(ol[i]);
            if (a > peak) peak = a;
            acc += static_cast<double>(ol[i]) * ol[i]; n++;
        }
    }
    rms = n ? static_cast<float>(std::sqrt(acc / n)) : 0.f;
}

void setup_wet(GraincloudEngine& e) {
    for (auto d : { DeckRef::A, DeckRef::B }) {
        e.set_param(ParamId::Mix,    d, 1.0f);  // fully wet -> output is the cloud
        e.set_param(ParamId::Env,    d, 0.5f);  // mid duration
        e.set_param(ParamId::ModAmp, d, 0.0f);  // no spread (coherent, easier to read)
        e.set_mod_speed(d, 0.6f, false);        // some density
    }
}
}

int main() {
    host::TimeSource time; host::HostArena arena; StubTransport transport;
    EngineContext ctx = host::make_context(arena, time);
    ctx.transport = &transport;

    GraincloudEngine e; e.init(ctx);
    setup_wet(e);

    float rms_hi, pk_hi, rms_lo, pk_lo, rms_zero, pk_zero;

    fill_buffer(e, 0.5f, 48000);
    e.on_play_pad(DeckRef::A, false);   // Play gates the cloud now - start both decks
    e.on_play_pad(DeckRef::B, false);
    measure(e, 400, rms_hi, pk_hi);   // settle + measure

    fill_buffer(e, 0.125f, 48000);
    measure(e, 400, rms_lo, pk_lo);

    fill_buffer(e, 0.0f, 48000);
    measure(e, 400, rms_zero, pk_zero);

    std::printf("graincloud engine test (cloud-reads-buffer)\n");
    std::printf("  V=0.5   -> rms=%.5f peak=%.5f\n", rms_hi, pk_hi);
    std::printf("  V=0.125 -> rms=%.5f peak=%.5f\n", rms_lo, pk_lo);
    std::printf("  V=0.0   -> rms=%.5f peak=%.5f\n", rms_zero, pk_zero);

    check(pk_hi > 0.01f,            "cloud is audible when the buffer has content");
    check(rms_zero < rms_hi * 0.1f, "empty buffer is (near) silent");
    check(rms_hi > rms_lo * 2.0f,   "output level tracks buffer content (cloud READS the buffer)");

    // --- Playhead-motion check: first half +0.4, second half -0.4; POS=0. A forward-moving playhead ->
    //     output mean swings positive (early) then negative (late). Stuck playhead -> stays positive. ---
    {
        GraincloudEngine e2; e2.init(ctx);
        const size_t N = 48000;                          // 1 s sample -> playhead sweeps in ~500 blocks
        for (auto d : { DeckRef::A, DeckRef::B }) {       // fill+play BOTH so the crossfader can't hide it
            e2.set_param(ParamId::Mix,  d, 1.0f);
            e2.set_param(ParamId::Pos,  d, 0.0f);         // start playhead at the very beginning
            e2.set_param(ParamId::Size, d, 0.4f);         // mid grain size
            e2.set_param(ParamId::Env,  d, 0.0f);         // no spray (clean playhead)
            e2.set_mod_speed(d, 0.6f, false);
            auto* raw = reinterpret_cast<Buffer::Frame*>(e2.audio_data(d));
            for (size_t i = 0; i < N; i++) { float v = i < N/2 ? 0.4f : -0.4f; raw[i].l = v; raw[i].r = v; }
            e2.audio_apply_loaded(d, N);
            e2.on_play_pad(d, false);
        }

        float il[host::kBlock] = {0}, ir[host::kBlock] = {0}, ol[host::kBlock], orr[host::kBlock];
        const float* in[2] = { il, ir }; float* out[2] = { ol, orr };
        double early = 0, late = 0; int ne = 0, nl = 0;
        for (int b = 0; b < 480; b++) {                  // ~< one full sweep (midpoint ~block 250)
            e2.process(in, out, host::kBlock);
            for (size_t i = 0; i < host::kBlock; i++) {
                if (b >= 20  && b < 120) { early += ol[i]; ne++; }   // playhead in first half (+)
                if (b >= 330 && b < 430) { late  += ol[i]; nl++; }   // playhead in second half (-)
            }
        }
        early = ne ? early/ne : 0; late = nl ? late/nl : 0;
        std::printf("  playhead: early_mean=%+.4f late_mean=%+.4f\n", early, late);
        check(early > 0.01f,  "playhead reads the FIRST half early (positive)");
        check(late  < -0.01f, "playhead has ADVANCED to the second half later (negative) -> it moves");
    }

    if (g_failures == 0) { std::printf("OK: cloud reads the buffer\n"); return 0; }
    std::printf("FAILED: %d - the cloud is NOT tracking the buffer (read bug, not SD-load)\n", g_failures);
    return 1;
}
