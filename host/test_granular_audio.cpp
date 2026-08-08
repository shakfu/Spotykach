// Host test for the granular engine's AUDIO path - loading, playing, and recording real samples.
//
// Why this exists alongside test_engine_params.cpp: that test drives the *parameter surface* (every
// ParamId round-trips across every mode, output stays finite, capabilities are right). It never puts
// audio through the engine, so a break anywhere in buffer -> deck -> generator -> output would leave
// it green. Granular is the default `make` target and the tree every other looper was cloned from,
// and until now nothing off-target asserted that it makes a sound at all.
//
// This is deliberately the same shape as test_graincloud.cpp, which does the equivalent job for the
// engine that shares this source tree - so the two can be compared when one of them regresses.
//
// What it pins:
//   1. **Load -> play produces audio that tracks the buffer.** A known DC level goes in through the
//      storage port (audio_data + audio_apply_loaded, i.e. a simulated SD load, no card involved) and
//      the output level must scale with it. Halve the input, the output roughly halves; zero it and
//      the engine goes quiet. That is the whole read path in three assertions.
//   2. **An empty deck is silent** and reports itself empty - the state the platform lights the Play
//      LED from.
//   3. **Record captures the input.** Arm, feed a signal (recording is level-triggered at ~-40 dB),
//      stop, then play back and assert audio comes out. This is the path a user touches first.
//   4. **Every mode renders finite audio** - Reel, Slice and Drift take different branches through
//      Generator/Vox, and Drift in particular runs the grain scheduler.
//
// Reel is used for the level assertions on purpose: it is the unsynced mode, so playback does not
// depend on transport ticks and the test needs no clock to be driven.

#include <cmath>
#include <cstdio>
#include <functional>

#include "engine/granular/granular_engine.h"
#include "engine/granular/buffer.h"
#include "transport/transport.h"
#include "host_setup.h"

using namespace spotykach;

namespace {

int g_failures = 0;
void check(bool c, const char* m) { if (!c) { std::printf("  FAIL: %s\n", m); g_failures++; } }

// ConfigId::Mode carries the PHYSICAL SWITCH POSITION, not the Mode enum value - granular maps it
// `v == 2 ? Drift : v == 1 ? Reel : Slice` (granular_engine.cpp), so the switch order is
// Slice / Reel / Drift while the enum order is Reel=0, Slice=1, Drift=2. Passing
// `static_cast<int>(Mode::Reel)` selects SLICE. Named here so the test cannot make that mistake
// silently: it did on the first run, and the symptom was an engine that looked completely dead
// (Slice queues playback until a key/bar boundary, which a 200-block render never reaches).
constexpr int kSwitchSlice = 0;
constexpr int kSwitchReel  = 1;
constexpr int kSwitchDrift = 2;

struct Stats { float rms = 0.f, peak = 0.f; bool finite = true; };

// Render `blocks` blocks with the given constant input level, summarising the left output.
//
// transport.tick() per block is NOT optional here, and is the reason this test looked like a silent
// engine on the first run: `Deck::toggle_play()` QUEUES playback (`_is_play_queued`) and the queue is
// drained from `Deck::tick()`, which only runs off a transport tick. With a stub ITransport that
// never fires, the deck sits at playing=0/queued=1 forever and renders silence. The host plays the
// platform's role - app.cpp drives the real Transport once per audio block - so the test must too.
Stats render(Transport& transport, GranularEngine& e, int blocks, float in_level = 0.f) {
    float il[host::kBlock], ir[host::kBlock], ol[host::kBlock], orr[host::kBlock];
    for (size_t i = 0; i < host::kBlock; i++) { il[i] = in_level; ir[i] = in_level; }
    const float* in[2] = { il, ir }; float* out[2] = { ol, orr };
    Stats s; double acc = 0; size_t n = 0;
    for (int b = 0; b < blocks; b++) {
        transport.tick(false);
        e.process(in, out, host::kBlock);
        for (size_t i = 0; i < host::kBlock; i++) {
            for (float v : { ol[i], orr[i] }) if (!std::isfinite(v)) s.finite = false;
            const float a = std::fabs(ol[i]);
            if (a > s.peak) s.peak = a;
            acc += static_cast<double>(ol[i]) * ol[i]; n++;
        }
    }
    s.rms = n ? static_cast<float>(std::sqrt(acc / n)) : 0.f;
    return s;
}

// Write a constant level into both decks' loop buffers through the storage port - the same entry
// point Storage uses after reading a WAV off the card, so this exercises the real load path.
void load_buffer(GranularEngine& e, float v, size_t frames) {
    for (auto d : { DeckRef::A, DeckRef::B }) {
        auto* raw = reinterpret_cast<Buffer::Frame*>(e.audio_data(d));
        const size_t cap = e.audio_capacity_bytes(d) / sizeof(Buffer::Frame);
        const size_t n = frames < cap ? frames : cap;
        for (size_t i = 0; i < n; i++) { raw[i].l = v; raw[i].r = v; }
        e.audio_apply_loaded(d, n);
    }
}

// Fully wet, whole-buffer playback: what the level assertions need to be readable.
void setup_wet(GranularEngine& e) {
    for (auto d : { DeckRef::A, DeckRef::B }) {
        e.set_config(ConfigId::Mode, d, kSwitchReel);   // unsynced: plays without waiting for a bar
        e.set_param(ParamId::Mix,   d, 1.0f);   // playback only, no input bleed
        e.set_param(ParamId::Pos,   d, 0.0f);   // from the start of the loop
        e.set_param(ParamId::Size,  d, 1.0f);   // the whole loop
        e.set_param(ParamId::Speed, d, 0.5f);   // unity
        e.set_param(ParamId::Env,   d, 0.0f);   // no loop envelope - keep the level flat
    }
}

} // namespace

