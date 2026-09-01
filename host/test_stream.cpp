// Host test for the SD-streaming core (no FatFs, no hardware): the lock-free SpscRing and the
// PlayStream/RecordStream state machines, driven against memory-backed fake source/sink at the audio
// block rate. Proves the hard part - wrap correctness, read-ahead, underrun/overrun policy, EOF and
// finalize - off-target, before any of it touches the Card/FatFs layer. Build: `make -C host test-stream`.

#include "memory/spsc_ring.h"
#include "memory/audio_stream.h"
#include "memory/byte_file.h"
#include "memory/wav_stream.h"
#include "memory/converting_source.h"
#include "engine/istreamdeck.h"   // BankEntry + bank_sort (scanner ordering)

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>

using namespace spotykach;

namespace {

int g_failures = 0;
void check(bool cond, const char* msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg); g_failures++; }
}

// Deterministic, order-sensitive byte pattern: any reorder/drop/dup shows up in a full-vector compare.
std::vector<uint8_t> ramp(uint32_t n) {
    std::vector<uint8_t> v(n);
    for (uint32_t i = 0; i < n; i++) v[i] = static_cast<uint8_t>(i * 31u + 7u);
    return v;
}

// A slow file body on "SD": hands out at most max_per_read bytes per read (simulates chunked FatFs).
struct MemSource : IChunkSource {
    std::vector<uint8_t> data;
    uint32_t pos = 0;
    uint32_t max_per_read;
    explicit MemSource(std::vector<uint8_t> d, uint32_t mpr = 0xffffffffu)
        : data(std::move(d)), max_per_read(mpr) {}
    uint32_t read(uint8_t* dst, uint32_t n) override {
        uint32_t avail = static_cast<uint32_t>(data.size()) - pos;
        uint32_t k = std::min(std::min(n, avail), max_per_read);
        std::memcpy(dst, data.data() + pos, k);
        pos += k;
        return k;
    }
    bool eof() const override { return pos >= data.size(); }
};

// A file being written on "SD": collects everything and records the finalize() call.
struct MemSink : IChunkSink {
    std::vector<uint8_t> data;
    bool finalized = false;
    uint32_t write(const uint8_t* src, uint32_t n) override {
        data.insert(data.end(), src, src + n);
        return n;
    }
    void finalize() override { finalized = true; }
};

// A seekable in-memory file standing in for an SD file (FatFs on device): grows on write, like a fresh
// FA_CREATE_ALWAYS file, so the streaming-WAV placeholder-then-patch path works.
struct MemFile : IByteFile {
    std::vector<uint8_t> buf;
    uint32_t cur = 0;
    uint32_t read(void* dst, uint32_t n) override {
        uint32_t avail = (cur < buf.size()) ? static_cast<uint32_t>(buf.size()) - cur : 0;
        uint32_t k = std::min(n, avail);
        std::memcpy(dst, buf.data() + cur, k);
        cur += k;
        return k;
    }
    uint32_t write(const void* src, uint32_t n) override {
        if (cur + n > buf.size()) buf.resize(cur + n);
        std::memcpy(buf.data() + cur, src, n);
        cur += n;
        return n;
    }
    bool seek(uint32_t pos) override { cur = pos; return true; }
};

constexpr uint32_t kBlock = 384; // 96 stereo int16 frames - the platform audio block, in bytes

} // namespace

