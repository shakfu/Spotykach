#pragma once
// Block conversion between PCM sample formats for the audio load/stream paths.
// Pure, host-testable; depends only on the sample16 helpers.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>
#include "sample16.h"

namespace spotykach {

// Every sample layout a WAV `fmt ` chunk can state that this firmware handles. The tag is what the
// bit depth alone cannot say: 32 bits is either integer or IEEE float, and 8-bit WAV is UNSIGNED
// offset-binary (128 = silence) where every other integer depth is signed two's-complement.
enum class PcmFormat : uint8_t { u8, i16, i24, i32, f32 };

inline uint8_t pcm_bytes(PcmFormat f)
{
    switch (f) {
        case PcmFormat::u8:  return 1;
        case PcmFormat::i16: return 2;
        case PcmFormat::i24: return 3;
        case PcmFormat::i32: return 4;
        default:             return 4;   // f32
    }
}

// Map a WAV header's (AudioFormat, BitsPerSample) onto a PcmFormat. False for anything not handled
// (f64, ADPCM, a bit depth that does not match its tag, ...) - the caller then rejects the file.
inline bool pcm_format_of(uint16_t audio_format, uint16_t bits, PcmFormat& out)
{
    if (audio_format == 1) {                      // WAVE_FORMAT_PCM, integer
        switch (bits) {
            case 8:  out = PcmFormat::u8;  return true;
            case 16: out = PcmFormat::i16; return true;
            case 24: out = PcmFormat::i24; return true;
            case 32: out = PcmFormat::i32; return true;
            default: return false;
        }
    }
    if (audio_format == 3 && bits == 32) {        // WAVE_FORMAT_IEEE_FLOAT
        out = PcmFormat::f32;
        return true;
    }
    return false;
}

// Decode one sample to normalized float. Little-endian, matching the on-disk WAV order and the
// in-memory layout on this little-endian target. Integer scales are the positive maxima, so the
// endpoints map to exactly +/-1.0 and encode/decode round-trips (see sample16.h).
inline float pcm_read1(const uint8_t* p, PcmFormat f)
{
    switch (f) {
        case PcmFormat::u8:  return (static_cast<float>(p[0]) - 128.f) * (1.f / 127.f);
        case PcmFormat::i16: { int16_t s; std::memcpy(&s, p, 2); return i16_to_float(s); }
        case PcmFormat::i24: {
            // Sign-extend 3 little-endian bytes into an int32 by landing them in the TOP 24 bits and
            // arithmetic-shifting down - no branch on the sign bit.
            const int32_t v = (static_cast<int32_t>(p[0]) << 8) | (static_cast<int32_t>(p[1]) << 16) |
                              (static_cast<int32_t>(p[2]) << 24);
            return static_cast<float>(v >> 8) * (1.f / 8388607.f);
        }
        case PcmFormat::i32: { int32_t s; std::memcpy(&s, p, 4); return static_cast<float>(s) * (1.f / 2147483647.f); }
        default:             { float v;   std::memcpy(&v, p, 4); return v; }   // f32
    }
}

// Encode a normalized float to one sample. Integer targets clamp (a hard clip): the float path
// tolerated values beyond +/-1.0, the integer ones must not wrap.
inline void pcm_write1(uint8_t* p, PcmFormat f, float v)
{
    switch (f) {
        case PcmFormat::u8: {
            const float c = std::clamp(v, -1.f, 1.f);
            p[0] = static_cast<uint8_t>(128 + static_cast<int>(c * 127.f + (c >= 0.f ? 0.5f : -0.5f)));
            break;
        }
        case PcmFormat::i16: { const int16_t s = float_to_i16(v); std::memcpy(p, &s, 2); break; }
        case PcmFormat::i24: {
            const float c = std::clamp(v, -1.f, 1.f);
            const int32_t s = static_cast<int32_t>(c * 8388607.f + (c >= 0.f ? 0.5f : -0.5f));
            p[0] = static_cast<uint8_t>(s & 0xff);
            p[1] = static_cast<uint8_t>((s >> 8) & 0xff);
            p[2] = static_cast<uint8_t>((s >> 16) & 0xff);
            break;
        }
        case PcmFormat::i32: {
            const float c = std::clamp(v, -1.f, 1.f);
            // 2147483647.0 is not representable in float; scale in double so +1.0 does not round up
            // past INT32_MAX and wrap negative.
            const int32_t s = static_cast<int32_t>(static_cast<double>(c) * 2147483647.0);
            std::memcpy(p, &s, 4);
            break;
        }
        default: std::memcpy(p, &v, 4); break;   // f32
    }
}

// Convert `n_samples` interleaved samples (channel layout is irrelevant - each value is independent)
// from `src_fmt` to `dst_fmt`, routed through normalized float. `src` and `dst` must not overlap.
inline void convert_pcm_block(const uint8_t* src, size_t n_samples, PcmFormat src_fmt,
                              uint8_t* dst, PcmFormat dst_fmt)
{
    const uint8_t sb = pcm_bytes(src_fmt), db = pcm_bytes(dst_fmt);
    for (size_t i = 0; i < n_samples; i++) {
        pcm_write1(dst, dst_fmt, pcm_read1(src, src_fmt));
        src += sb;
        dst += db;
    }
}

// Convert `frames` INTERLEAVED frames, adapting both the sample format and the channel count. Channel
// folding: N -> 1 averages every channel (a real downmix, so material present on only one side is kept,
// not dropped), 1 -> N duplicates, and N -> 2 takes the first two. `src` and `dst` must not overlap.
//
// Shared by the streaming adapter (converting_source.h) and the loop-buffer loader (pcm_loader.h) so
// "what a stereo 24-bit file sounds like" has exactly one answer on this device.
inline constexpr uint16_t kPcmMaxChannels = 8;

inline void convert_pcm_frames(const uint8_t* src, size_t frames, PcmFormat src_fmt, uint16_t src_ch,
                               uint8_t* dst, PcmFormat dst_fmt, uint16_t dst_ch)
{
    if (src_ch == dst_ch) {                    // no fold: every sample is independent
        convert_pcm_block(src, frames * src_ch, src_fmt, dst, dst_fmt);
        return;
    }
    const uint8_t sb = pcm_bytes(src_fmt), db = pcm_bytes(dst_fmt);
    for (size_t f = 0; f < frames; f++) {
        float ch[kPcmMaxChannels];
        const uint16_t n_in = src_ch < kPcmMaxChannels ? src_ch : kPcmMaxChannels;
        for (uint16_t c = 0; c < n_in; c++) ch[c] = pcm_read1(src + c * sb, src_fmt);

        if (dst_ch == 1) {
            float sum = 0.f;
            for (uint16_t c = 0; c < n_in; c++) sum += ch[c];
            pcm_write1(dst, dst_fmt, sum / static_cast<float>(n_in));
        }
        else {
            for (uint16_t c = 0; c < dst_ch; c++) {
                // 1 -> N duplicates the single channel; N -> 2 takes the first two.
                pcm_write1(dst + c * db, dst_fmt, ch[(n_in == 1) ? 0 : (c < n_in ? c : n_in - 1)]);
            }
        }
        src += static_cast<size_t>(src_ch) * sb;
        dst += static_cast<size_t>(dst_ch) * db;
    }
}

// Width-only overload kept for the loop-buffer load path, which knows its two formats as byte widths
// (4 = 32-bit IEEE float, 2 = 16-bit PCM) because those are the only two the buffer is ever stored in.
inline void convert_pcm_block(const uint8_t* src, size_t n_samples, int src_bps,
                              uint8_t* dst, int dst_bps)
{
    convert_pcm_block(src, n_samples, src_bps == 4 ? PcmFormat::f32 : PcmFormat::i16,
                      dst, dst_bps == 4 ? PcmFormat::f32 : PcmFormat::i16);
}

}  // namespace spotykach
