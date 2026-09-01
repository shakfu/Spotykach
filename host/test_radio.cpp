// Headless test for the radio engine (dual virtual RadioMusic). Two layers:
//   A. RawStreamReader over a memory file: read/eof/rewind/seek_to_frame/frame-length, the headerless
//      16-bit-mono codec the radio streams.
//   B. The RadioEngine through its public IEngine surface, driven by a fake IStreamDeck that records the
//      start_play_raw(path, start_frame, loop) calls and serves a sine. Covers station/bank quantization,
//      the free-running-playhead offset (clock + start) mod L, varispeed, the inter-station static, and
//      the RESET (Play pad / gate) re-tune.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "engine/radio/radio_engine.h"
#include "engine/istreamdeck.h"
#include "memory/byte_file.h"
#include "memory/raw_stream.h"
#include "memory/converting_source.h"
#include "host_setup.h"

using namespace spotykach;

namespace {

int g_failures = 0;
void check(bool cond, const char* msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg); g_failures++; }
}

// Seekable in-memory file (stands in for a FatFs .raw on the card).
struct MemFile : IByteFile {
    std::vector<uint8_t> buf;
    uint32_t cur = 0;
    uint32_t read(void* dst, uint32_t n) override {
        uint32_t avail = (cur < buf.size()) ? (uint32_t)buf.size() - cur : 0;
        uint32_t k = n < avail ? n : avail;
        std::memcpy(dst, buf.data() + cur, k); cur += k; return k;
    }
    uint32_t write(const void*, uint32_t n) override { return n; }
    bool seek(uint32_t pos) override { cur = pos; return true; }
};

// int16 ramp body: value i = (int16_t)(i*7+1), so any mis-seek/mis-read shows up.
std::vector<uint8_t> ramp_i16(uint32_t frames) {
    std::vector<uint8_t> v(frames * 2);
    for (uint32_t i = 0; i < frames; i++) {
        int16_t s = (int16_t)(i * 7 + 1);
        v[2 * i] = s & 0xff; v[2 * i + 1] = (s >> 8) & 0xff;
    }
    return v;
}

// Build a WAV (RIFF) with the given fmt/channels/bits/rate and a `frames`-frame body. For 16-bit mono it
// writes the int16 ramp (so seek/read can be checked); otherwise a zero body of the right size.
std::vector<uint8_t> wav_make(uint16_t fmt, uint16_t ch, uint16_t bits, uint32_t rate, uint32_t frames) {
    auto le16 = [](std::vector<uint8_t>& v, uint16_t x){ v.push_back(x & 0xff); v.push_back((x >> 8) & 0xff); };
    auto le32 = [](std::vector<uint8_t>& v, uint32_t x){ for (int i = 0; i < 4; i++) v.push_back((x >> (8 * i)) & 0xff); };
    auto tag  = [](std::vector<uint8_t>& v, const char* s){ for (int i = 0; i < 4; i++) v.push_back((uint8_t)s[i]); };
    const uint16_t blockAlign = (uint16_t)(ch * (bits / 8));
    const uint32_t dataBytes  = frames * blockAlign;
    std::vector<uint8_t> v;
    tag(v, "RIFF"); le32(v, 36 + dataBytes); tag(v, "WAVE");
    tag(v, "fmt "); le32(v, 16); le16(v, fmt); le16(v, ch); le32(v, rate);
    le32(v, rate * blockAlign); le16(v, blockAlign); le16(v, bits);
    tag(v, "data"); le32(v, dataBytes);
    if (fmt == 1 && ch == 1 && bits == 16) {
        for (uint32_t i = 0; i < frames; i++) { int16_t s = (int16_t)(i * 7 + 1); v.push_back(s & 0xff); v.push_back((s >> 8) & 0xff); }
    } else {
        v.insert(v.end(), dataBytes, 0);
    }
    return v;
}

