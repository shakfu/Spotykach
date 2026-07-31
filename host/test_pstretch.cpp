// Headless test for the pstretch engine (real-time clean-room PaulStretch ambient time-smear).
// Two layers: (A) the vendored radix-2 FFT (round-trip identity + a known single-bin spectrum), and
// (B) the PstretchEngine through its public IEngine surface (finite/bounded, produces a wet smear, stretch
// and freeze change the output, dry passthrough at MIX=0, param readback, routing).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "engine/pstretch/pstretch_engine.h"
#include "engine/pstretch/fft.h"
#include "engine/itransport.h"
#include "engine/arena.h"
#include "host_setup.h"

using namespace spotykach;

namespace {

int g_failures = 0;
void check(bool cond, const char* msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg); g_failures++; }
}

// An engine sub-allocates raw pointers from ctx.arena and holds them for its lifetime (on device the
// arena is the persistent SDRAM block). The HostArena backing it must therefore outlive the engine -
// scoping it to just the init() call (a lambda/brace block) frees it and dangles the engine's buffers
// (pstretch's ~1 MB ring is mmap-backed, so the freed pages unmap -> segfault on the next process()).
// Pool the arenas for the whole run so a context's memory never dies under an engine still in use.
std::vector<std::unique_ptr<host::HostArena>> g_arenas;
spotykach::EngineContext pooled_ctx(host::TimeSource& time) {
    g_arenas.push_back(std::make_unique<host::HostArena>());
    return host::make_context(*g_arenas.back(), time);
}

struct Stereo { std::vector<float> l, r; };

// Minimal stream-deck stub for the SD-file source tests: a fixed 3-clip bank, a known 330 Hz sine on
// play_consume (distinct from the 220 Hz live drive), a frame counter to prove the engine pulled from the
// stream (not the live input), and the last opened path per deck (to prove Aux re-selects clips).
struct FakeStream : IStreamDeck {
    uint64_t consumed = 0;
    int      opens    = 0;
    double   phase    = 0.0;
    char     last_path[2][40] = {};
    uint32_t last_frame[2] = { 0, 0 };   // start_frame of the most recent open (for the scrub seek test)
    bool     scan_wav  = false;     // scan_bank reports .wav clips (is_wav=true, rate=scan_rate)
    uint32_t scan_rate = 0;
    int      clip_count = 3;        // how many clips scan_bank reports (0 = card not ready / empty folder)
    mutable int scan_calls = 0;     // number of scan_bank() calls (to prove a re-scan happened)
    static const char* clip_name(int k) {
        static const char* names[3] = { "one.raw", "two.raw", "three.raw" };
        return names[k % 3];
    }
    uint32_t play_consume(DeckRef::Ref, uint8_t* dst, uint32_t n) override {
        const uint32_t cnt = n / sizeof(int16_t);
        int16_t* s = reinterpret_cast<int16_t*>(dst);
        const double inc = 2.0 * M_PI * 330.0 / host::kSampleRate;
        for (uint32_t k = 0; k < cnt; k++) { s[k] = (int16_t)(12000.0 * std::sin(phase)); phase += inc; }
        consumed += cnt;
        return n;
    }
    bool     playing[2] = { false, false };   // reflects start_play_*/stop, so prepare() reconciliation works
    uint32_t record_produce(DeckRef::Ref, const uint8_t*, uint32_t n) override { return n; }
    bool is_playing(DeckRef::Ref d) const override { return playing[(d == DeckRef::A) ? 0 : 1]; }
    bool is_recording(DeckRef::Ref) const override { return false; }
    bool start_play(DeckRef::Ref, const char*)   override { return true; }
    bool start_record(DeckRef::Ref, const char*) override { return true; }
    void stop(DeckRef::Ref d) override { playing[(d == DeckRef::A) ? 0 : 1] = false; }
    void set_loop(DeckRef::Ref, bool) override {}
    uint32_t loop_frames(DeckRef::Ref) const override { return 48000; }
    bool exists(const char*) const override { return true; }
    bool start_play_raw(DeckRef::Ref d, const char* path, uint32_t start_frame, bool) override {
        const int i = (d == DeckRef::A) ? 0 : 1;
        std::strncpy(last_path[i], path, 39); last_frame[i] = start_frame; opens++; playing[i] = true; return true;
    }
    bool start_play_wav(DeckRef::Ref d, const char* path, uint32_t start_frame, bool) override {
        const int i = (d == DeckRef::A) ? 0 : 1;
        std::strncpy(last_path[i], path, 39); last_frame[i] = start_frame; opens++; playing[i] = true; return true;
    }
    int scan_bank(const char*, BankEntry* out, int max) const override {
        ++scan_calls;
        int n = clip_count < 3 ? clip_count : 3;
        if (n > max) n = max;
        for (int k = 0; k < n; k++) {
            BankEntry e{}; std::strncpy(e.name, clip_name(k), 12);
            e.frames = 48000; e.is_wav = scan_wav; e.rate = scan_wav ? scan_rate : 0; out[k] = e;
        }
        return n;
    }
};

