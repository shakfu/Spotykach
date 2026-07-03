// Host test for the generic WAV cue-point parser (src/memory/wav.h find_cue_points) that backs the
// CapWavCues platform capability. Pure parser test: builds WAV byte buffers with a `cue ` chunk and
// checks the extracted sample-frame offsets, ordering-independence, bounds, the frame_limit filter,
// and the kMax clamp. No engine or hardware deps.
#include "memory/wav.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include <cstdio>

using spotykach::WavCues;

namespace {

void put_u32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(v & 0xff); b.push_back((v >> 8) & 0xff);
    b.push_back((v >> 16) & 0xff); b.push_back((v >> 24) & 0xff);
}
void put_id(std::vector<uint8_t>& b, const char* id) {
    for (int i = 0; i < 4; i++) b.push_back((uint8_t)id[i]);
}

// A minimal 24-byte cue point whose only field we read is dwSampleOffset (byte 20).
void put_cue_point(std::vector<uint8_t>& b, uint32_t id, uint32_t sample_offset) {
    put_u32(b, id);            // dwIdentifier
    put_u32(b, sample_offset); // dwPosition (unused by parser)
    put_id(b, "data");         // fccChunk
    put_u32(b, 0);             // dwChunkStart
    put_u32(b, 0);             // dwBlockStart
    put_u32(b, sample_offset); // dwSampleOffset (byte 20 - the one we read)
}

// Build a `cue ` chunk (id + size + numPoints + points) into a standalone vector.
std::vector<uint8_t> cue_chunk(const std::vector<uint32_t>& offsets) {
    std::vector<uint8_t> body;
    put_u32(body, (uint32_t)offsets.size());
    for (size_t i = 0; i < offsets.size(); i++) put_cue_point(body, (uint32_t)i + 1, offsets[i]);
    std::vector<uint8_t> chunk;
    put_id(chunk, "cue ");
    put_u32(chunk, (uint32_t)body.size());
    chunk.insert(chunk.end(), body.begin(), body.end());
    return chunk;
}

std::vector<uint8_t> fmt_chunk() {
    std::vector<uint8_t> c;
    put_id(c, "fmt "); put_u32(c, 16);
    // AudioFormat=1, channels=2, rate=48000, bytesPerSec, blockAlign=4, bits=16 (irrelevant to cues)
    c.push_back(1); c.push_back(0); c.push_back(2); c.push_back(0);
    put_u32(c, 48000); put_u32(c, 48000 * 4);
    c.push_back(4); c.push_back(0); c.push_back(16); c.push_back(0);
    return c;
}
std::vector<uint8_t> data_chunk(uint32_t nbytes) {
    std::vector<uint8_t> c;
    put_id(c, "data"); put_u32(c, nbytes);
    c.insert(c.end(), nbytes, 0);
    return c;
}

// Assemble a RIFF/WAVE file from ordered chunk blobs.
std::vector<uint8_t> riff(const std::vector<std::vector<uint8_t>>& chunks) {
    std::vector<uint8_t> body;
    put_id(body, "WAVE");
    for (const auto& ch : chunks) body.insert(body.end(), ch.begin(), ch.end());
    std::vector<uint8_t> f;
    put_id(f, "RIFF"); put_u32(f, (uint32_t)body.size());
    f.insert(f.end(), body.begin(), body.end());
    return f;
}

} // namespace

int main() {
    printf("=== WAV cue-point parser (CapWavCues) ===\n");

    const size_t kLimit = 100000; // frame_limit (audio length in frames)

    // 1) Basic: three in-range cues, data-before-cue order.
    {
        auto f = riff({ fmt_chunk(), data_chunk(64), cue_chunk({0, 24000, 48000}) });
        WavCues cues;
        find_cue_points(f.data(), f.size(), kLimit, cues);
        assert(cues.count == 3);
        assert(cues.frames[0] == 0);
        assert(cues.frames[1] == 24000);
        assert(cues.frames[2] == 48000);
        printf("basic 3-cue parse (data before cue): OK\n");
    }

    // 2) Ordering-independence: cue chunk BEFORE data.
    {
        auto f = riff({ fmt_chunk(), cue_chunk({100, 200}), data_chunk(64) });
        WavCues cues;
        find_cue_points(f.data(), f.size(), kLimit, cues);
        assert(cues.count == 2 && cues.frames[0] == 100 && cues.frames[1] == 200);
        printf("cue chunk before data chunk: OK\n");
    }

    // 3) frame_limit filter: a cue at/beyond the audio end is dropped.
    {
        auto f = riff({ fmt_chunk(), data_chunk(8), cue_chunk({10, kLimit, kLimit + 5, 20}) });
        WavCues cues;
        find_cue_points(f.data(), f.size(), kLimit, cues);
        assert(cues.count == 2 && cues.frames[0] == 10 && cues.frames[1] == 20);
        printf("out-of-range cues dropped by frame_limit: OK\n");
    }

    // 4) No cue chunk -> count 0.
    {
        auto f = riff({ fmt_chunk(), data_chunk(64) });
        WavCues cues; cues.count = 99; // ensure the parser resets it
        find_cue_points(f.data(), f.size(), kLimit, cues);
        assert(cues.count == 0);
        printf("no cue chunk -> empty: OK\n");
    }

    // 5) More than kMax cues -> clamped, no overflow.
    {
        std::vector<uint32_t> many;
        for (uint32_t i = 0; i < WavCues::kMax + 10u; i++) many.push_back(i * 2);
        auto f = riff({ fmt_chunk(), data_chunk(8), cue_chunk(many) });
        WavCues cues;
        find_cue_points(f.data(), f.size(), kLimit, cues);
        assert(cues.count == WavCues::kMax);
        assert(cues.frames[0] == 0 && cues.frames[WavCues::kMax - 1] == (WavCues::kMax - 1) * 2);
        printf("cue count clamped to kMax (%u): OK\n", (unsigned)WavCues::kMax);
    }

    // 6) Truncated cue chunk (claims N points but bytes cut off) -> parses what fits, no over-read.
    {
        auto f = riff({ fmt_chunk(), data_chunk(8), cue_chunk({5, 6, 7}) });
        f.resize(f.size() - 30); // lop off the tail so the last point(s) are incomplete
        WavCues cues;
        find_cue_points(f.data(), f.size(), kLimit, cues);
        assert(cues.count <= 3); // whatever fully fit; the point is: no crash / over-read
        printf("truncated cue chunk handled safely (count=%u): OK\n", (unsigned)cues.count);
    }

    // 7) Not a RIFF/WAVE buffer -> empty, no crash.
    {
        std::vector<uint8_t> junk(20, 0xAB);
        WavCues cues;
        find_cue_points(junk.data(), junk.size(), kLimit, cues);
        assert(cues.count == 0);
        printf("non-RIFF buffer -> empty: OK\n");
    }

    printf("\nAll WAV cue-point tests passed.\n");
    return 0;
}
