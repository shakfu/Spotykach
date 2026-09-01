#pragma once

// The ONE RIFF/WAVE chunk walk. Before this existed the same walk was written three times - the
// buffer parse in wav.h, WavStreamReader::begin (wav_stream.h) and RawStreamReader::begin_wav
// (raw_stream.h) - which agreed on the hard parts (WAVE_FORMAT_EXTENSIBLE unwrapping, word-aligned
// chunk stepping, a bounded scan) by maintenance rather than by construction. They now all parse
// through here and keep only their own accept/reject policy, which is the part that legitimately
// differs per engine (see docs/dev/unified-wav-reader.md).
//
// This header describes what a file IS; it decides nothing about whether an engine can play it.
// Pure and host-testable: no FatFs, no hardware, no allocation.

#include "memory/byte_file.h"

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace spotykach {

// Everything the 16-byte WAVEFORMAT states, plus where the samples are. Lossless w.r.t. `fmt `, so a
// caller can reconstruct a canonical WavHeader from it without inventing field values.
struct WavInfo {
    uint16_t audio_format    = 0;  // 1 = PCM integer, 3 = IEEE-754 float (EXTENSIBLE already unwrapped)
    uint16_t channels        = 0;
    uint32_t sample_rate     = 0;
    uint32_t byte_per_sec    = 0;
    uint16_t byte_per_bloc   = 0;  // block align
    uint16_t bits_per_sample = 0;
    uint32_t fmt_size        = 0;  // declared `fmt ` body size (16 PCM / 18 / 40 EXTENSIBLE)
    uint32_t data_start      = 0;  // byte offset of the `data` chunk BODY
    uint32_t data_size       = 0;  // DataSize as declared in the header (NOT clamped to the file)
};

// Scan bound: refuse a pathological chunk list rather than loop. 64 is far past any real file - the
// fattest layouts seen in the wild (fmt18 + fact + LIST/INFO + bext + cue) are under ten chunks.
inline constexpr uint32_t kWavMaxChunks = 64;

namespace detail {

inline uint16_t wav_rd16(const uint8_t* p, int o) {
    return static_cast<uint16_t>(p[o] | (p[o + 1] << 8));
}
inline uint32_t wav_rd32(const uint8_t* p, int o) {
    return static_cast<uint32_t>(p[o])            | (static_cast<uint32_t>(p[o + 1]) << 8) |
          (static_cast<uint32_t>(p[o + 2]) << 16) | (static_cast<uint32_t>(p[o + 3]) << 24);
}

// The walk itself, over anything offering `bool at(offset, dst, n)` - a random-access read that either
// delivers all n bytes or fails. Templated rather than virtual so the two callers (a file, a memory
// buffer) each get a flat inlined copy with no vtable; it is main-loop-only code either way.
//
// After the 12-byte RIFF/WAVE header the file is a list of `<4-byte id><LE32 size><body>` chunks, each
// followed by a single pad byte when `size` is odd. We step that list, capture `fmt `, stop at `data`,
// and SKIP everything else (`fact`, `LIST`, `JUNK`, `bext`, `cue `, ...) by its size - so `data` is
// found however much metadata precedes it, as a conformant reader must. Stepping via at() rather than
// a fixed window means there is no offset ceiling.
template <typename Src>
bool walk_wav(Src& src, WavInfo& out)
{
    uint8_t riff[12];
    if (!src.at(0, riff, sizeof(riff)))                                     return false;
    if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) return false;

    bool have_fmt = false;
    uint32_t pos  = 12;                              // first chunk begins right after RIFF/WAVE
    for (uint32_t guard = 0; guard < kWavMaxChunks; guard++) {
        uint8_t ch[8];
        if (!src.at(pos, ch, sizeof(ch)))            return false;   // ran off the end before `data`
        const uint32_t size = wav_rd32(ch, 4);
        const uint32_t body = pos + 8;

        if (std::memcmp(ch, "fmt ", 4) == 0) {
            if (size < 16)                           return false;   // malformed: WAVEFORMAT is >= 16 bytes
            uint8_t fb[16];
            if (!src.at(body, fb, sizeof(fb)))       return false;
            out.audio_format    = wav_rd16(fb, 0);
            out.channels        = wav_rd16(fb, 2);
            out.sample_rate     = wav_rd32(fb, 4);
            out.byte_per_sec    = wav_rd32(fb, 8);
            out.byte_per_bloc   = wav_rd16(fb, 12);
            out.bits_per_sample = wav_rd16(fb, 14);
            out.fmt_size        = size;
            // WAVE_FORMAT_EXTENSIBLE: the real format tag is the first 2 bytes of the SubFormat GUID
            // (at body+24, past cbSize(2) + wValidBitsPerSample(2) + dwChannelMask(4)).
            if (out.audio_format == 0xFFFE && size >= 40) {
                uint8_t tag[2];
                if (!src.at(body + 24, tag, sizeof(tag))) return false;
                out.audio_format = wav_rd16(tag, 0);
            }
            have_fmt = true;
        }
        else if (std::memcmp(ch, "data", 4) == 0) {
            if (!have_fmt)                           return false;   // `fmt ` must precede `data`
            out.data_start = body;
            out.data_size  = size;
            return true;
        }

        pos = body + size + (size & 1u);             // next chunk; chunks are word-aligned
    }
    return false;                                    // no `data` within kWavMaxChunks
}

// at() over an IByteFile: absolute seek + a full read, else fail.
struct FileSrc {
    IByteFile* f;
    bool at(uint32_t off, void* dst, uint32_t n) const {
        return f && f->seek(off) && f->read(dst, n) == n;
    }
};

// at() over a memory buffer: bounds-checked copy. The `size - off < n` form avoids the overflow an
// `off + n > size` comparison would have on a bogus offset.
struct MemSrc {
    const uint8_t* p;
    uint32_t       size;
    bool at(uint32_t off, void* dst, uint32_t n) const {
        if (!p || off > size || size - off < n) return false;
        std::memcpy(dst, p + off, n);
        return true;
    }
};

} // namespace detail

// Parse `f` (any position; this seeks). On success the file is left positioned at the data-chunk body,
// which is what every streaming caller wants next. Returns false on a missing/malformed header.
inline bool parse_wav(IByteFile* f, WavInfo& out)
{
    out = WavInfo{};
    detail::FileSrc src{ f };
    if (!detail::walk_wav(src, out)) return false;
    return f->seek(out.data_start);
}

// Parse the first `size` bytes of an in-memory copy of the file. `data_start`/`data_size` are still
// file-absolute, so a caller that only buffered the head can seek the real file to `data_start`.
inline bool parse_wav(const uint8_t* bytes, uint32_t size, WavInfo& out)
{
    out = WavInfo{};
    detail::MemSrc src{ bytes, size };
    return detail::walk_wav(src, out);
}

} // namespace spotykach