// Fake stream deck: a fixed 4-station bank, sine playback, and a record of the last start_play_raw().
struct FakeStream : IStreamDeck {
    uint32_t L[4] = { 10000, 20000, 33333, 48000 };   // station frame-lengths
    bool     playing[2] = { false, false };
    // last open per deck
    char     last_path[2][40] = {};
    uint32_t last_frame[2] = { 0, 0 };
    bool     last_loop[2] = { false, false };
    int      open_calls[2] = { 0, 0 };
    double   phase[2] = { 0.0, 0.0 };

    uint32_t play_consume(DeckRef::Ref deck, uint8_t* dst, uint32_t n) override {
        const int i = (deck == DeckRef::A) ? 0 : 1;
        const uint32_t cnt = n / sizeof(int16_t);
        int16_t* s = reinterpret_cast<int16_t*>(dst);
        const double inc = 2.0 * M_PI * 220.0 / host::kSampleRate;
        for (uint32_t k = 0; k < cnt; k++) { s[k] = (int16_t)(12000.0 * std::sin(phase[i])); phase[i] += inc; }
        return n;
    }
    uint32_t record_produce(DeckRef::Ref, const uint8_t*, uint32_t n) override { return n; }
    bool is_playing(DeckRef::Ref deck)   const override { return playing[(deck == DeckRef::A) ? 0 : 1]; }
    bool is_recording(DeckRef::Ref)      const override { return false; }
    bool start_play(DeckRef::Ref, const char*)   override { return true; }
    bool start_record(DeckRef::Ref, const char*) override { return true; }
    void stop(DeckRef::Ref deck) override { playing[(deck == DeckRef::A) ? 0 : 1] = false; }
    void set_loop(DeckRef::Ref, bool) override {}
    uint32_t loop_frames(DeckRef::Ref) const override { return 0; }
    bool exists(const char*) const override { return true; }

    bool     wav_mode = false;        // scan_bank reports .wav stations (is_wav=true, rate=wav_rate)
    uint32_t wav_rate = 44100;
    bool     last_was_wav[2] = { false, false };

    bool start_play_raw(DeckRef::Ref deck, const char* path, uint32_t start_frame, bool loop) override {
        const int i = (deck == DeckRef::A) ? 0 : 1;
        std::strncpy(last_path[i], path, sizeof(last_path[i]) - 1);
        last_frame[i] = start_frame; last_loop[i] = loop; open_calls[i]++; last_was_wav[i] = false;
        playing[i] = true;
        return true;
    }
    bool start_play_wav(DeckRef::Ref deck, const char* path, uint32_t start_frame, bool loop) override {
        const int i = (deck == DeckRef::A) ? 0 : 1;
        std::strncpy(last_path[i], path, sizeof(last_path[i]) - 1);
        last_frame[i] = start_frame; last_loop[i] = loop; open_calls[i]++; last_was_wav[i] = true;
        playing[i] = true;
        return true;
    }
    uint32_t frames_of(const char* path) const override {
        // path ".../0N.raw" -> station N-1
        const char* slash = std::strrchr(path, '/');
        int n = slash ? std::atoi(slash + 1) : 0;
        return (n >= 1 && n <= 4) ? L[n - 1] : 0;
    }
    char rate_txt[16] = {0};   // contents of /radio/rate.txt ("" = file absent)
    int read_text(const char*, char* buf, int max) const override {
        if (max <= 0) return 0;
        if (rate_txt[0] == 0) { buf[0] = '\0'; return 0; }
        int i = 0;
        for (; rate_txt[i] && i < max - 1; i++) buf[i] = rate_txt[i];
        buf[i] = '\0';
        return i;
    }
    int scan_bank(const char*, BankEntry* out, int max) const override {
        const int n = (max < 4) ? max : 4;
        for (int s = 0; s < n; s++) {
            // names "01.raw".."04.raw" (or .wav in wav_mode)
            out[s].name[0] = '0'; out[s].name[1] = (char)('1' + s);
            std::strcpy(out[s].name + 2, wav_mode ? ".wav" : ".raw");
            out[s].frames = L[s];
            out[s].is_wav = wav_mode;
            out[s].rate   = wav_mode ? wav_rate : 0;
        }
        return n;
    }
};

