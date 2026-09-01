#pragma once

//https://en.wikipedia.org/wiki/WAV

#include <cstdint>
#include <cstddef>
#include <cstring>

#include "engine/wav_cues.h"   // spotykach::WavCues (the platform-owned parsed-marker type)
#include "memory/wav_source.h"  // the shared RIFF chunk walk (spotykach::parse_wav / WavInfo)
#include "memory/pcm_convert.h" // PcmFormat (the native storage format, derived from kWav* below)

struct WavHeader {
  // Master RIFF chunk
  uint8_t FileTypeBlocID[4] = {'R', 'I', 'F', 'F'};
  uint32_t size;  // RIFF chunk size: a 32-bit field per the WAV spec (was size_t,
                  // which is 4 bytes only on the 32-bit target and breaks the
                  // 44-byte layout / static_assert on a 64-bit host).
  uint8_t FileFormatID[4] = {'W', 'A', 'V', 'E'};
  
  // Chunk describing the data format
  uint8_t FormatBlocID[4] = {'f', 'm', 't', ' '};
  uint32_t BlocSize = 16; // Fixed
  uint16_t AudioFormat;   // 1 = PCM integer. 3=IEEE754 float.
  uint16_t NbrChannels;
  uint32_t SampleRate;
  uint32_t BytePerSec;
  uint16_t BytePerBloc;
  uint16_t BitsPerSample;
  // Chunk containing the sampled data
  uint8_t DataBlocID[4] = {'d', 'a', 't', 'a'};
  uint32_t DataSize;
};

// Sample storage format for the loop buffer, reflected in the WAV header. Default is
// 32-bit IEEE-754 float (AudioFormat 3); LOFI_INT16 selects 16-bit PCM (AudioFormat 1).
// This MUST match Buffer::Frame's actual element type - see docs/lofi-int16-scope.md.
#if LOFI_INT16
static constexpr uint16_t kWavAudioFormat    = 1;  // PCM integer
static constexpr uint16_t kWavBitsPerSample  = 16;
static constexpr uint16_t kWavBytesPerSample = 2;
#else
static constexpr uint16_t kWavAudioFormat    = 3;  // IEEE-754 float
static constexpr uint16_t kWavBitsPerSample  = 32;
static constexpr uint16_t kWavBytesPerSample = 4;
#endif

// The same storage choice as a PcmFormat: what the loop buffer holds, what the decks record, and what
// an incoming file is converted TO. One definition so the load path and the streaming path cannot
// disagree about what "native" means.
inline constexpr spotykach::PcmFormat kNativeSampleFormat =
    (kWavAudioFormat == 3) ? spotykach::PcmFormat::f32 : spotykach::PcmFormat::i16;

inline WavHeader wav_header(const size_t size, const uint16_t channels = 2) {
    WavHeader header;
    static_assert(sizeof(header) == 44, "");

    header.AudioFormat = kWavAudioFormat;
    header.NbrChannels = channels;
    header.SampleRate = 48000;
    header.BytePerBloc = kWavBytesPerSample * header.NbrChannels;
    header.BytePerSec = header.SampleRate * header.BytePerBloc;
    header.BitsPerSample = kWavBitsPerSample;

    header.DataSize = size;
    header.size = header.DataSize + sizeof(header) - 8;

    return header;
};

template <typename T>
inline T read_val(const uint8_t* data, size_t offset) 
{
    T value;
    std::memcpy(&value, data + offset, sizeof(T));
    return value;
}
inline bool check_id(const uint8_t* data, size_t offset, const char* id) 
{
  return std::memcmp(data + offset, id, 4) == 0;
}

// Parse a WAV header out of an in-memory copy of the file head into the canonical 44-byte WavHeader
// layout, reporting the data-chunk BODY offset in `header_size` (what the caller seeks to before
// streaming the samples). A thin adapter over the shared chunk walk in wav_source.h - the walk, the
// bounds checks and the EXTENSIBLE unwrapping all live there; this only re-shapes the result. The
// chunk IDs are the struct's defaults: reaching a `fmt `/`data` at all is what proves they were seen.
inline bool wav_header(const uint8_t* bytes, size_t size, WavHeader& header, size_t& header_size)
{
    spotykach::WavInfo info;
    if (!spotykach::parse_wav(bytes, static_cast<uint32_t>(size), info)) return false;

    header.size          = read_val<uint32_t>(bytes, 4);   // the RIFF chunk size, as stated
    header.BlocSize      = info.fmt_size;
    header.AudioFormat   = info.audio_format;
    header.NbrChannels   = info.channels;
    header.SampleRate    = info.sample_rate;
    header.BytePerSec    = info.byte_per_sec;
    header.BytePerBloc   = info.byte_per_bloc;
    header.BitsPerSample = info.bits_per_sample;
    header.DataSize      = info.data_size;
    header_size          = info.data_start;
    return true;
}

// Scan a WAV byte buffer for the `cue ` chunk and fill `out` with the cue markers' sample-frame
// offsets, keeping only those inside `frame_limit` (markers past the audio end are dropped) and
// clamped to WavCues::kMax. Pure and host-testable: no engine, hardware, or allocation deps. `size`
// is the number of valid bytes in `bytes`. Order-independent (the `cue ` chunk may precede or follow
// `data`). All reads are bounds-checked - a malformed/truncated chunk yields whatever was parsed so
// far rather than an over-read.
//
// A WAV `cue ` chunk body is: uint32 numCuePoints, then numCuePoints * 24-byte cue points; each cue
// point carries its dwSampleOffset (the sample-frame position) at byte offset 20 within the point.
inline void find_cue_points(const uint8_t* bytes, size_t size, size_t frame_limit, spotykach::WavCues& out)
{
    out.count = 0;
    if (size < 12 || !check_id(bytes, 0, "RIFF")) return;

    size_t cursor = 12;   // past RIFF + riffSize + WAVE
    while (cursor + 8 <= size) {
        char chunkID[4];
        std::memcpy(chunkID, bytes + cursor, 4);
        uint32_t chunkSize = read_val<uint32_t>(bytes, cursor + 4);
        size_t body = cursor + 8;

        if (std::memcmp(chunkID, "cue ", 4) == 0 && body + 4 <= size) {
            uint32_t n = read_val<uint32_t>(bytes, body);
            for (uint32_t i = 0; i < n && out.count < spotykach::WavCues::kMax; ++i) {
                size_t off = body + 4 + static_cast<size_t>(i) * 24 + 20; // dwSampleOffset within point
                if (off + 4 > size) break;
                uint32_t frame = read_val<uint32_t>(bytes, off);
                if (frame < frame_limit) out.frames[out.count++] = frame;
            }
            return;
        }

        cursor = body + chunkSize + (chunkSize & 1u);   // chunks are word-aligned
    }
}
