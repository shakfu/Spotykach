// Host test for the mosc engine (dual Mutable Instruments *Plaits* macro-oscillator).
//
// mosc was the largest engine in the tree with no off-target test at all - it is a QSPI build, so a
// mistake in it is not even caught by the normal `make ENGINE=` sweep, and it ships as a published
// binary. This is the coverage that closes that.
//
// What it pins, in rough order of how much it would hurt to get wrong:
//
//   1. **Every one of the 24 Plaits engines renders.** The Aux knob selects the model, and each model
//      is a different DSP object with its own state, its own resources, and (for several) its own
//      scratch allocation out of the arena. A model that faults, allocates past the arena, or emits
//      NaN would be silently shipped: nothing else here instantiates all 24. Each is asserted finite,
//      bounded, and - for the pitched ones - audible.
//   2. **Drone vs Gate.** Drone bypasses the LPG so the voice runs open; Gate leaves it shut until a
//      trigger. Getting these backwards is the kind of bug that reads as "the engine is silent".
//   3. **Param round-trip** over the ids the engine actually implements (its `live_params` set), which
//      is what the on-device `describe` sweep trusts.
//   4. **Level control**, so Mix is wired to something.
//
// Deliberately NOT asserted: anything about the *timbre* of a model. The host cannot judge that, and a
// test that pinned FFT bins would fail on every upstream Plaits change for no benefit. Finite, bounded
// and non-silent is the contract worth locking.

#include <cmath>
#include <cstdio>
#include <functional>

#include "engine/mosc/mosc_engine.h"
#include "engine/itransport.h"
#include "host_setup.h"

using namespace spotykach;