// Run `blocks` audio blocks on deck A's left output; clears `finite` on any non-finite sample.
std::vector<float> run(RadioEngine& e, int blocks, bool& finite) {
    float il[host::kBlock] = {0}, ir[host::kBlock] = {0}, ol[host::kBlock], orr[host::kBlock];
    const float* in[2] = { il, ir };
    float* out[2] = { ol, orr };
    std::vector<float> v;
    for (int b = 0; b < blocks; b++) {
        e.process(in, out, host::kBlock);
        for (size_t i = 0; i < host::kBlock; i++) {
            if (!std::isfinite(ol[i]) || !std::isfinite(orr[i])) finite = false;
            v.push_back(ol[i]);
        }
    }
    return v;
}
float peak(const std::vector<float>& v) { float p = 0.f; for (float x : v) p = std::fmax(p, std::fabs(x)); return p; }
float sad(const std::vector<float>& a, const std::vector<float>& b) {
    float s = 0.f; size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) s += std::fabs(a[i] - b[i]);
    return s;
}
int station_of(const char* path) {   // ".../0N.raw" -> index N-1
    const char* slash = std::strrchr(path, '/');
    return slash ? std::atoi(slash + 1) - 1 : -1;
}

} // namespace

int main() {
    host::TimeSource time;

    // --- A. RawStreamReader -----------------------------------------------------------------------
    {
        const uint32_t frames = 1000;
        MemFile f; f.buf = ramp_i16(frames);
        RawStreamReader r;
        check(r.begin(&f, (uint32_t)f.buf.size()), "raw: begin() accepts a non-empty body");
        check(r.frames() == frames, "raw: frames = filesize/2");
        check(r.data_bytes() == frames * 2, "raw: data_bytes = filesize");

        // seek to frame 500 and read 4 frames -> values 500,501,502,503 (as int16 ramp)
        check(r.seek_to_frame(500), "raw: seek_to_frame succeeds");
        uint8_t blk[8]; uint32_t got = r.read(blk, 8);
        check(got == 8, "raw: reads the requested bytes after a seek");
        int16_t s0 = (int16_t)(blk[0] | (blk[1] << 8));
        check(s0 == (int16_t)(500 * 7 + 1), "raw: seek lands on the exact frame");

        // rewind goes back to frame 0
        r.rewind();
        got = r.read(blk, 2);
        int16_t r0 = (int16_t)(blk[0] | (blk[1] << 8));
        check(r0 == (int16_t)(0 * 7 + 1), "raw: rewind returns to frame 0");

        // seek past the end clamps to eof
        check(r.seek_to_frame(frames + 100), "raw: over-seek is clamped, not an error");
        check(r.eof(), "raw: a clamped over-seek reads as eof");

        // odd filesize is floored to a whole frame
        MemFile f2; f2.buf = ramp_i16(10); f2.buf.push_back(0xAA);   // 21 bytes
        RawStreamReader r2; r2.begin(&f2, (uint32_t)f2.buf.size());
        check(r2.frames() == 10, "raw: odd trailing byte is floored to a whole frame");
    }

    // A2. RawStreamReader::begin_wav - 16-bit mono PCM header parse, body offset, seek, rate, rejection.
    {
        const uint32_t frames = 500, rate = 44100;
        MemFile f; f.buf = wav_make(1, 1, 16, rate, frames);
        RawStreamReader r; uint32_t got_rate = 0;
        check(r.begin_wav(&f, (uint32_t)f.buf.size(), got_rate), "wav: begin_wav accepts 16-bit mono PCM");
        check(r.frames() == frames, "wav: frames = data-chunk size / 2 (past the header)");
        check(got_rate == rate, "wav: header sample rate reported out");

        check(r.seek_to_frame(100), "wav: seek_to_frame past the header");
        uint8_t blk[2]; r.read(blk, 2);
        int16_t s0 = (int16_t)(blk[0] | (blk[1] << 8));
        check(s0 == (int16_t)(100 * 7 + 1), "wav: seek lands on the exact frame (body-relative)");
        r.rewind(); r.read(blk, 2);
        int16_t r0 = (int16_t)(blk[0] | (blk[1] << 8));
        check(r0 == (int16_t)(0 * 7 + 1), "wav: rewind returns to the body start, not the file start");

        auto opens = [](std::vector<uint8_t> bytes) {
            MemFile m; m.buf = std::move(bytes); uint32_t rr; RawStreamReader x;
            return x.begin_wav(&m, (uint32_t)m.buf.size(), rr);
        };
        // Any decodable PCM shape is now accepted here and converted to the int16-mono frames these
        // engines consume (StreamDeck::start_play_wav inserts the adapter). Only genuinely undecodable
        // headers are refused - a station that cannot be played must not enter the bank.
        check(opens(wav_make(1, 2, 16, 48000, 100)), "wav: stereo accepted (downmixed by the adapter)");
        check(opens(wav_make(1, 1,  8, 48000, 100)), "wav: 8-bit accepted");
        check(opens(wav_make(3, 1, 32, 48000, 100)), "wav: float accepted");
        check(opens(wav_make(1, 1, 24, 48000, 100)), "wav: 24-bit accepted");
        check(!opens(wav_make(3, 1, 64, 48000, 100)), "wav: 64-bit float rejected (undecodable)");
        check(!opens(wav_make(2, 1,  4, 48000, 100)), "wav: ADPCM rejected (undecodable)");
        check(!opens(wav_make(1, 9, 16, 48000, 100)), "wav: 9 channels rejected (past the downmix bound)");

        // Lengths and seeks stay in SOURCE frames whatever the shape, which is what keeps the radio's
        // free-running playhead (a frame index modulo the station length) meaningful across formats.
        {
            MemFile m; m.buf = wav_make(1, 2, 24, 44100, 250);   // stereo i24: a 6-byte frame
            RawStreamReader x; uint32_t rr = 0;
            check(x.begin_wav(&m, (uint32_t)m.buf.size(), rr), "wide: stereo 24-bit station opens");
            check(x.src_frame_bytes() == 6 && x.src_channels() == 2, "wide: source frame size reported");
            check(x.frames() == 250, "wide: frames counted in source frames, not bytes");
            check(rr == 44100, "wide: the file's own rate still drives the pitch rebase");
            check(x.seek_to_frame(100), "wide: seek to a source frame");
            check(x.frames() - 100 == 150 && x.data_bytes() == 250 * 6, "wide: body length unchanged by the seek");
        }

        // The adapter over this reader: a stereo 24-bit body arrives as int16 mono frames, one per
        // source frame, with the two channels averaged.
        {
            auto le16 = [](std::vector<uint8_t>& v, uint16_t x){ v.push_back(x & 0xff); v.push_back((x >> 8) & 0xff); };
            auto le32 = [](std::vector<uint8_t>& v, uint32_t x){ for (int i = 0; i < 4; i++) v.push_back((x >> (8 * i)) & 0xff); };
            auto tag  = [](std::vector<uint8_t>& v, const char* s){ for (int i = 0; i < 4; i++) v.push_back((uint8_t)s[i]); };
            const uint32_t N = 400;
            std::vector<uint8_t> v;
            tag(v, "RIFF"); le32(v, 36 + N * 6); tag(v, "WAVE");
            tag(v, "fmt "); le32(v, 16); le16(v, 1); le16(v, 2); le32(v, 48000);
            le32(v, 48000 * 6); le16(v, 6); le16(v, 24);
            tag(v, "data"); le32(v, N * 6);
            for (uint32_t i = 0; i < N; i++) {          // L = +x, R = 0 -> mono x/2
                uint8_t f[6];
                const float x = 0.5f;
                pcm_write1(f, PcmFormat::i24, x);
                pcm_write1(f + 3, PcmFormat::i24, 0.f);
                v.insert(v.end(), f, f + 6);
            }
            MemFile m; m.buf = std::move(v);
            RawStreamReader r; uint32_t rr = 0;
            check(r.begin_wav(&m, (uint32_t)m.buf.size(), rr), "adapter: stereo 24-bit station opens");
            ConvertingSource c;
            check(c.begin(&r, r.src_format(), r.src_channels(), kRawFrameFormat, kRawFrameChannels),
                  "adapter: configured to the int16-mono frames the engines consume");
            std::vector<uint8_t> out; uint8_t blk[256];
            for (int g = 0; g < 10000 && !c.eof(); g++) {
                uint32_t got = c.read(blk, sizeof(blk));
                if (!got) break;
                out.insert(out.end(), blk, blk + got);
            }
            check(out.size() == N * 2, "adapter: one int16 mono frame per source frame");
            int16_t s0 = (int16_t)(out[0] | (out[1] << 8));
            check(s0 > 8100 && s0 < 8300, "adapter: (0.5 + 0.0)/2 = 0.25 full-scale in int16");
        }
    }

    // --- B. RadioEngine through IEngine -----------------------------------------------------------
    FakeStream stream;
    auto make = [&](RadioEngine& e) {
        host::HostArena a; EngineContext c = host::make_context(a, time); c.stream = &stream;
        e.init(c);
    };

    // B1. Station quantization: PITCH (Speed) over a 4-station bank rounds to the nearest station.
    {
        RadioEngine e; make(e);
        e.set_param(ParamId::Env, DeckRef::A, 0.5f);   // high static -> immediate switching (no settle)
        e.set_param(ParamId::Speed, DeckRef::A, 0.0f);
        e.prepare();
        check(station_of(stream.last_path[0]) == 0, "station: knob 0.0 -> station 0");
        e.set_param(ParamId::Speed, DeckRef::A, 1.0f);
        e.prepare();
        check(station_of(stream.last_path[0]) == 3, "station: knob 1.0 -> last station");
        e.set_param(ParamId::Speed, DeckRef::A, 0.34f);   // round(0.34*3)=round(1.02)=1
        e.prepare();
        check(station_of(stream.last_path[0]) == 1, "station: knob 0.34 -> station 1");
        check(stream.last_loop[0], "station: stations open in loop mode (free-running)");
    }

    // B2. Bank select: Alt+PITCH (Aux) picks the folder; the open path reflects it.
    {
        RadioEngine e; make(e);
        e.set_param(ParamId::Env, DeckRef::A, 0.5f);
        e.set_param(ParamId::Speed, DeckRef::A, 0.0f);
        e.set_param(ParamId::Aux, DeckRef::A, 0.5f);   // 0.5*16 = bank 8
        e.prepare();
        check(std::strncmp(stream.last_path[0], "radio/8/", 8) == 0, "bank: Aux 0.5 -> /radio/8");
        check(e.param(ParamId::Aux, DeckRef::A) > 0.5f && e.param(ParamId::Aux, DeckRef::A) < 0.6f,
              "bank: Aux readback reports the selected bank");
    }

    // B3. Free-running playhead: after the clock advances, a station opens at (clock + start) mod L.
    {
        RadioEngine e; make(e);
        e.set_param(ParamId::Env, DeckRef::A, 0.5f);
        e.set_param(ParamId::Pos, DeckRef::A, 0.0f);      // START = 0
        e.set_param(ParamId::Speed, DeckRef::A, 0.0f);    // station 0 (L=10000)
        e.prepare();
        check(stream.last_frame[0] == 0, "playhead: first open at clock 0 lands at frame 0");

        bool fin = true;
        run(e, 500, fin);                                 // advance the clock 500 blocks = 48000 frames
        e.set_param(ParamId::Speed, DeckRef::A, 1.0f);    // switch to station 3 (L=48000)
        e.prepare();
        const uint32_t expect = (500u * (uint32_t)host::kBlock + 0u) % 48000u;  // = 48000 % 48000 = 0
        check(stream.last_frame[0] == expect, "playhead: reopen seeks to (clock+start) mod L");

        // a non-trivial modulo: switch to station 2 (L=33333) at the same clock
        run(e, 100, fin);                                 // +9600 -> clock 57600
        e.set_param(ParamId::Speed, DeckRef::A, 0.5f);    // round(0.5*3)=2 -> station 2 (L=33333)
        e.prepare();
        const uint32_t expect2 = (57600u) % 33333u;       // 24267
        check(stream.last_frame[0] == expect2, "playhead: modulo wraps for a shorter station");
        check(fin, "playhead: output stayed finite while advancing the clock");
    }

    // B4. START offset shifts the seek target.
    {
        RadioEngine e; make(e);
        e.set_param(ParamId::Env, DeckRef::A, 0.5f);
        e.set_param(ParamId::Speed, DeckRef::A, 0.0f);    // station 0, L=10000
        e.set_param(ParamId::Pos, DeckRef::A, 0.25f);     // START = 0.25 * 10000 = 2500
        e.prepare();
        check(stream.last_frame[0] == 2500, "start: POS offsets the seek by start*L (clock 0)");
    }

    // B5. Varispeed: SIZE maps to 0.5x..2x, output stays finite + bounded, and 0.5x != 2x.
    {
        RadioEngine e; make(e);
        e.set_param(ParamId::Env, DeckRef::A, 0.0f);      // no static
        e.set_param(ParamId::Speed, DeckRef::A, 0.0f);
        e.prepare();                                      // station playing
        bool f1 = true, f2 = true;
        stream.phase[0] = 0.0;
        e.set_param(ParamId::Size, DeckRef::A, 0.0f);     // 0.5x
        const auto slow = run(e, 60, f1);
        stream.phase[0] = 0.0;
        e.set_param(ParamId::Size, DeckRef::A, 1.0f);     // 2x
        const auto fast = run(e, 60, f2);
        check(f1 && f2, "varispeed: output is finite at both speeds");
        check(peak(slow) < 1.2f && peak(fast) < 1.2f, "varispeed: output bounded near 0 dBFS");
        check(sad(slow, fast) > 1.0f, "varispeed: 0.5x and 2x differ (resampler is wired)");
    }

    // B6. Static: ENV > 0 mixes noise on a switch; ENV = 0 is clean. Both finite.
    {
        RadioEngine e; make(e);
        e.set_param(ParamId::Speed, DeckRef::A, 0.0f);
        e.set_param(ParamId::Env, DeckRef::A, 0.0f);
        e.prepare();
        bool f1 = true; stream.phase[0] = 0.0;
        const auto clean = run(e, 8, f1);                 // ENV 0 -> pure station
        e.set_param(ParamId::Env, DeckRef::A, 0.9f);      // strong static
        e.set_param(ParamId::Speed, DeckRef::A, 1.0f);    // switch -> static burst
        e.prepare();
        bool f2 = true; stream.phase[0] = 0.0;
        const auto noisy = run(e, 8, f2);
        check(f1 && f2, "static: output finite with and without static");
        check(sad(clean, noisy) > 0.5f, "static: a switch with ENV>0 audibly differs from the clean signal");
    }

    // B7. Anti-stutter: hammering RESET (gate/pad) on a LIVE deck must NOT re-open it (re-opening flushes
    // the ring, and a spurious/floating gate firing every loop was the constant-stutter bug). RESET only
    // (re)starts a SILENT deck.
    {
        RadioEngine e; make(e);
        e.set_param(ParamId::Env, DeckRef::A, 0.5f);      // immediate-switch path -> opens on one prepare
        e.set_param(ParamId::Speed, DeckRef::A, 0.0f);
        e.prepare();                                      // opens station 0, now playing
        const int n0 = stream.open_calls[0];
        bool fin = true;
        for (int k = 0; k < 50; k++) {                    // simulate a floating gate chattering every loop
            e.on_gate_trigger(DeckRef::A);
            e.prepare();
            run(e, 1, fin);
        }
        check(stream.open_calls[0] == n0, "reset spam on a live deck causes no re-opens (anti-stutter)");
        check(fin, "reset-spam output stays finite");
    }

    // B7b. Boundary chatter: a target that oscillates between two stations (pot/CV noise at a boundary)
    // must NOT re-open repeatedly - the engine holds the last station until the target settles.
    {
        RadioEngine e; make(e);
        e.set_param(ParamId::Env, DeckRef::A, 0.5f);      // open immediately...
        e.set_param(ParamId::Speed, DeckRef::A, 0.5f);    // ...on a station (station 2)
        e.prepare();
        e.set_param(ParamId::Env, DeckRef::A, 0.0f);      // ...then low static, so the settle timer governs
        const int n0 = stream.open_calls[0];
        bool fin = true;
        for (int k = 0; k < 60; k++) {                    // jitter the V/oct CV across a station boundary
            e.cv_voct(DeckRef::A, (k & 1) ? 0.18f : -0.18f);
            e.prepare();
            run(e, 1, fin);
        }
        check(stream.open_calls[0] == n0, "boundary chatter does not re-open the station (no stutter)");
        check(fin, "boundary-chatter output stays finite");
    }

    // B8. On-card rate.txt rebases the resampler: a 44.1k card plays differently from the 48k default.
    {
        // 44.1k mode (rate.txt present). The rate ratio is applied in _render_deck whenever the deck is
        // playing, so we force the fake deck to play (independent of the open/settle path) and compare.
        std::strcpy(stream.rate_txt, "44100");
        RadioEngine e; make(e);
        e.set_param(ParamId::Env, DeckRef::A, 0.0f);
        e.set_param(ParamId::Size, DeckRef::A, 0.5f);    // unity varispeed
        e.set_param(ParamId::Speed, DeckRef::A, 0.0f);
        e.prepare();                                     // loads "44100" immediately (file present)
        bool f1 = true; stream.phase[0] = 0.0; stream.playing[0] = true;
        const auto at441 = run(e, 40, f1);

        // 48k default (no rate.txt)
        stream.rate_txt[0] = '\0';
        RadioEngine e2; make(e2);
        e2.set_param(ParamId::Env, DeckRef::A, 0.0f);
        e2.set_param(ParamId::Size, DeckRef::A, 0.5f);
        e2.set_param(ParamId::Speed, DeckRef::A, 0.0f);
        e2.prepare();
        bool f2 = true; stream.phase[0] = 0.0; stream.playing[0] = true;
        const auto at48 = run(e2, 40, f2);

        check(f1 && f2, "rate: output finite in both 44.1k and 48k modes");
        check(sad(at441, at48) > 1.0f, "rate: /radio/rate.txt=44100 rebases playback vs the 48k default");
    }

    // B9. .wav stations: the engine dispatches to start_play_wav and rebases to the header's OWN rate (no
    // rate.txt needed), so a 44.1k WAV and a 48k WAV play at different rates.
    {
        stream.rate_txt[0] = '\0';
        stream.wav_mode = true;

        stream.wav_rate = 44100;
        RadioEngine e; make(e);
        e.set_param(ParamId::Env, DeckRef::A, 0.5f);     // immediate open
        e.set_param(ParamId::Speed, DeckRef::A, 0.0f);
        e.prepare();
        check(stream.last_was_wav[0], "wav: engine opens a .wav station via start_play_wav");
        check(std::strstr(stream.last_path[0], ".wav") != nullptr, "wav: open path carries the .wav extension");
        bool f1 = true; stream.phase[0] = 0.0;
        const auto at441 = run(e, 40, f1);

        stream.wav_rate = 48000;
        RadioEngine e2; make(e2);
        e2.set_param(ParamId::Env, DeckRef::A, 0.5f);
        e2.set_param(ParamId::Speed, DeckRef::A, 0.0f);
        e2.prepare();
        bool f2 = true; stream.phase[0] = 0.0;
        const auto at48 = run(e2, 40, f2);

        check(f1 && f2, "wav: output finite");
        check(sad(at441, at48) > 1.0f, "wav: the header sample rate rebases playback (44.1k vs 48k differ)");

        stream.wav_mode = false;     // restore
    }

    if (g_failures == 0) { std::printf("OK: all radio checks passed\n"); return 0; }
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
}