// Minimal transport stub for the clock-sync test: a fixed BPM the synced LFO locks its rate to.
struct FakeTransport : ITransport {
    float bpm = 120.f;
    float tempo() const override { return bpm; }
    ClockSource::Source source() const override { return ClockSource::Source(0); }
    bool is_external_sync() const override { return false; }
    uint8_t key_interval() const override { return 0; }
    bool is_key_sub_quarter() const override { return false; }
    void set_on_tick(std::function<void(const TransportTick&)>) override {}
};

Stereo run(PstretchEngine& e, int blocks, const float* drive, bool& finite) {
    float il[host::kBlock], ir[host::kBlock], ol[host::kBlock], orr[host::kBlock];
    const float* in[2] = { il, ir };
    float* out[2] = { ol, orr };
    Stereo s;
    int d = 0;
    for (int b = 0; b < blocks; b++) {
        for (size_t i = 0; i < host::kBlock; i++) { il[i] = ir[i] = drive[d++ % 4096]; }
        e.process(in, out, host::kBlock);
        for (size_t i = 0; i < host::kBlock; i++) {
            if (!std::isfinite(ol[i]) || !std::isfinite(orr[i])) finite = false;
            s.l.push_back(ol[i]); s.r.push_back(orr[i]);
        }
    }
    return s;
}
float peak(const std::vector<float>& v) { float p = 0.f; for (float x : v) p = std::fmax(p, std::fabs(x)); return p; }
float energy(const std::vector<float>& v) { float s = 0.f; for (float x : v) s += std::fabs(x); return s; }
float sad(const std::vector<float>& a, const std::vector<float>& b) {
    float s = 0.f; size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) s += std::fabs(a[i] - b[i]);
    return s;
}

} // namespace

