// Desktop host harness for the Spotykach DSP core.
//
// Runs the real spotykach::Core off-target over a WAV file: allocates the large buffers
// on the heap, injects a monotonic ITimeSource, and pumps audio through Core::process in
// 96-frame blocks at the same 500 Hz block rate as the hardware (48000 / 96). This gives a
// fast iteration loop and a regression net before any UI/engine refactor.
//
// Usage:
//   host_spotykach [input.wav] [output.wav] [--record]
//   - no input:  a 1 s 220 Hz test tone is synthesised
//   - --record:  arm deck A, record the input, then play the loop back (best-effort)
//
// Default behaviour (no --record) is a pass-through, which deterministically exercises the
// whole DSP graph end to end.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <memory>   // std::make_unique (the engine is heap-held so the large buffers stay off the stack)
#include <string>
#include <vector>

#include "engine/granular/core.h"
#include "engine/granular/mode.h"
#include "engine/granular/granular_engine.h"
#include "transport/transport.h"

#include "host_setup.h"
#include "wav.h"

using namespace spotykach;

namespace {

constexpr size_t kBlock = host::kBlock;
constexpr float  kSampleRate = host::kSampleRate;

void synth_tone(host::Audio& a, float seconds, float freq) {
    a.sample_rate = 48000;
    a.channels = 2;
    size_t n = static_cast<size_t>(seconds * a.sample_rate);
    a.l.resize(n);
    a.r.resize(n);
    for (size_t i = 0; i < n; i++) {
        float v = 0.5f * std::sin(2.f * 3.14159265f * freq * i / a.sample_rate);
        a.l[i] = v;
        a.r[i] = v;
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string in_path, out_path = "build/host_out.wav";
    bool do_record = false;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--record") do_record = true;
        else if (in_path.empty()) in_path = arg;
        else out_path = arg;
    }

    host::Audio in;
    if (!in_path.empty()) {
        if (!host::read_wav(in_path, in)) {
            std::fprintf(stderr, "Failed to read 16-bit PCM WAV: %s\n", in_path.c_str());
            return 1;
        }
        std::printf("Read %s: %zu frames, %u Hz, %u ch\n", in_path.c_str(), in.frames(), in.sample_rate, in.channels);
    } else {
        synth_tone(in, 1.0f, 220.f);
        std::printf("No input given; synthesised 1 s 220 Hz tone\n");
    }

    // --- set up the core ---
    host::TimeSource time;
    host::HostArena hb;
    auto ctx = host::make_context(hb, time);

    // The transport is a platform-owned service the engine only observes (post "decouple transport"):
    // construct + init it and inject it via the context BEFORE init() so the granular Core subscribes
    // to its ticks. The host plays the platform's role of driving it (transport.tick() per block).
    Transport transport;
    transport.init(kSampleRate, static_cast<float>(kBlock), &time);
    ctx.transport = &transport;

    // Drive the audio lifecycle through the IEngine interface (proving the Phase 2 seam
    // off-target); reach the graph directly via core() for the host-side UI-equivalent setup.
    auto engine = std::make_unique<GranularEngine>();
    IEngine& eng = *engine;
    eng.init(ctx);
    // The transport fires these per quarter / clock-out; the UI installs them on hardware, so
    // the host must provide no-ops or the empty std::functions throw bad_function_call.
    transport.set_on_quarter([](bool) {});
    transport.set_on_clock_out([]() {});

    // Force a known route + mode via the public config API (item 3a-4 removed core()).
    eng.set_config(ConfigId::Route, DeckRef::A, 0); // Stereo
    eng.set_config(ConfigId::Route, DeckRef::A, 1); // DoubleMono (forces a change off the default)
    for (auto ref : {DeckRef::A, DeckRef::B}) eng.set_config(ConfigId::Mode, ref, 1); // Reel (also infers panner)

    // Tail to let any loop play out after the input ends.
    size_t tail_frames = do_record ? static_cast<size_t>(kSampleRate) * 2 : 0;
    size_t total_frames = in.frames() + tail_frames;
    // round up to whole blocks
    total_frames = ((total_frames + kBlock - 1) / kBlock) * kBlock;

    host::Audio out;
    out.sample_rate = 48000;
    out.channels = 2;
    out.l.resize(total_frames);
    out.r.resize(total_frames);

    if (do_record) eng.on_record_pad(DeckRef::A, false); // arm record (set_source + toggle_recording); detector records on sound

    float in_l[kBlock], in_r[kBlock];
    float out_l[kBlock], out_r[kBlock];
    const float* in_ptrs[2] = {in_l, in_r};
    float* out_ptrs[2] = {out_l, out_r};

    bool switched_to_play = false;
    for (size_t pos = 0; pos < total_frames; pos += kBlock) {
        // Once the input is consumed, optionally stop recording and start playback.
        if (do_record && !switched_to_play && pos >= in.frames()) {
            eng.on_play_pad(DeckRef::A, false); // disarm + start playback (approximates the old disarm()+play())
            switched_to_play = true;
        }

        for (size_t i = 0; i < kBlock; i++) {
            size_t s = pos + i;
            in_l[i] = (s < in.frames()) ? in.l[s] : 0.f;
            in_r[i] = (s < in.frames()) ? in.r[s] : 0.f;
        }

        transport.tick(false);               // internal clock advances one block; fans a TransportTick
        eng.process(in_ptrs, out_ptrs, kBlock);

        for (size_t i = 0; i < kBlock; i++) {
            out.l[pos + i] = out_l[i];
            out.r[pos + i] = out_r[i];
        }
    }

    // Report peak so a silent/NaN run is obvious.
    float peak = 0.f;
    bool nan = false;
    for (size_t i = 0; i < total_frames; i++) {
        peak = std::max(peak, std::max(std::fabs(out.l[i]), std::fabs(out.r[i])));
        if (std::isnan(out.l[i]) || std::isnan(out.r[i])) nan = true;
    }
    std::printf("Processed %zu frames; output peak %.4f%s\n", total_frames, peak, nan ? "  [WARNING: NaN in output]" : "");

    if (!host::write_wav(out_path, out)) {
        std::fprintf(stderr, "Failed to write %s\n", out_path.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", out_path.c_str());
    return nan ? 2 : 0;
}