int main() {
    // --- 1. SpscRing: wrap correctness + accounting under many small interleaved transfers ----------
    {
        uint8_t buf[16];
        SpscRing r; r.init(buf, sizeof(buf));   // tiny power-of-two cap forces frequent wraps
        check(r.capacity() == 16, "ring reports its capacity");
        check(r.writable() == 16 && r.readable() == 0, "fresh ring is empty / fully writable");

        const auto src = ramp(1000);
        std::vector<uint8_t> out;
        uint32_t written = 0, guard = 0;
        while (out.size() < src.size() && guard++ < 100000) {
            written += r.write(src.data() + written, std::min<uint32_t>(7, (uint32_t)src.size() - written));
            uint8_t tmp[5];
            uint32_t got = r.read(tmp, 5);
            out.insert(out.end(), tmp, tmp + got);
        }
        for (uint8_t tmp[5]; r.readable(); ) { uint32_t g = r.read(tmp, 5); out.insert(out.end(), tmp, tmp + g); }
        check(out.size() == src.size() && out == src, "ring delivers bytes in order across wraps (no drop/dup)");

        // full/empty edges
        SpscRing r2; uint8_t b2[8]; r2.init(b2, 8);
        check(r2.write(src.data(), 100) == 8, "write into empty ring caps at capacity");
        check(r2.writable() == 0 && r2.readable() == 8, "ring reports full");
        uint8_t sink8[8];
        check(r2.read(sink8, 100) == 8, "read from full ring caps at what's available");
        check(r2.readable() == 0, "ring empty after draining");
    }

    // --- 2. PlayStream happy path: no underrun, byte-exact, EOF finish ------------------------------
    {
        const auto file = ramp(19 * kBlock + 137);   // not block-aligned -> exercises the EOF tail
        MemSource src(file, 200);                     // SD hands out <=200 bytes/read
        uint8_t ringbuf[4096]; SpscRing ring; ring.init(ringbuf, sizeof(ringbuf));
        uint8_t scratch[512];
        PlayStream play; play.init(&ring, scratch, sizeof(scratch));
        play.start(&src);

        std::vector<uint8_t> out;
        uint8_t blk[kBlock];
        uint32_t guard = 0;
        while (!play.finished() && guard++ < 100000) {
            play.pump();                     // main loop reads ahead
            uint32_t got = play.consume(blk, kBlock);   // ISR drains a block
            out.insert(out.end(), blk, blk + got);
        }
        check(out.size() == file.size() && out == file, "play delivers the whole file, byte-exact, in order");
        check(play.underruns() == 0, "play has zero underruns when the pump keeps up");
        check(play.finished(), "play reports finished at EOF + drained");
    }

    // --- 3. PlayStream underrun: pump starved -> silence + counted (not a false 'finish') -----------
    {
        const auto file = ramp(8 * kBlock);
        MemSource src(file);
        uint8_t ringbuf[256]; SpscRing ring; ring.init(ringbuf, sizeof(ringbuf));
        uint8_t scratch[128];
        PlayStream play; play.init(&ring, scratch, sizeof(scratch));
        play.start(&src);
        play.pump();                          // fill the tiny ring once, then starve it

        uint8_t blk[kBlock];
        uint32_t got1 = play.consume(blk, kBlock);   // drains 256, 128 short -> silence + underrun
        bool tail_silent = true;
        for (uint32_t i = got1; i < kBlock; i++) if (blk[i] != 0) tail_silent = false;
        uint32_t got2 = play.consume(blk, kBlock);   // nothing left -> all silence + underrun
        check(play.underruns() >= 2, "starved play counts underruns");
        check(tail_silent && got2 == 0, "underrun shortfall is zero-filled (silence), not stale data");
        check(!play.finished(), "underrun is not mistaken for end-of-stream (file not at EOF)");
    }

    // --- 4. RecordStream happy path: byte-exact to sink, flush + finalize on stop -------------------
    {
        const auto take = ramp(23 * kBlock + 51);
        MemSink sink;
        uint8_t ringbuf[4096]; SpscRing ring; ring.init(ringbuf, sizeof(ringbuf));
        uint8_t scratch[512];
        RecordStream rec; rec.init(&ring, scratch, sizeof(scratch));
        rec.start(&sink);

        uint32_t pi = 0, guard = 0;
        while (pi < take.size()) {
            uint32_t chunk = std::min<uint32_t>(kBlock, (uint32_t)take.size() - pi);
            rec.produce(take.data() + pi, chunk);   // ISR pushes a block of input
            pi += chunk;
            rec.pump();                              // main loop drains to SD
        }
        rec.stop();
        while (!rec.finished() && guard++ < 100000) rec.pump();   // flush remaining, finalize
        check(sink.data.size() == take.size() && sink.data == take, "record writes the whole take, byte-exact");
        check(rec.overruns() == 0, "record has zero overruns when the pump keeps up");
        check(sink.finalized, "record finalizes the sink on stop");
    }

    // --- 5. RecordStream overrun: pump starved -> excess dropped + counted, never blocks ------------
    {
        MemSink sink;
        uint8_t ringbuf[256]; SpscRing ring; ring.init(ringbuf, sizeof(ringbuf));
        uint8_t scratch[128];
        RecordStream rec; rec.init(&ring, scratch, sizeof(scratch));
        rec.start(&sink);
        const auto blkdata = ramp(kBlock);
        rec.produce(blkdata.data(), kBlock);   // 256 fit, 128 dropped
        rec.produce(blkdata.data(), kBlock);   // 0 fit, 384 dropped
        check(rec.overruns() == (kBlock - 256) + kBlock, "record drops and counts exactly the overflow bytes");
    }

    // --- 6. Round trip: record a take to SD, then stream it back -> identical -----------------------
    {
        const auto take = ramp(31 * kBlock + 200);
        // record
        MemSink sink;
        uint8_t rb1[2048]; SpscRing ring1; ring1.init(rb1, sizeof(rb1));
        uint8_t sc1[256];
        RecordStream rec; rec.init(&ring1, sc1, sizeof(sc1)); rec.start(&sink);
        for (uint32_t pi = 0; pi < take.size(); ) {
            uint32_t c = std::min<uint32_t>(kBlock, (uint32_t)take.size() - pi);
            rec.produce(take.data() + pi, c); pi += c; rec.pump();
        }
        rec.stop(); while (!rec.finished()) rec.pump();
        // play back the recorded file
        MemSource src(sink.data, 173);
        uint8_t rb2[2048]; SpscRing ring2; ring2.init(rb2, sizeof(rb2));
        uint8_t sc2[256];
        PlayStream play; play.init(&ring2, sc2, sizeof(sc2)); play.start(&src);
        std::vector<uint8_t> out; uint8_t blk[kBlock]; uint32_t guard = 0;
        while (!play.finished() && guard++ < 100000) { play.pump(); uint32_t g = play.consume(blk, kBlock); out.insert(out.end(), blk, blk + g); }
        check(out == take, "round trip (record -> SD -> stream back) reproduces the input exactly");
    }

    // --- 7. Streaming WAV codec: placeholder header -> stream body -> patch on finalize -------------
    {
        const auto body = ramp(17 * kBlock + 99);
        MemFile file;
        WavStreamWriter w;
        check(w.begin(&file, 1), "WAV writer emits a placeholder header");
        for (uint32_t pi = 0; pi < body.size(); ) {       // stream the body in blocks
            uint32_t c = std::min<uint32_t>(kBlock, (uint32_t)body.size() - pi);
            w.write(body.data() + pi, c); pi += c;
        }
        w.finalize();
        check(file.buf.size() == 44u + body.size(), "streamed WAV = 44-byte header + body");
        WavHeader h; size_t hs = 0;
        bool parsed = wav_header(file.buf.data(), (uint32_t)file.buf.size(), h, hs);
        check(parsed && hs == 44 && h.DataSize == body.size(),
              "finalized header patches DataSize / body offset correctly");

        file.cur = 0;
        WavStreamReader r;
        check(r.begin(&file), "WAV reader parses the streamed header");
        std::vector<uint8_t> out; uint8_t blk[kBlock];
        for (uint32_t guard = 0; !r.eof() && guard < 100000; guard++) {
            uint32_t g = r.read(blk, kBlock);
            out.insert(out.end(), blk, blk + g);
            if (g == 0) break;
        }
        check(out == body, "WAV reader reproduces the body byte-exact (stops at DataSize)");
    }

    // --- 8. Full stack end-to-end: ISR -> ring -> WAV file -> ring -> ISR ---------------------------
    // The whole step-1 + step-2 path minus FatFs: record through the ring into a streamed WAV, then
    // play that WAV back through the ring. This is what the device must reproduce once Card backs IByteFile.
    {
        const auto take = ramp(29 * kBlock + 211);
        MemFile file;
        // record: produce() -> RecordStream -> WavStreamWriter -> MemFile
        WavStreamWriter w; w.begin(&file, 1);
        uint8_t rb1[2048]; SpscRing ring1; ring1.init(rb1, sizeof(rb1));
        uint8_t sc1[256];
        RecordStream rec; rec.init(&ring1, sc1, sizeof(sc1)); rec.start(&w);
        for (uint32_t pi = 0; pi < take.size(); ) {
            uint32_t c = std::min<uint32_t>(kBlock, (uint32_t)take.size() - pi);
            rec.produce(take.data() + pi, c); pi += c; rec.pump();
        }
        rec.stop(); for (uint32_t g = 0; !rec.finished() && g < 100000; g++) rec.pump();

        // play: MemFile -> WavStreamReader -> PlayStream -> consume()
        file.cur = 0;
        WavStreamReader r; check(r.begin(&file), "end-to-end: reader opens the just-recorded WAV");
        uint8_t rb2[2048]; SpscRing ring2; ring2.init(rb2, sizeof(rb2));
        uint8_t sc2[256];
        PlayStream play; play.init(&ring2, sc2, sizeof(sc2)); play.start(&r);
        std::vector<uint8_t> out; uint8_t blk[kBlock];
        for (uint32_t g = 0; !play.finished() && g < 100000; g++) {
            play.pump(); uint32_t got = play.consume(blk, kBlock); out.insert(out.end(), blk, blk + got);
        }
        check(out == take, "end-to-end record->WAV->play reproduces the input exactly");
        check(play.underruns() == 0, "end-to-end playback has no underruns");
    }

    // --- 9. Looping: PlayStream rewinds the WAV at EOF and repeats the body seamlessly --------------
    {
        const auto body = ramp(5 * kBlock + 137);
        MemFile file;
        WavStreamWriter w; w.begin(&file, 1);
        for (uint32_t pi = 0; pi < body.size(); ) {
            uint32_t c = std::min<uint32_t>(kBlock, (uint32_t)body.size() - pi);
            w.write(body.data() + pi, c); pi += c;
        }
        w.finalize();

        file.cur = 0;
        WavStreamReader r; check(r.begin(&file), "loop: reader opens the WAV");
        check(r.data_bytes() == body.size(), "loop: reader reports body length for loop sizing");

        uint8_t rb[2048]; SpscRing ring; ring.init(rb, sizeof(rb));
        uint8_t sc[256];
        PlayStream play; play.init(&ring, sc, sizeof(sc)); play.start(&r);
        play.set_loop(true);

        // Read ~2.5 loops' worth; it must equal the body tiled (rewind wraps cleanly) and never finish.
        const uint32_t want = body.size() * 2 + body.size() / 2;
        std::vector<uint8_t> out; uint8_t blk[kBlock]; uint32_t guard = 0;
        while (out.size() < want && guard++ < 100000) {
            play.pump(); uint32_t g = play.consume(blk, kBlock); out.insert(out.end(), blk, blk + g);
        }
        check(!play.finished(), "loop: a looping stream never reports finished");
        bool tiled_ok = out.size() >= want;
        for (uint32_t i = 0; i < want && tiled_ok; i++) if (out[i] != body[i % body.size()]) tiled_ok = false;
        check(tiled_ok, "loop: playback repeats the body seamlessly across the rewind");
    }

    // --- 10. Format acceptance + header robustness. The reader accepts any PCM depth this firmware can
    // convert, mono or multichannel, at 48 kHz; StreamDeck wraps a non-native file in a ConvertingSource
    // (section 11) so the engine still receives native mono frames. Off-RATE is still a hard reject -
    // converting that means resampling, which changes what a frame count means. It must also find `data`
    // even when an externally-authored WAV prepends a fat metadata chunk.
    {
        auto le16 = [](std::vector<uint8_t>& v, uint16_t x){ v.push_back(x & 0xff); v.push_back((x >> 8) & 0xff); };
        auto le32 = [](std::vector<uint8_t>& v, uint32_t x){ for (int i = 0; i < 4; i++) v.push_back((x >> (8 * i)) & 0xff); };
        auto tag  = [](std::vector<uint8_t>& v, const char* s){ for (int i = 0; i < 4; i++) v.push_back((uint8_t)s[i]); };
        // Build a WAV: fmt chunk (+ optional JUNK metadata chunk of `junk` bytes before data) + body ramp.
        auto make_wav = [&](uint16_t fmt, uint16_t ch, uint16_t bits, uint32_t sr, uint32_t junk, uint32_t body) {
            const uint16_t blockAlign = ch * (bits / 8);
            std::vector<uint8_t> v;
            tag(v, "RIFF"); le32(v, 0); tag(v, "WAVE");
            tag(v, "fmt "); le32(v, 16); le16(v, fmt); le16(v, ch); le32(v, sr);
            le32(v, sr * blockAlign); le16(v, blockAlign); le16(v, bits);
            if (junk) { tag(v, "JUNK"); le32(v, junk); for (uint32_t i = 0; i < junk; i++) v.push_back((uint8_t)(i & 0xff)); }
            tag(v, "data"); le32(v, body);
            for (uint32_t i = 0; i < body; i++) v.push_back((uint8_t)(i & 0xff));
            const uint32_t riff = (uint32_t)v.size() - 8;
            v[4] = riff & 0xff; v[5] = (riff >> 8) & 0xff; v[6] = (riff >> 16) & 0xff; v[7] = (riff >> 24) & 0xff;
            return v;
        };
        const uint32_t body = 3 * kBlock + 17;
        auto accepts = [&](std::vector<uint8_t> bytes) {
            MemFile mf; mf.buf = std::move(bytes); mf.cur = 0; WavStreamReader r; return r.begin(&mf);
        };
        const uint16_t wrongFmt  = (kWavAudioFormat == 3) ? 1 : 3;     // int vs float, opposite the build
        const uint16_t wrongBits = (kWavBitsPerSample == 32) ? 16 : 32;

        (void)wrongFmt; (void)wrongBits;

        check(accepts(make_wav(kWavAudioFormat, 1, kWavBitsPerSample, 48000, 0, body)), "acceptance: native mono file accepted");
        check(accepts(make_wav(kWavAudioFormat, 2, kWavBitsPerSample, 48000, 0, body)), "acceptance: stereo accepted (downmixed by the adapter)");
        check(accepts(make_wav(1, 1, 16, 48000, 0, body)), "acceptance: 16-bit PCM accepted");
        check(accepts(make_wav(1, 1, 24, 48000, 0, body)), "acceptance: 24-bit PCM accepted");
        check(accepts(make_wav(1, 1, 32, 48000, 0, body)), "acceptance: 32-bit INTEGER PCM accepted (was the classic silent-noise case)");
        check(accepts(make_wav(1, 1,  8, 48000, 0, body)), "acceptance: 8-bit PCM accepted");
        check(accepts(make_wav(1, 2, 24, 48000, 0, body)), "acceptance: stereo 24-bit accepted (both axes at once)");

        check(accepts(make_wav(kWavAudioFormat, 1, kWavBitsPerSample, 44100, 0, body)), "acceptance: 44.1 kHz accepted (resampled by the adapter)");
        check(accepts(make_wav(1, 2, 24, 96000, 0, body)), "acceptance: 96 kHz stereo 24-bit accepted (all three axes at once)");

        // Still refused: a nonsense rate, and a format nothing can decode.
        check(!accepts(make_wav(kWavAudioFormat, 1, kWavBitsPerSample, 100, 0, body)), "acceptance: an absurd sample rate rejected");
        check(!accepts(make_wav(3, 1, 64, 48000, 0, body)), "acceptance: 64-bit float rejected (undecodable)");
        check(!accepts(make_wav(2, 1,  4, 48000, 0, body)), "acceptance: ADPCM rejected (undecodable)");
        check(!accepts(make_wav(1, 9, 16, 48000, 0, body)), "acceptance: 9 channels rejected (past the downmix bound)");

        // The reader reports the file's own shape, and a length in SOURCE frames - the count the engine
        // ends up with, since conversion changes a frame's width, never how many frames there are.
        {
            MemFile mf; mf.buf = make_wav(1, 2, 24, 48000, 0, 6 * 60); mf.cur = 0;   // 60 stereo i24 frames
            WavStreamReader r;
            check(r.begin(&mf), "shape: a stereo 24-bit file parses");
            check(r.src_format() == PcmFormat::i24 && r.src_channels() == 2, "shape: file format/channels reported out");
            check(r.src_frame_bytes() == 6 && r.frames() == 60, "shape: frames counted in source frames, not bytes");
            check(r.src_rate() == 48000, "shape: the file's own rate reported out");
        }

        // Resampling accounting: the length an ENGINE receives is the source length at the device
        // rate, which is what keeps a 44.1 kHz loop the same number of buffer frames as a take
        // recorded on the device (and so keeps softcut's loop lengths and sync honest).
        {
            check(ConvertingSource::out_frames(44100, 44100, 48000) == 48000, "rate: 1 s at 44.1k -> 1 s of device frames");
            check(ConvertingSource::out_frames(96000, 96000, 48000) == 48000, "rate: 1 s at 96k -> 1 s of device frames");
            check(ConvertingSource::out_frames(1000, 48000, 48000) == 1000, "rate: no rate change is exact");
            check(!ConvertingSource::is_identity(PcmFormat::f32, 1, PcmFormat::f32, 1, 44100, 48000),
                  "rate: an off-rate file is not an identity, however native its depth");
            check(ConvertingSource::is_identity(PcmFormat::f32, 1, PcmFormat::f32, 1, 48000, 48000),
                  "rate: a native file at the device rate still bypasses the adapter entirely");
        }

        // Robustness: a 64-byte metadata chunk pushes `data` well past offset 64; the old 64-byte window
        // would miss it (begin -> false -> silent no-load), the widened window must still find + stream it.
        {
            MemFile mf; mf.buf = make_wav(kWavAudioFormat, 1, kWavBitsPerSample, 48000, 64, body); mf.cur = 0;
            WavStreamReader r;
            check(r.begin(&mf), "robustness: data behind a metadata chunk (past byte 64) still parses");
            std::vector<uint8_t> out(body); uint32_t g = r.read(out.data(), body);
            bool body_ok = (g == body);
            for (uint32_t i = 0; i < body && body_ok; i++) if (out[i] != (uint8_t)(i & 0xff)) body_ok = false;
            check(body_ok, "robustness: body after the metadata chunk reads back intact");
        }

        // Spec-compliant chunk walk: build a WAV from an explicit chunk list so we can reproduce the
        // exact real-world layouts a conformant reader must accept (any unknown chunk, any order, any
        // amount of leading metadata, odd-sized chunks with their pad byte).
        auto put = [&](std::vector<uint8_t>& v, const char* id, const std::vector<uint8_t>& d) {
            tag(v, id); le32(v, (uint32_t)d.size()); v.insert(v.end(), d.begin(), d.end());
            if (d.size() & 1) v.push_back(0);          // chunks are word-aligned: pad odd sizes
        };
        auto fmt_body = [&](uint16_t af, uint16_t ch, uint16_t bits, uint32_t sr, bool ext18) {
            std::vector<uint8_t> b; const uint16_t ba = ch * (bits / 8);
            le16(b, af); le16(b, ch); le32(b, sr); le32(b, sr * ba); le16(b, ba); le16(b, bits);
            if (ext18) le16(b, 0);                      // cbSize=0 -> an 18-byte (non-PCM) fmt chunk
            return b;
        };
        auto wrap = [&](std::vector<uint8_t> v) {       // patch the RIFF size, hand back the bytes
            const uint32_t riff = (uint32_t)v.size() - 8;
            v[4] = riff & 0xff; v[5] = (riff >> 8) & 0xff; v[6] = (riff >> 16) & 0xff; v[7] = (riff >> 24) & 0xff;
            return v;
        };
        auto bodyramp = [&](uint32_t n) { std::vector<uint8_t> d(n); for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)(i & 0xff); return d; };
        auto reads_body = [&](std::vector<uint8_t> bytes, uint32_t n) {
            MemFile mf; mf.buf = std::move(bytes); mf.cur = 0; WavStreamReader r;
            if (!r.begin(&mf)) return false;
            std::vector<uint8_t> out(n); if (r.read(out.data(), n) != n) return false;
            for (uint32_t i = 0; i < n; i++) if (out[i] != (uint8_t)(i & 0xff)) return false;
            return true;
        };

        // (a) The exact externally-authored layout that broke on hardware: an 18-byte float `fmt ` +
        // `fact` + a 62-byte `LIST`/INFO block push `data` to offset 128. Must parse and stream intact.
        {
            std::vector<uint8_t> v; tag(v, "RIFF"); le32(v, 0); tag(v, "WAVE");
            put(v, "fmt ", fmt_body(kWavAudioFormat, 1, kWavBitsPerSample, 48000, /*ext18=*/true));
            put(v, "fact", std::vector<uint8_t>(4, 0));
            put(v, "LIST", std::vector<uint8_t>(62, 0xAB));
            put(v, "data", bodyramp(body));
            check(reads_body(wrap(v), body), "spec: fmt18 + fact + LIST (data@128, the hardware-noise layout) parses + streams");
        }
        // (b) Metadata past the OLD 256-byte window: a 300-byte JUNK chunk. The old buffer reader gave up
        // here; the file-walking reader must seek through and still find `data`.
        check(reads_body(make_wav(kWavAudioFormat, 1, kWavBitsPerSample, 48000, 300, body), body),
              "spec: data behind a 300-byte chunk (past the old 256-byte window) parses");
        // (c) A chunk BEFORE `fmt ` (legal ordering) must be skipped, not mistaken for fmt/data.
        {
            std::vector<uint8_t> v; tag(v, "RIFF"); le32(v, 0); tag(v, "WAVE");
            put(v, "JUNK", std::vector<uint8_t>(20, 0));
            put(v, "fmt ", fmt_body(kWavAudioFormat, 1, kWavBitsPerSample, 48000, false));
            put(v, "data", bodyramp(body));
            check(reads_body(wrap(v), body), "spec: a chunk before fmt is skipped, fmt+data still found");
        }
        // (d) An odd-sized chunk's pad byte must be accounted for, or `data` mis-aligns into garbage.
        {
            std::vector<uint8_t> v; tag(v, "RIFF"); le32(v, 0); tag(v, "WAVE");
            put(v, "fmt ", fmt_body(kWavAudioFormat, 1, kWavBitsPerSample, 48000, false));
            put(v, "JUNK", std::vector<uint8_t>(3, 0x7));   // odd size -> 1 pad byte
            put(v, "data", bodyramp(body));
            check(reads_body(wrap(v), body), "spec: odd-sized chunk pad byte handled (data stays aligned)");
        }
    }

    // --- 11. ConvertingSource: the depth/channel adapter that lets a non-native file reach a native
    // engine. Everything here runs in the main-loop pump; the ISR still only drains the ring.
    {
        // A source that hands back deliberately awkward byte counts, ending mid-frame. FatFs behind a
        // ring does exactly this, and a frame stitched across two reads is the failure it causes.
        struct DribbleSource : IChunkSource {
            std::vector<uint8_t> buf; uint32_t cur = 0; uint32_t step = 3;
            uint32_t read(uint8_t* dst, uint32_t n) override {
                uint32_t want = std::min<uint32_t>(std::min<uint32_t>(n, step), (uint32_t)buf.size() - cur);
                std::memcpy(dst, buf.data() + cur, want); cur += want; return want;
            }
            bool eof() const override { return cur >= buf.size(); }
            void rewind() override { cur = 0; }
        };
        auto drain = [](IChunkSource& src, uint32_t chunk) {          // read to exhaustion
            std::vector<uint8_t> out; uint8_t blk[4096];
            for (uint32_t guard = 0; guard < 100000 && !src.eof(); guard++) {
                uint32_t g = src.read(blk, chunk);
                if (!g && src.eof()) break;
                if (!g) break;
                out.insert(out.end(), blk, blk + g);
            }
            return out;
        };
        auto as_floats = [](const std::vector<uint8_t>& b) {
            std::vector<float> f(b.size() / 4);
            std::memcpy(f.data(), b.data(), f.size() * 4);
            return f;
        };

        check(ConvertingSource::is_identity(PcmFormat::f32, 1, PcmFormat::f32, 1), "adapter: native mono is an identity (bypassed)");
        check(!ConvertingSource::is_identity(PcmFormat::f32, 2, PcmFormat::f32, 1), "adapter: stereo is not an identity");
        check(!ConvertingSource::is_identity(PcmFormat::i16, 1, PcmFormat::f32, 1), "adapter: a different depth is not an identity");

        // i16 mono -> f32 mono, through a source that dribbles 3 bytes at a time (so every other frame
        // straddles a read). Values and count must both survive.
        {
            const int N = 200;
            DribbleSource src; src.buf.resize(N * 2);
            for (int i = 0; i < N; i++) {
                const int16_t v = (int16_t)((i - N / 2) * 100);
                std::memcpy(src.buf.data() + i * 2, &v, 2);
            }
            ConvertingSource c;
            check(c.begin(&src, PcmFormat::i16, 1, PcmFormat::f32, 1), "adapter: i16 mono -> f32 mono begins");
            const auto out = as_floats(drain(c, 37 * 4));
            bool ok = (out.size() == (size_t)N);
            for (int i = 0; i < N && ok; i++) {
                const float want = (float)((int16_t)((i - N / 2) * 100)) * (1.f / 32767.f);
                if (std::fabs(out[i] - want) > 1e-6f) ok = false;
            }
            check(ok, "adapter: i16 -> f32 exact, and no frame lost across a mid-frame read boundary");
        }

        // Stereo -> mono is an AVERAGE, not a channel drop: material only on one side must survive.
        {
            const int N = 64;
            DribbleSource src; src.buf.resize(N * 8); src.step = 5;   // stereo f32, awkward dribble
            for (int i = 0; i < N; i++) {
                const float l = 0.5f, r = -0.25f;
                std::memcpy(src.buf.data() + i * 8,     &l, 4);
                std::memcpy(src.buf.data() + i * 8 + 4, &r, 4);
            }
            ConvertingSource c;
            check(c.begin(&src, PcmFormat::f32, 2, PcmFormat::f32, 1), "adapter: f32 stereo -> mono begins");
            check(c.src_frame_bytes() == 8 && c.dst_frame_bytes() == 4, "adapter: frame sizes reflect both formats");
            const auto out = as_floats(drain(c, 4096));
            bool ok = out.size() == (size_t)N;
            for (float v : out) if (std::fabs(v - 0.125f) > 1e-6f) ok = false;   // (0.5 + -0.25)/2
            check(ok, "adapter: stereo folds to mono by averaging (one-sided material is kept)");
        }

        // Mono -> stereo duplicates (the granular loop buffer's direction).
        {
            DribbleSource src; src.buf.resize(4 * 4); src.step = 4096;
            for (int i = 0; i < 4; i++) { const float v = 0.1f * (i + 1); std::memcpy(src.buf.data() + i * 4, &v, 4); }
            ConvertingSource c;
            check(c.begin(&src, PcmFormat::f32, 1, PcmFormat::f32, 2), "adapter: mono -> stereo begins");
            const auto out = as_floats(drain(c, 4096));
            bool ok = out.size() == 8;
            for (int i = 0; i < 4 && ok; i++) {
                if (std::fabs(out[i * 2] - out[i * 2 + 1]) > 1e-9f) ok = false;          // L == R
                if (std::fabs(out[i * 2] - 0.1f * (i + 1)) > 1e-6f) ok = false;
            }
            check(ok, "adapter: mono duplicates into both channels");
        }

        // Whole destination frames only: a read whose `n` is not frame-aligned returns the frames that
        // fit and keeps the rest, rather than emitting a partial sample.
        {
            DribbleSource src; src.buf.resize(16 * 2); src.step = 4096;
            ConvertingSource c; c.begin(&src, PcmFormat::i16, 1, PcmFormat::f32, 1);
            uint8_t blk[16];
            check(c.read(blk, 10) == 8, "adapter: a non-frame-aligned read returns whole frames only");
        }

        // A file truncated mid-frame drops the partial tail rather than emitting half a sample, and
        // still reports eof so the stream can finish.
        {
            DribbleSource src; src.buf.resize(4 * 3 + 2); src.step = 4096;   // 4 i24 frames + 2 stray bytes
            ConvertingSource c; c.begin(&src, PcmFormat::i24, 1, PcmFormat::f32, 1);
            const auto out = drain(c, 4096);
            check(out.size() == 16, "adapter: a trailing partial frame is dropped");
            check(c.eof(), "adapter: eof once the source is dry and no whole frame is staged");
        }

        // rewind clears the carried partial frame, so a looping stream does not stitch the first frame
        // of pass two out of bytes from either side of the seam.
        {
            DribbleSource src; src.buf.resize(3 * 2); src.step = 3;
            ConvertingSource c; c.begin(&src, PcmFormat::i16, 1, PcmFormat::f32, 1);
            uint8_t blk[4]; c.read(blk, 4);              // leaves 1 carried byte
            c.rewind();
            const auto out = drain(c, 4096);
            check(out.size() == 12, "adapter: rewind drops the carry and replays every frame");
        }

        // Resampling: a 24 kHz ramp becomes twice as many frames at 48 kHz, interpolated - and it does
        // that through a source that dribbles bytes, so the interpolator's state has to survive chunk
        // boundaries (a discontinuity there is an audible tick every read).
        {
            const int N = 512;
            DribbleSource src; src.buf.resize(N * 4); src.step = 7;
            for (int i = 0; i < N; i++) { const float v = (float)i / (float)N; std::memcpy(src.buf.data() + i * 4, &v, 4); }
            ConvertingSource c;
            check(c.begin(&src, PcmFormat::f32, 1, PcmFormat::f32, 1, 24000, 48000), "resample: 24k -> 48k begins");
            const auto out = as_floats(drain(c, 97 * 4));
            // 2x the frames, give or take the final partial step at the end of the source.
            check(out.size() >= (size_t)(2 * N - 2) && out.size() <= (size_t)(2 * N),
                  "resample: 24k -> 48k roughly doubles the frame count");
            bool monotone = true, halved = true;
            for (size_t i = 1; i < out.size(); i++) if (out[i] < out[i - 1] - 1e-6f) monotone = false;
            // A ramp resampled 2:1 advances by half the source step per output frame.
            for (size_t i = 1; i < out.size() - 2; i++) {
                if (std::fabs((out[i] - out[i - 1]) - 0.5f / (float)N) > 1e-5f) halved = false;
            }
            check(monotone, "resample: output stays monotone across every chunk boundary (no discontinuity)");
            check(halved, "resample: the interpolated step is exactly half the source step");
        }

        // Downward: 96 kHz halves the frame count, and the values are the even-indexed source frames.
        {
            const int N = 400;
            DribbleSource src; src.buf.resize(N * 4); src.step = 4096;
            for (int i = 0; i < N; i++) { const float v = (float)i; std::memcpy(src.buf.data() + i * 4, &v, 4); }
            ConvertingSource c;
            check(c.begin(&src, PcmFormat::f32, 1, PcmFormat::f32, 1, 96000, 48000), "resample: 96k -> 48k begins");
            const auto out = as_floats(drain(c, 4096));
            check(out.size() >= (size_t)(N / 2 - 1) && out.size() <= (size_t)(N / 2 + 1),
                  "resample: 96k -> 48k roughly halves the frame count");
            bool picked = true;
            for (size_t i = 0; i < out.size() && picked; i++) if (std::fabs(out[i] - (float)(2 * i)) > 1e-3f) picked = false;
            check(picked, "resample: 2:1 lands exactly on every other source frame");
        }

        // rewind must reset the interpolator, not just the byte carry: a looping stream that kept its
        // phase would drift one fractional frame per pass and smear the seam.
        {
            const int N = 64;
            DribbleSource src; src.buf.resize(N * 4); src.step = 4096;
            for (int i = 0; i < N; i++) { const float v = (float)i; std::memcpy(src.buf.data() + i * 4, &v, 4); }
            ConvertingSource c; c.begin(&src, PcmFormat::f32, 1, PcmFormat::f32, 1, 32000, 48000);
            const auto first = as_floats(drain(c, 4096));
            c.rewind();
            const auto second = as_floats(drain(c, 4096));
            check(!first.empty() && first == second, "resample: rewind replays the pass identically (phase reset)");
        }

        // End to end, the real path: a stereo 24-bit WAV on "disk" -> WavStreamReader -> ConvertingSource
        // -> PlayStream -> the ISR's consume(), landing as native mono float frames.
        {
            auto le16 = [](std::vector<uint8_t>& v, uint16_t x){ v.push_back(x & 0xff); v.push_back((x >> 8) & 0xff); };
            auto le32 = [](std::vector<uint8_t>& v, uint32_t x){ for (int i = 0; i < 4; i++) v.push_back((x >> (8 * i)) & 0xff); };
            auto tag  = [](std::vector<uint8_t>& v, const char* s){ for (int i = 0; i < 4; i++) v.push_back((uint8_t)s[i]); };
            const uint32_t N = 300;
            std::vector<uint8_t> v;
            tag(v, "RIFF"); le32(v, 0); tag(v, "WAVE");
            tag(v, "fmt "); le32(v, 16); le16(v, 1); le16(v, 2); le32(v, 48000);
            le32(v, 48000 * 6); le16(v, 6); le16(v, 24);
            tag(v, "data"); le32(v, N * 6);
            for (uint32_t i = 0; i < N; i++) {                       // L = +ramp, R = -ramp -> mono 0
                uint8_t f[6];
                const float x = (float)i / (float)N * 0.8f;
                pcm_write1(f,     PcmFormat::i24,  x);
                pcm_write1(f + 3, PcmFormat::i24, -x);
                v.insert(v.end(), f, f + 6);
            }
            const uint32_t riff = (uint32_t)v.size() - 8;
            v[4] = riff & 0xff; v[5] = (riff >> 8) & 0xff; v[6] = (riff >> 16) & 0xff; v[7] = (riff >> 24) & 0xff;

            MemFile file; file.buf = std::move(v); file.cur = 0;
            WavStreamReader r;
            check(r.begin(&file), "end-to-end: stereo 24-bit WAV opens");
            check(r.frames() == N, "end-to-end: length in source frames");

            ConvertingSource conv;
            check(conv.begin(&r, r.src_format(), r.src_channels(), kStreamFrameFormat, kStreamFrameChannels),
                  "end-to-end: adapter configured from the file's own shape");

            SpscRing ring; std::vector<uint8_t> mem(4096); ring.init(mem.data(), (uint32_t)mem.size());
            uint8_t scratch[512];
            PlayStream play; play.init(&ring, scratch, sizeof(scratch));
            play.start(&conv);

            std::vector<uint8_t> got; uint8_t blk[128];
            for (uint32_t guard = 0; guard < 10000 && !play.finished(); guard++) {
                play.pump();
                got.insert(got.end(), blk, blk + play.consume(blk, sizeof(blk)));
            }
            const auto f = as_floats(got);
            bool silent = f.size() >= N;
            for (uint32_t i = 0; i < N && silent; i++) if (std::fabs(f[i]) > 1e-5f) silent = false;
            check(silent, "end-to-end: L/-L stereo downmixes to silence through the whole pump path");
            check(play.underruns() == 0, "end-to-end: the adapter keeps the ring fed (no underruns)");
        }
    }

    // --- bank_sort: scanned entries ordered by case-insensitive name (deterministic, not FAT order) ----
    {
        auto mk = [](const char* nm) {
            BankEntry e{}; std::strncpy(e.name, nm, 12); e.name[12] = '\0';
            e.frames = 48000; e.rate = 0; e.is_wav = false; return e;
        };
        BankEntry b[5] = { mk("Zoo.wav"), mk("apple.raw"), mk("10.wav"), mk("2.wav"), mk("Bee.raw") };
        bank_sort(b, 5);
        // Case-insensitive lexicographic: digits < letters, case folded. ("10" before "2" is the lexicographic
        // caveat that motivates zero-padding numeric names.)
        check(std::strcmp(b[0].name, "10.wav")   == 0, "bank_sort: digit-leading first ('10.wav')");
        check(std::strcmp(b[1].name, "2.wav")    == 0, "bank_sort: lexicographic ('2' after '10')");
        check(std::strcmp(b[2].name, "apple.raw")== 0, "bank_sort: letters after digits, case-insensitive");
        check(std::strcmp(b[3].name, "Bee.raw")  == 0, "bank_sort: case-insensitive ('Bee' before 'Zoo')");
        check(std::strcmp(b[4].name, "Zoo.wav")  == 0, "bank_sort: last entry");
        // Stable + idempotent: re-sorting an ordered bank leaves it unchanged; payload travels with the name.
        check(b[0].frames == 48000 && b[2].is_wav == false, "bank_sort: entry payload preserved");
        bank_sort(b, 5);
        check(std::strcmp(b[0].name, "10.wav") == 0 && std::strcmp(b[4].name, "Zoo.wav") == 0,
              "bank_sort: idempotent on an already-sorted bank");
        BankEntry one[1] = { mk("solo.wav") };
        bank_sort(one, 1); bank_sort(b, 0);   // n<=1 is a no-op, must not read out of bounds
        check(std::strcmp(one[0].name, "solo.wav") == 0, "bank_sort: n<=1 no-op");
    }

    if (g_failures == 0) { std::printf("OK: all stream checks passed\n"); return 0; }
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
}