int main() {
    // --- A. Vendored FFT ---------------------------------------------------------------------------
    {
        std::vector<uint8_t> mem(1 << 16, 0);
        Arena ar(EngineArena{ mem.data(), mem.size() });
        pstretch::FFT fft;
        const int N = 64;
        float* cosb = ar.alloc<float>(N / 2); float* sinb = ar.alloc<float>(N / 2);
        uint16_t* brevb = ar.alloc<uint16_t>(N);
        check(fft.init(N, cosb, sinb, brevb), "fft: init power-of-two");
        check(!pstretch::FFT{}.init(48, cosb, sinb, brevb), "fft: rejects non-power-of-two");

        // Round-trip: forward then inverse reconstructs the input.
        float re[64], im[64], re0[64];
        for (int i = 0; i < N; i++) { re[i] = re0[i] = std::sin(0.3f * i) + 0.5f * std::cos(0.11f * i); im[i] = 0.f; }
        fft.transform(re, im, false);
        fft.transform(re, im, true);
        float err = 0.f; for (int i = 0; i < N; i++) err += std::fabs(re[i] - re0[i]);
        check(err < 1e-3f, "fft: inverse(forward(x)) == x");

        // A pure cosine at bin 5 -> energy concentrated at bins 5 and N-5.
        for (int i = 0; i < N; i++) { re[i] = std::cos(6.28318530718f * 5.f * i / N); im[i] = 0.f; }
        fft.transform(re, im, false);
        auto mag = [&](int k){ return std::sqrt(re[k]*re[k] + im[k]*im[k]); };
        float other = 0.f; for (int k = 0; k < N; k++) if (k != 5 && k != N-5) other = std::fmax(other, mag(k));
        check(mag(5) > 10.f * (other + 1e-6f), "fft: cosine concentrates at its bin");
    }

    // --- B. PstretchEngine through IEngine -------------------------------------------------------------
    host::TimeSource time;
    auto make = [&](PstretchEngine& e) { EngineContext c = pooled_ctx(time); e.init(c); };

    // A 220 Hz sine drive table.
    float drive[4096];
    for (int i = 0; i < 4096; i++) drive[i] = 0.5f * std::sin(6.28318530718f * 220.f * i / host::kSampleRate);

    // B1. Produces a finite, bounded, non-silent wet smear (run long enough to clear the startup latency).
    {
        PstretchEngine e; make(e);
        e.set_config(ConfigId::Route, DeckRef::A, 1);          // DoubleMono: A->L, B->R
        e.set_param(ParamId::Size, DeckRef::A, 0.4f);          // moderate stretch
        e.set_param(ParamId::Mix,  DeckRef::A, 1.f);           // full wet
        e.set_param(ParamId::Mix,  DeckRef::B, 1.f);
        bool fin = true;
        const Stereo s = run(e, 400, drive, fin);              // ~38k samples
        check(fin, "engine: output finite");
        check(peak(s.l) <= 1.05f, "engine: output bounded");
        check(energy(s.l) > 0.f, "engine: produces a wet smear");
    }

    // B2. Dry passthrough at MIX=0 (DoubleMono isolates deck A on the left; ENV open by default).
    {
        PstretchEngine e; make(e);
        e.set_config(ConfigId::Route, DeckRef::A, 1);
        e.set_param(ParamId::Mix, DeckRef::A, 0.f);            // fully dry
        e.set_param(ParamId::Env, DeckRef::A, 1.f);            // tone fully open (LP passes through)
        bool fin = true; const Stereo s = run(e, 60, drive, fin);
        // Compare the left channel to the drive after the one-pole settles (skip the first samples).
        float err = 0.f; int n = 0;
        for (size_t i = 200; i < s.l.size(); i++) { err += std::fabs(s.l[i] - drive[i % 4096]); n++; }
        check(fin && n > 0 && err / n < 0.02f, "engine: MIX=0 is a clean dry passthrough");
    }

    // B3. Stretch amount changes the output.
    {
        auto render = [&](float sizeN) {
            PstretchEngine e; make(e);
            e.set_config(ConfigId::Route, DeckRef::A, 1);
            e.set_param(ParamId::Mix, DeckRef::A, 1.f);
            e.set_param(ParamId::Size, DeckRef::A, sizeN);
            bool fin = true; return run(e, 400, drive, fin).l;
        };
        check(sad(render(0.2f), render(0.9f)) > 1.f, "engine: stretch amount changes the smear");
    }

    // B4. Freeze (Play pad) keeps producing a drone and is reflected in readback path.
    {
        PstretchEngine e; make(e);
        e.set_config(ConfigId::Route, DeckRef::A, 1);
        e.set_param(ParamId::Mix, DeckRef::A, 1.f);
        e.set_param(ParamId::Size, DeckRef::A, 0.6f);
        bool fin = true; run(e, 300, drive, fin);              // prime the buffer
        e.on_play_pad(DeckRef::A, /*reverse=*/false);          // freeze ON
        const Stereo frozen = run(e, 300, drive, fin);
        check(fin, "engine: frozen output finite");
        check(energy(frozen.l) > 0.f, "engine: freeze holds an evolving drone (non-silent)");
        e.on_play_pad(DeckRef::A, false);                      // toggles back off (idempotent path)
    }

    // B5. Param readback + routing.
    {
        PstretchEngine e; make(e);
        e.set_param(ParamId::Size, DeckRef::A, 0.7f);
        e.set_param(ParamId::Pos,  DeckRef::A, 0.3f);
        e.set_param(ParamId::Speed, DeckRef::A, 0.8f);
        check(std::fabs(e.param(ParamId::Size, DeckRef::A) - 0.7f) < 1e-4f, "readback: SIZE");
        check(std::fabs(e.param(ParamId::Pos, DeckRef::A) - 0.3f) < 1e-4f, "readback: POS");
        check(std::fabs(e.param(ParamId::Speed, DeckRef::A) - 0.8f) < 1e-4f, "readback: PITCH");
        e.set_config(ConfigId::Route, DeckRef::A, 2);
        check(e.route() == Route::GenerativeStereo, "route: switch position 2 -> GenerativeStereo");
    }

    // B7. Capture/hold (Mode switch position 1): grab the recent ring and loop the stretch through it - keeps
    // producing a drone indefinitely (the loop must not run dry). The Rev pad re-grabs while in Capture.
    {
        PstretchEngine e; make(e);
        e.set_config(ConfigId::Route, DeckRef::A, 1);
        e.set_param(ParamId::Mix, DeckRef::A, 1.f);
        e.set_param(ParamId::Size, DeckRef::A, 0.6f);
        bool fin = true; run(e, 300, drive, fin);          // fill the ring with input
        e.set_config(ConfigId::Mode, DeckRef::A, 1);       // source = Capture (grabs on entry)
        const Stereo cap = run(e, 700, drive, fin);        // long hold -> the loop must keep going
        check(fin, "capture: output finite");
        check(energy(cap.l) > 0.f, "capture: loops the grabbed span (non-silent, plays through)");
        std::vector<float> tail(cap.l.end() - 4000, cap.l.end());
        check(energy(tail) > 0.f, "capture: still producing after a long hold (loops, no underrun)");
        e.on_play_pad(DeckRef::A, /*reverse=*/true);       // re-grab (Rev pad) - still producing
        const Stereo regrab = run(e, 300, drive, fin);
        check(fin && energy(regrab.l) > 0.f, "capture: Rev-pad re-grab keeps the loop going");
        e.set_config(ConfigId::Mode, DeckRef::A, 0);       // back to live
    }

    // B8. SD-file source (Phase 2): Mode-switch position 2 streams a clip from the (fake) card through the
    // stretch. The ring is fed from the stream, not the live input - assert the engine actually consumed
    // from the stream, opened the clip, and produced a finite, bounded, non-silent wash; then that exiting
    // SD mode (Mode 0) returns to a working live source.
    {
        FakeStream fake;
        PstretchEngine e;
        { EngineContext c = pooled_ctx(time); c.stream = &fake; e.init(c); }
        e.set_config(ConfigId::Route, DeckRef::A, 1);          // DoubleMono: A -> L
        e.set_param(ParamId::Mix, DeckRef::A, 1.f);            // full wet
        e.set_param(ParamId::Size, DeckRef::A, 0.5f);          // moderate stretch (slow read head)
        e.set_config(ConfigId::Mode, DeckRef::A, 2);           // source = SD
        bool fin = true;
        const Stereo sd = run(e, 600, drive, fin);
        check(fin, "sd: output finite");
        check(peak(sd.l) <= 1.05f, "sd: output bounded");
        check(fake.opens > 0, "sd: opened a clip from the card on entering SD mode");
        check(fake.consumed > 0, "sd: fed the ring from the stream (not the live input)");
        check(energy(sd.l) > 0.f, "sd: streams the clip through the stretch (non-silent)");
        // Read head is slow (stretch ~8x): consumption stays well below real time (the trivial-bandwidth
        // property that makes streaming long clips feasible). 600 blocks * 96 = 57600 input frames.
        check(fake.consumed < 57600u, "sd: consumes source below real time (slow read head)");
        e.set_config(ConfigId::Mode, DeckRef::A, 0);           // back to live
        const Stereo live = run(e, 300, drive, fin);
        check(fin && energy(live.l) > 0.f, "sd: returns to a working live source on exit");
    }

    // B9. Aux (Alt+PITCH) clip selector: pick which clip in /pstretch each deck streams. prepare() scans the
    // 3-clip fake bank; entering SD opens the selected clip; moving Aux re-opens a different clip live; and a
    // selection made BEFORE entering SD is honoured on entry.
    {
        FakeStream fake;
        PstretchEngine e;
        { EngineContext c = pooled_ctx(time); c.stream = &fake; e.init(c); }
        e.prepare();                                           // scan /pstretch -> one/two/three.raw
        e.set_param(ParamId::Mix, DeckRef::A, 1.f);
        e.set_config(ConfigId::Mode, DeckRef::A, 2);           // deck A -> SD: opens clip 0 (one.raw)
        check(std::strstr(fake.last_path[0], "one.raw") != nullptr, "aux: SD opens the first clip by default");
        e.set_param(ParamId::Aux, DeckRef::A, 0.9f);           // idx = floor(0.9*3) = 2 -> three.raw
        check(std::strstr(fake.last_path[0], "three.raw") != nullptr, "aux: moving Aux re-opens the clip live");
        check(std::fabs(e.param(ParamId::Aux, DeckRef::A) - (2.f + 0.5f) / 3.f) < 1e-4f, "aux: readback slot");
        bool fin = true; const Stereo s = run(e, 300, drive, fin);
        check(fin && energy(s.l) > 0.f, "aux: still streaming after a clip change");
        // Deck B: select clip 1 (two.raw) BEFORE switching to SD - the choice is honoured on entry.
        e.set_param(ParamId::Aux, DeckRef::B, 0.5f);           // idx = floor(0.5*3) = 1 -> two.raw
        e.set_config(ConfigId::Mode, DeckRef::B, 2);
        check(std::strstr(fake.last_path[1], "two.raw") != nullptr, "aux: pre-SD selection honoured on entry");
    }

    // B10. Off-rate clips play at native pitch. The fake SD source is a 330 Hz tone (advanced per consumed
    // frame, independent of the declared rate), so with diffusion OFF (clean resynthesis) and PITCH unity the
    // wet output is that tone scaled by clip_rate/48000. Measure the dominant frequency by zero-crossing rate:
    // a 48 k clip reproduces ~330 Hz, and a 44.1 k clip is lower by exactly 44100/48000 (the rate correction).
    {
        auto sd_tone_hz = [&](uint32_t clip_rate, float& peak_out) {
            FakeStream fake; fake.scan_wav = true; fake.scan_rate = clip_rate;
            PstretchEngine e;
            { EngineContext c = pooled_ctx(time); c.stream = &fake; e.init(c); }
            e.prepare();
            e.set_config(ConfigId::Route, DeckRef::A, 1);     // A -> L
            e.set_param(ParamId::Mix,  DeckRef::A, 1.f);      // fully wet
            e.set_param(ParamId::Pos,  DeckRef::A, 0.f);      // diffusion OFF -> clean tone
            e.set_param(ParamId::Speed, DeckRef::A, 0.5f);    // PITCH unity
            e.set_param(ParamId::Size, DeckRef::A, 0.3f);     // modest stretch
            e.set_config(ConfigId::Mode, DeckRef::A, 2);      // source = SD (wav at clip_rate)
            bool fin = true; const Stereo s = run(e, 900, drive, fin);
            const size_t start = s.l.size() > 24000 ? s.l.size() - 24000 : 0;   // steady-state tail
            int zc = 0; float pk = 0.f;
            for (size_t i = start + 1; i < s.l.size(); i++) {
                pk = std::fmax(pk, std::fabs(s.l[i]));
                if ((s.l[i - 1] <= 0.f) != (s.l[i] <= 0.f)) zc++;
            }
            peak_out = pk;
            const float dur = static_cast<float>(s.l.size() - start - 1) / host::kSampleRate;
            return 0.5f * static_cast<float>(zc) / dur;   // Hz
        };
        float pk48 = 0.f, pk441 = 0.f;
        const float f48  = sd_tone_hz(48000, pk48);
        const float f441 = sd_tone_hz(44100, pk441);
        check(pk48 > 0.05f && pk441 > 0.05f, "rate: SD tone present (non-trivial amplitude)");
        check(f48 > 280.f && f48 < 380.f, "rate: 48k clip reproduces the source tone near native pitch (~330 Hz)");
        check(std::fabs((f441 / f48) - (44100.f / 48000.f)) < 0.03f,
              "rate: 44.1k clip is pitch-corrected by clip_rate/48000 (not played sharp)");
    }

    // B11. Alt+POS scrub (SD only): re-seeks the stream playhead to a position in the clip, debounced. With a
    // null time source the settle is immediate (host). Assert: SD opens at frame 0; a scrub to 0.5 re-opens
    // near the clip midpoint after prepare(); a sub-step nudge does NOT re-seek; and scrub is a no-op in Live.
    {
        FakeStream fake;
        PstretchEngine e;
        { EngineContext c = pooled_ctx(time);
          c.stream = &fake; c.time = nullptr; e.init(c); }                 // null time -> settle immediately
        e.prepare();
        e.set_config(ConfigId::Mode, DeckRef::A, 2);          // SD: opens clip at frame 0
        check(fake.last_frame[0] == 0u, "scrub: SD opens the clip at frame 0");
        const int opens_before = fake.opens;
        e.set_param(ParamId::AltPos, DeckRef::A, 0.5f);       // scrub to the middle of the 48000-frame clip
        e.prepare();                                          // settle -> re-seek
        check(fake.last_frame[0] == 24000u, "scrub: Alt+POS re-seeks the playhead to the clip position");
        check(std::fabs(e.param(ParamId::AltPos, DeckRef::A) - 0.5f) < 1e-4f, "scrub: Alt+POS readback");
        const int opens_after_seek = fake.opens;
        e.set_param(ParamId::AltPos, DeckRef::A, 0.505f);     // < kScrubStep (1%) from 0.5 -> ignored
        e.prepare();
        check(fake.opens == opens_after_seek, "scrub: a sub-step nudge does not re-seek (no FatFs thrash)");
        check(opens_after_seek > opens_before, "scrub: the settled move did re-open exactly once");
        // Live deck: Alt+POS stores the value but never seeks (no clip to scrub).
        const int opens_live = fake.opens;
        e.set_param(ParamId::AltPos, DeckRef::B, 0.7f);
        e.prepare();
        check(fake.opens == opens_live, "scrub: Alt+POS is a no-op in Live mode");
    }

    // B12. Late card mount: a scan that runs before the card is ready returns 0 clips; the engine must NOT
    // give up - selecting SD (and the boot-window retry) must re-scan and pick up clips that appear later.
    // Regression guard for the one-shot-scan bug (no playback / no Aux dots when the card mounts slowly).
    {
        FakeStream fake; fake.clip_count = 0;                  // card "not ready yet" -> empty scan
        PstretchEngine e;
        { EngineContext c = pooled_ctx(time);
          c.stream = &fake; c.time = nullptr; e.init(c); }     // null time -> first scan settles immediately
        e.prepare();                                           // boot scan finds nothing
        check(e.param(ParamId::Aux, DeckRef::A) == 0.f, "late-mount: no clips found on the early scan");
        const int calls_before = fake.scan_calls;
        fake.clip_count = 2;                                   // ...the card finishes mounting
        e.set_param(ParamId::Mix, DeckRef::A, 1.f);
        e.set_config(ConfigId::Mode, DeckRef::A, 2);           // select SD -> must re-scan and open a clip
        check(fake.scan_calls > calls_before, "late-mount: entering SD re-scans the card");
        check(fake.opens > 0, "late-mount: a clip opens once the card is ready (playback recovers)");
        bool fin = true; const Stereo s = run(e, 400, drive, fin);
        check(fin && energy(s.l) > 0.f, "late-mount: streams after the late scan (no permanent silence)");
    }

    // B13. Drift selected at power-on with the card not yet mounted: the open attempt finds 0 clips and
    // opens nothing, but a later prepare() (once the card is ready) must re-scan AND open a clip on its own.
    // Regression guard for "Drift gives silence forever if the card mounts after the switch is already set".
    {
        FakeStream fake; fake.clip_count = 0;                  // card not ready at the moment SD is selected
        PstretchEngine e;
        { EngineContext c = pooled_ctx(time);
          c.stream = &fake; c.time = nullptr; e.init(c); }
        e.set_param(ParamId::Mix, DeckRef::A, 1.f);
        e.set_config(ConfigId::Mode, DeckRef::A, 2);           // Drift/SD at "boot": nothing to open yet
        check(fake.opens == 0 && !fake.is_playing(DeckRef::A), "boot-Drift: nothing opens while card not ready");
        fake.clip_count = 2;                                   // ...card finishes mounting
        e.prepare();                                           // reconciliation must re-scan + open unprompted
        check(fake.is_playing(DeckRef::A), "boot-Drift: prepare() opens a clip once the card mounts");
        bool fin = true; const Stereo s = run(e, 400, drive, fin);
        check(fin && energy(s.l) > 0.f, "boot-Drift: streams after the card mounts (self-heals)");
        e.prepare(); e.prepare();
        check(fake.opens == 1, "boot-Drift: a streaming deck is not re-opened every prepare()");
    }

    // B14. Mod LFO (Cycle/Glow): raising the depth modulates the selected target and changes the smear vs no
    // mod; the target is chosen by the Size/Pos mod switch (Pos->diffusion, Size->stretch, both->pitch); all
    // targets and the follower stay finite/bounded/non-silent; Cycle/Glow read back for knob pickup.
    {
        auto base = [&](PstretchEngine& e) {
            e.set_config(ConfigId::Route, DeckRef::A, 1);
            e.set_param(ParamId::Size, DeckRef::A, 0.5f);
            e.set_param(ParamId::Mix,  DeckRef::A, 1.f);
        };
        PstretchEngine e0; make(e0); base(e0);                       // mod off (depth 0)
        bool f0 = true; const Stereo s0 = run(e0, 400, drive, f0);

        PstretchEngine e1; make(e1); base(e1);                       // LFO on diffusion, full depth
        e1.set_config(ConfigId::StartModOn, DeckRef::A, 1);
        e1.set_config(ConfigId::SizeModOn,  DeckRef::A, 0);
        e1.set_mod_speed(DeckRef::A, 0.8f, false);
        e1.set_param(ParamId::ModAmp, DeckRef::A, 1.f);
        bool f1 = true; const Stereo s1 = run(e1, 400, drive, f1);
        check(f0 && f1, "mod: output finite");
        check(peak(s1.l) <= 1.05f, "mod: modulated output bounded");
        check(sad(s0.l, s1.l) > 1.f, "mod: LFO on diffusion changes the smear vs no mod");

        PstretchEngine e2; make(e2); base(e2);                       // stretch target (Size-only)
        e2.set_config(ConfigId::StartModOn, DeckRef::A, 0);
        e2.set_config(ConfigId::SizeModOn,  DeckRef::A, 1);
        e2.set_mod_speed(DeckRef::A, 0.6f, false);
        e2.set_param(ParamId::ModAmp, DeckRef::A, 1.f);
        bool f2 = true; const Stereo s2 = run(e2, 300, drive, f2);
        check(f2 && peak(s2.l) <= 1.05f && energy(s2.l) > 0.f, "mod: stretch target finite/bounded/non-silent");

        PstretchEngine e3; make(e3); base(e3);                       // tone target (both on)
        e3.set_config(ConfigId::StartModOn, DeckRef::A, 1);
        e3.set_config(ConfigId::SizeModOn,  DeckRef::A, 1);
        e3.set_mod_speed(DeckRef::A, 0.5f, false);
        e3.set_param(ParamId::ModAmp, DeckRef::A, 1.f);
        bool f3 = true; const Stereo s3 = run(e3, 300, drive, f3);
        check(f3 && peak(s3.l) <= 1.05f && energy(s3.l) > 0.f, "mod: tone target finite/bounded/non-silent");

        PstretchEngine e4; make(e4); base(e4);                       // input-envelope follower
        e4.set_config(ConfigId::ModType, DeckRef::A, 1);
        e4.set_config(ConfigId::StartModOn, DeckRef::A, 1);
        e4.set_config(ConfigId::SizeModOn,  DeckRef::A, 0);
        e4.set_param(ParamId::ModAmp, DeckRef::A, 1.f);
        bool f4 = true; const Stereo s4 = run(e4, 300, drive, f4);
        check(f4 && peak(s4.l) <= 1.05f && energy(s4.l) > 0.f, "mod: input follower finite/bounded/non-silent");

        e1.set_mod_speed(DeckRef::A, 0.42f, false);
        check(std::fabs(e1.param(ParamId::ModSpeed, DeckRef::A) - 0.42f) < 1e-4f, "mod: Cycle (ModSpeed) readback");
        check(std::fabs(e1.param(ParamId::ModAmp, DeckRef::A) - 1.f) < 1e-4f, "mod: Glow (ModAmp) readback");
    }

    // B15. CV inputs: unpatched (0) CV is an exact no-op (the knob alone rules); cv_mix cancels the wet to a
    // dry passthrough; V/Oct and Size/Pos CV change the output (pitch / stretch modulation).
    {
        auto base = [&](PstretchEngine& e) {
            e.set_config(ConfigId::Route, DeckRef::A, 1);
            e.set_param(ParamId::Size, DeckRef::A, 0.5f);
            e.set_param(ParamId::Mix,  DeckRef::A, 1.f);
        };
        PstretchEngine e0; make(e0); base(e0);
        bool f0 = true; const Stereo s0 = run(e0, 300, drive, f0);
        PstretchEngine ez; make(ez); base(ez);
        ez.cv_voct(DeckRef::A, 0.f); ez.cv_size_pos(DeckRef::A, 0.f); ez.cv_mix(DeckRef::A, 0.f);
        bool fz = true; const Stereo sz = run(ez, 300, drive, fz);
        check(f0 && fz && sad(s0.l, sz.l) < 1e-3f, "cv: zero CV is an exact no-op (knob alone rules)");

        // cv_mix = -1 with MIX=1 -> effective mix 0 -> clean dry passthrough (ENV open, DoubleMono isolates A).
        PstretchEngine em; make(em); base(em);
        em.set_param(ParamId::Env, DeckRef::A, 1.f);
        em.cv_mix(DeckRef::A, -1.f);
        bool fm = true; const Stereo sm = run(em, 200, drive, fm);
        float err = 0.f; int nn = 0;
        for (size_t i = 200; i < sm.l.size(); i++) { err += std::fabs(sm.l[i] - drive[i % 4096]); nn++; }
        check(fm && nn > 0 && err / nn < 0.02f, "cv: cv_mix=-1 cancels the wet to a dry passthrough");

        PstretchEngine ev; make(ev); base(ev);                       // V/Oct +12 semitones
        ev.cv_voct(DeckRef::A, 12.f);
        bool fv = true; const Stereo sv = run(ev, 300, drive, fv);
        check(fv && peak(sv.l) <= 1.05f, "cv: pitched output finite/bounded");
        check(sad(s0.l, sv.l) > 1.f, "cv: V/Oct CV shifts the pitch (changes the output)");

        PstretchEngine es; make(es); base(es);                       // Size/Pos CV -> stretch
        es.cv_size_pos(DeckRef::A, 0.4f);
        bool fs = true; const Stereo ss = run(es, 300, drive, fs);
        check(fs && sad(s0.l, ss.l) > 1.f, "cv: Size/Pos CV changes the stretch (changes the output)");
    }

    // B16. Gate in: in Live it toggles FREEZE (so the evolving output diverges from an un-gated deck fed the
    // same input); in Capture it RE-GRABS the ring (keeps looping, no underrun). Both decks are primed and
    // then stepped identically (run() restarts the drive each call), so the only difference is the gate.
    {
        auto base = [&](PstretchEngine& e) {
            e.set_config(ConfigId::Route, DeckRef::A, 1);
            e.set_param(ParamId::Size, DeckRef::A, 0.6f);
            e.set_param(ParamId::Mix,  DeckRef::A, 1.f);
        };
        PstretchEngine eb; make(eb); base(eb);   // ungated reference
        PstretchEngine eg; make(eg); base(eg);   // gated
        bool fb = true, fg = true;
        run(eb, 100, drive, fb); run(eg, 100, drive, fg);            // identical priming
        eg.on_gate_trigger(DeckRef::A);                              // Live: freeze on
        const Stereo b2 = run(eb, 200, drive, fb);
        const Stereo g2 = run(eg, 200, drive, fg);
        check(fb && fg, "gate: output finite");
        check(peak(g2.l) <= 1.05f && energy(g2.l) > 0.f, "gate: frozen drone bounded + non-silent");
        check(sad(b2.l, g2.l) > 1.f, "gate: Live gate toggles freeze (diverges from the un-gated deck)");
        eg.on_gate_trigger(DeckRef::A);                              // freeze off
        bool fu = true; const Stereo g3 = run(eg, 100, drive, fu);
        check(fu && energy(g3.l) > 0.f, "gate: a second gate unfreezes (still producing)");

        PstretchEngine ec; make(ec); base(ec);
        bool fc = true; run(ec, 300, drive, fc);                     // fill the ring with live input first
        ec.set_config(ConfigId::Mode, DeckRef::A, 1);                // Capture (grabs the primed ring)
        run(ec, 100, drive, fc);
        ec.on_gate_trigger(DeckRef::A);                              // re-grab
        const Stereo c2 = run(ec, 200, drive, fc);
        check(fc && energy(c2.l) > 0.f, "gate: Capture re-grab keeps the loop going (non-silent)");
    }

    // B17. Clock-synced LFO (Alt+Cycle): with a transport the LFO rate locks to a musical division of tempo,
    // recomputed per block — so the same knob at two tempos modulates at different rates (different output);
    // deck_leds reports the sync state (Cycle LED), and a plain Cycle move (sync=false) unsyncs.
    {
        auto base = [](PstretchEngine& e) {
            e.set_config(ConfigId::Route, DeckRef::A, 1);
            e.set_param(ParamId::Size, DeckRef::A, 0.5f);
            e.set_param(ParamId::Mix,  DeckRef::A, 1.f);
            e.set_config(ConfigId::StartModOn, DeckRef::A, 1);   // target = diffusion
            e.set_config(ConfigId::SizeModOn,  DeckRef::A, 0);
            e.set_param(ParamId::ModAmp, DeckRef::A, 1.f);       // full depth
        };
        host::HostArena a1; host::TimeSource t1; EngineContext c1 = host::make_context(a1, t1);
        FakeTransport tr1; tr1.bpm = 90.f;  c1.transport = &tr1;
        PstretchEngine e1; e1.init(c1); base(e1);
        e1.set_mod_speed(DeckRef::A, 0.6f, /*sync=*/true);
        bool f1 = true; const Stereo s1 = run(e1, 400, drive, f1);

        host::HostArena a2; host::TimeSource t2; EngineContext c2 = host::make_context(a2, t2);
        FakeTransport tr2; tr2.bpm = 240.f; c2.transport = &tr2;
        PstretchEngine e2; e2.init(c2); base(e2);
        e2.set_mod_speed(DeckRef::A, 0.6f, /*sync=*/true);
        bool f2 = true; const Stereo s2 = run(e2, 400, drive, f2);

        check(f1 && f2, "sync: output finite");
        check(peak(s1.l) <= 1.05f && energy(s1.l) > 0.f, "sync: synced LFO bounded + non-silent");
        check(sad(s1.l, s2.l) > 1.f, "sync: LFO rate tracks tempo (90 vs 240 BPM differ)");
        check(e1.deck_leds(DeckRef::A).mod_synced, "sync: deck_leds reports synced (Cycle LED)");
        e1.set_mod_speed(DeckRef::A, 0.6f, /*sync=*/false);
        check(!e1.deck_leds(DeckRef::A).mod_synced, "sync: a plain Cycle move unsyncs");
    }

    // B18. CV / gate OUT + crossfade CV. (a) Crossfade CV shifts the A/B balance - at full+ it mutes deck A,
    // so the left channel goes silent in DoubleMono. (b) Mod CV out emits the LFO as a moving 0..1 CV. (c)
    // Gate out fires ~once per LFO cycle (a tempo-synced clock when the LFO is synced), not stuck high.
    {
        PstretchEngine ex; make(ex);
        ex.set_config(ConfigId::Route, DeckRef::A, 1); ex.set_param(ParamId::Mix, DeckRef::A, 1.f);
        ex.cv_crossfade(1.f);                                  // blend fully to B -> A (left) muted
        bool fx = true; const Stereo xm = run(ex, 200, drive, fx);
        PstretchEngine ex2; make(ex2);
        ex2.set_config(ConfigId::Route, DeckRef::A, 1); ex2.set_param(ParamId::Mix, DeckRef::A, 1.f);
        ex2.cv_crossfade(-1.f);                                // blend fully to A -> A (left) full
        bool fx2 = true; const Stereo xf = run(ex2, 200, drive, fx2);
        check(fx && fx2, "cvout: crossfade-CV output finite");
        check(energy(xm.l) < 0.01f * energy(xf.l) + 1e-3f, "cvout: crossfade CV mutes A on the left at full+");

        PstretchEngine e; make(e);
        e.set_config(ConfigId::Route, DeckRef::A, 1);
        e.set_param(ParamId::Size, DeckRef::A, 0.5f);
        e.set_param(ParamId::Mix,  DeckRef::A, 1.f);
        e.set_mod_speed(DeckRef::A, 1.f, false);               // fast free LFO (~7.7 Hz)
        e.set_param(ParamId::ModAmp, DeckRef::A, 1.f);
        float il[host::kBlock], ir[host::kBlock], ol[host::kBlock], orr[host::kBlock];
        const float* in[2] = { il, ir }; float* out[2] = { ol, orr };
        float cv0[host::kBlock], cv1[host::kBlock];
        float cvmin = 2.f, cvmax = -1.f; int gates = 0; bool cvfin = true; int d = 0;
        for (int b = 0; b < 400; b++) {
            for (size_t i = 0; i < host::kBlock; i++) il[i] = ir[i] = drive[d++ % 4096];
            e.process(in, out, host::kBlock);
            e.process_cv(cv0, cv1, host::kBlock);
            for (size_t i = 0; i < host::kBlock; i++) {
                if (!std::isfinite(cv0[i])) cvfin = false;
                cvmin = std::fmin(cvmin, cv0[i]); cvmax = std::fmax(cvmax, cv0[i]);
            }
            if (e.gate_out_triggered(DeckRef::A)) gates++;
        }
        check(cvfin, "cvout: Mod CV out finite");
        check(cvmin >= 0.f && cvmax <= 1.f, "cvout: Mod CV out in [0,1]");
        check(cvmax - cvmin > 0.5f, "cvout: Mod CV out swings with the LFO");
        check(gates > 0 && gates < 200, "gateout: one pulse per LFO cycle (fires, not stuck high)");
    }

    if (g_failures == 0) { std::printf("OK: all pstretch checks passed\n"); return 0; }
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
}