int main() {
    std::printf("=== granular audio path (load / play / record) ===\n");

    host::TimeSource time; host::HostArena arena;
    EngineContext ctx = host::make_context(arena, time);

    // A real platform Transport, injected BEFORE init() so granular Core can subscribe to its ticks.
    Transport transport;
    transport.init(host::kSampleRate, static_cast<float>(host::kBlock), &time);
    ctx.transport = &transport;

    GranularEngine e;
    e.init(ctx);
    setup_wet(e);

    // --- an empty deck is silent, and says so -------------------------------------------------
    {
        check(e.audio_is_empty(DeckRef::A), "a fresh deck reports empty");
        const Stats s = render(transport, e, 60);
        check(s.finite, "an empty engine renders finite output");
        check(s.rms < 1e-5f, "an empty engine is silent");
    }

    // --- load -> play, and the output tracks the buffer ----------------------------------------
    Stats hi, lo, zero;
    {
        load_buffer(e, 0.5f, 48000);          // 1 s of DC at 0.5
        check(!e.audio_is_empty(DeckRef::A), "deck reports non-empty after a load");
        e.on_play_pad(DeckRef::A, false);
        e.on_play_pad(DeckRef::B, false);
        render(transport, e, 100);                        // settle (fades, smoothers)
        hi = render(transport, e, 200);

        load_buffer(e, 0.125f, 48000);        // a quarter of the level
        render(transport, e, 100);
        lo = render(transport, e, 200);

        load_buffer(e, 0.0f, 48000);
        render(transport, e, 100);
        zero = render(transport, e, 200);

        std::printf("  buffer=0.5   -> rms=%.5f peak=%.5f\n", hi.rms, hi.peak);
        std::printf("  buffer=0.125 -> rms=%.5f peak=%.5f\n", lo.rms, lo.peak);
        std::printf("  buffer=0.0   -> rms=%.5f peak=%.5f\n", zero.rms, zero.peak);

        check(hi.finite && lo.finite && zero.finite, "playback output is finite");
        check(hi.rms > 1e-3f, "a loaded, playing deck produces audio");
        check(lo.rms < hi.rms, "a quieter buffer plays back quieter");
        check(zero.rms < 1e-5f, "a silent buffer plays back silent");
        // The engine reads the buffer rather than synthesising: a 4x quieter buffer should be
        // roughly 4x quieter out. Loose bounds - windowing and fades move the exact ratio.
        const float ratio = hi.rms / (lo.rms > 0.f ? lo.rms : 1e-9f);
        std::printf("  level ratio (expect ~4) = %.2f\n", ratio);
        check(ratio > 2.f && ratio < 8.f, "output level scales with buffer content");
    }

    // --- record captures the input --------------------------------------------------------------
    // Recording is level-triggered (~-40 dB), so the arm is followed by real signal, not silence.
    {
        GranularEngine r;
        host::HostArena arena2; host::TimeSource time2;
        EngineContext ctx2 = host::make_context(arena2, time2);
        Transport transport2;
        transport2.init(host::kSampleRate, static_cast<float>(host::kBlock), &time2);
        ctx2.transport = &transport2;
        r.init(ctx2);
        setup_wet(r);

        check(r.audio_is_empty(DeckRef::A), "deck starts empty before recording");

        r.on_record_pad(DeckRef::A, false);       // arm
        render(transport2, r, 400, 0.4f);                     // feed signal: crosses the threshold and records
        r.on_play_pad(DeckRef::A, false);         // stop recording -> loop what was captured

        check(!r.audio_is_empty(DeckRef::A), "deck is non-empty after recording");

        r.set_param(ParamId::Mix, DeckRef::A, 1.0f);   // hear the loop, not the input
        render(transport2, r, 100, 0.f);
        const Stats played = render(transport2, r, 200, 0.f);
        std::printf("  recorded loop plays back rms=%.5f peak=%.5f\n", played.rms, played.peak);
        check(played.finite, "recorded playback is finite");
        check(played.rms > 1e-3f, "what was recorded plays back as audio");
    }

    // --- every mode renders ---------------------------------------------------------------------
    // Reel / Slice / Drift take different paths through Generator and Vox; Drift runs the grain
    // scheduler. A mode that produced NaN would otherwise only show up on hardware.
    {
        load_buffer(e, 0.5f, 48000);
        // on_play_pad TOGGLES. The decks are already playing from the section above, so pressing it
        // again would STOP them - which is what happened on the first draft of this test, and the
        // per-mode checks still "passed" because they only asserted finite-and-bounded. Silence is
        // finite. Hence the explicit non-silence assertion below, and this guard.
        for (auto d : { DeckRef::A, DeckRef::B })
            if (!e.play_leds(d).playing) e.on_play_pad(d, false);
        const struct { int sw; const char* name; } modes[] = {
            { kSwitchReel, "Reel" }, { kSwitchSlice, "Slice" }, { kSwitchDrift, "Drift" } };
        for (const auto& m : modes) {
            for (auto d : { DeckRef::A, DeckRef::B }) e.set_config(ConfigId::Mode, d, m.sw);
            // Generous settle: Slice queues playback until a key/bar boundary (2 s at 120 BPM, i.e.
            // ~1000 blocks), so a short render would read as silence for reasons that are not a bug.
            render(transport, e, 1200);
            // Slice is the TRIGGERED mode: Deck::play() deliberately dispatches nothing for it
            // ("going to be triggered from tick method"), so with an empty sequencer track it is
            // correctly silent until something fires a slice. Give it a gate, as a patched CV would.
            if (m.sw == kSwitchSlice) {
                for (auto d : { DeckRef::A, DeckRef::B }) e.on_gate_trigger(d);
                render(transport, e, 50);
            }
            const Stats s = render(transport, e, 400);
            std::printf("  mode %-5s rms=%.5f peak=%.5f\n", m.name, s.rms, s.peak);
            char msg[64];
            std::snprintf(msg, sizeof(msg), "%s renders finite audio", m.name);
            check(s.finite, msg);
            std::snprintf(msg, sizeof(msg), "%s stays bounded (|x| <= 4)", m.name);
            check(s.peak <= 4.f, msg);
            std::snprintf(msg, sizeof(msg), "%s actually produces audio", m.name);
            check(s.rms > 1e-4f, msg);
        }
    }

    if (g_failures == 0) { std::printf("\nAll granular audio tests passed.\n"); return 0; }
    std::printf("\nFAILED: %d check(s)\n", g_failures);
    return 1;
}