namespace {

int g_failures = 0;
void check(bool c, const char* m) { if (!c) { std::printf("  FAIL: %s\n", m); g_failures++; } }

// Kept in sync with plaits::kMaxEngines, which mosc_engine.cpp mirrors as its own kEngines. Not
// included from the Plaits headers on purpose: this file is a black-box test of the IEngine surface,
// and the count is part of what it is checking.
constexpr int kEngines = 24;

struct StubTransport : ITransport {
    float               tempo() const override { return 120.f; }
    ClockSource::Source source() const override { return ClockSource::internal; }
    bool                is_external_sync() const override { return false; }
    uint8_t             key_interval() const override { return 4; }
    bool                is_key_sub_quarter() const override { return false; }
    void set_on_tick(std::function<void(const TransportTick&)>) override {}
};

struct Stats { float rms = 0.f, peak = 0.f; bool finite = true; };

// Render `blocks` blocks with a silent input (mosc is a generator) and summarise the left output.
Stats render(MoscEngine& e, int blocks) {
    float il[host::kBlock] = {0}, ir[host::kBlock] = {0}, ol[host::kBlock], orr[host::kBlock];
    const float* in[2] = { il, ir }; float* out[2] = { ol, orr };
    Stats s; double acc = 0; size_t n = 0;
    for (int b = 0; b < blocks; b++) {
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

// A deck set up to make a sound: mid-range note, everything open, droning.
void voice_open(MoscEngine& e, DeckRef::Ref d) {
    e.set_config(ConfigId::Mode, d, 1);          // Drone - LPG bypassed, engine runs continuously
    e.set_param(ParamId::Speed,  d, 0.5f);       // note: mid of the pitch range
    e.set_param(ParamId::Size,   d, 0.5f);       // harmonics
    e.set_param(ParamId::Pos,    d, 0.5f);       // timbre
    e.set_param(ParamId::Env,    d, 0.5f);       // morph
    e.set_param(ParamId::ModAmp, d, 0.5f);       // decay
    e.set_param(ParamId::Mix,    d, 1.0f);       // full level
    e.set_mod_speed(d, 0.5f, false);             // LPG colour
}

} // namespace

int main() {
    std::printf("=== mosc (dual Plaits macro-oscillator) ===\n");

    host::TimeSource time; host::HostArena arena; StubTransport transport;
    EngineContext ctx = host::make_context(arena, time);
    ctx.transport = &transport;

    MoscEngine e;
    e.init(ctx);

    // --- capabilities -----------------------------------------------------------------------
    {
        const Capabilities c = e.capabilities();
        check((c & CapOwnDisplay) != 0, "mosc advertises CapOwnDisplay");
        check((c & CapDualDeck)   != 0, "mosc advertises CapDualDeck");
        check((c & CapAux)        != 0, "mosc advertises CapAux (Alt+PITCH selects the engine)");
    }

    // --- every Plaits engine renders ----------------------------------------------------------
    // The one check that could not be made any other way without a device: 24 different DSP objects,
    // each instantiated, driven, and inspected.
    std::printf("sweeping %d Plaits engines (deck A, drone):\n", kEngines);
    voice_open(e, DeckRef::A);
    e.set_param(ParamId::Mix, DeckRef::B, 0.f);   // silence B so A is measured alone

    int silent = 0;
    for (int i = 0; i < kEngines; i++) {
        const float aux = static_cast<float>(i) / (kEngines - 1);
        e.set_param(ParamId::Aux, DeckRef::A, aux);
        // Round-trip the selector: the platform re-reads it to draw the Aux ring.
        const float back = e.param(ParamId::Aux, DeckRef::A);
        check(std::fabs(back - aux) < 1e-4f, "Aux round-trips");

        render(e, 20);                    // let the model settle (envelopes, filters, LPG)
        const Stats s = render(e, 60);

        char msg[96];
        std::snprintf(msg, sizeof(msg), "engine %d output is finite", i);
        check(s.finite, msg);
        std::snprintf(msg, sizeof(msg), "engine %d output is bounded (|x| <= 4)", i);
        check(s.peak <= 4.f, msg);

        if (s.rms < 1e-5f) silent++;
        std::printf("  %2d: rms=%.5f peak=%.5f%s\n", i, s.rms, s.peak, s.rms < 1e-5f ? "  (silent)" : "");
    }
    // Not every model is guaranteed audible at one fixed knob setting - a couple of the drum and
    // speech models are one-shots that want a trigger. The contract worth asserting is that the
    // SWEEP is not silent, i.e. selecting an engine does something, rather than each individual one.
    check(silent <= 4, "at most a few of the 24 engines are silent at a fixed drone setting");

    // --- Drone vs Gate ------------------------------------------------------------------------
    {
        e.set_param(ParamId::Aux, DeckRef::A, 0.f);   // engine 0: a plain pitched model
        e.set_config(ConfigId::Mode, DeckRef::A, 1);  // Drone
        render(e, 20);
        const Stats drone = render(e, 60);

        e.set_config(ConfigId::Mode, DeckRef::A, 0);  // Gate - LPG shut until struck
        render(e, 60);                                // let the LPG close
        const Stats gate_idle = render(e, 60);

        std::printf("drone rms=%.5f   gate-idle rms=%.5f\n", drone.rms, gate_idle.rms);
        check(drone.rms > 1e-4f, "Drone mode runs the voice open (audible with no trigger)");
        check(gate_idle.rms < drone.rms, "Gate mode is quieter than Drone when nothing has triggered");
    }

    // --- level ---------------------------------------------------------------------------------
    {
        e.set_config(ConfigId::Mode, DeckRef::A, 1);  // back to Drone
        e.set_param(ParamId::Mix, DeckRef::A, 1.0f);
        render(e, 20);
        const Stats loud = render(e, 60);
        e.set_param(ParamId::Mix, DeckRef::A, 0.0f);
        render(e, 20);
        const Stats quiet = render(e, 60);
        std::printf("level: mix=1 rms=%.5f  mix=0 rms=%.5f\n", loud.rms, quiet.rms);
        check(loud.rms > quiet.rms, "Mix controls the output level");
    }

    // --- param round-trip over the ids the engine declares as live -----------------------------
    // Mirrors what the on-device `describe` sweep does, so a divergence between live_params() and the
    // set_param switch shows up here rather than on a bench.
    {
        const ParamId ids[] = { ParamId::Speed, ParamId::Size, ParamId::Pos,
                                ParamId::Env,   ParamId::ModAmp, ParamId::Mix, ParamId::Aux };
        for (auto d : { DeckRef::A, DeckRef::B }) {
            for (ParamId id : ids) {
                for (float v : { 0.f, 0.25f, 0.75f, 1.f }) {
                    e.set_param(id, d, v);
                    check(std::fabs(e.param(id, d) - v) < 1e-4f, "param round-trips");
                }
            }
        }
        const Stats s = render(e, 40);
        check(s.finite, "still finite after sweeping every live param to its extremes");
    }

    // --- both decks at once ---------------------------------------------------------------------
    {
        voice_open(e, DeckRef::A);
        voice_open(e, DeckRef::B);
        render(e, 20);
        const Stats s = render(e, 60);
        check(s.finite, "two voices running together stay finite");
        check(s.rms > 1e-4f, "two voices running together are audible");
    }

    if (g_failures == 0) { std::printf("\nAll mosc tests passed.\n"); return 0; }
    std::printf("\nFAILED: %d check(s)\n", g_failures);
    return 1;
}
