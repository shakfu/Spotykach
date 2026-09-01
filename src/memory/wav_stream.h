#pragma once

#include "memory/audio_stream.h"  // IChunkSource / IChunkSink
#include "memory/byte_file.h"
#include "memory/wav.h"
#include "memory/wav_source.h"
#include "memory/pcm_convert.h"

#include <cstdint>
#include <cstring>

namespace spotykach {

// Streaming WAV codec: presents a WAV file on disk to PlayStream/RecordStream as a plain byte
// source/sink (IChunkSource/IChunkSink), so the ring machinery stays format-agnostic. Reuses the
// 44-byte canonical header build/parse in wav.h. The classic streaming-write problem - the final length
// isn't known when recording starts - is handled by writing a placeholder header, streaming the body,
// then seeking back to 0 and patching the size fields on finalize().

// The frame format the streaming engines (tape / shuttle / softcut) work in: mono samples of the
// build's storage width. This is what a file is converted TO, and what recordings are still written
// AS - the read path widened, the write path did not (docs/dev/unified-wav-reader.md).
inline constexpr PcmFormat kStreamFrameFormat = kNativeSampleFormat;
inline constexpr uint16_t  kStreamFrameChannels = 1;

// Reads a WAV body as a byte stream: parse the header up front, seek to the data chunk, then hand out
// body bytes, stopping exactly at DataSize (so trailing chunks past `data` aren't streamed as audio).
// The bytes come out in the FILE's format; a caller whose file is not already in the engine's frame
// format wraps this in a ConvertingSource (see StreamDeck::start_play).
class WavStreamReader : public IChunkSource {
public:
    // Device sample rate: what the ring, the engines and every recorded file are in. An off-rate file
    // is RESAMPLED to it by ConvertingSource (in the main-loop pump), so the engines keep counting
    // frames at this rate whatever the card holds. Matches the rate wav.h writes into recorded headers.
    static constexpr uint32_t kPlaybackSampleRate = 48000;
    // Downmix bound. The decorator can fold any of these to mono; beyond stereo it is only bandwidth,
    // since the ring still receives one mono frame per source frame.
    static constexpr uint16_t kMaxChannels = 8;
    // Sanity bounds on a header's stated rate; the resampler's ratio is derived from it.
    static constexpr uint32_t kRateMin = 4000;
    static constexpr uint32_t kRateMax = 192000;

    // Returns false on a missing/invalid/unsupported header. The chunk walk lives in wav_source.h
    // (shared with wav.h and raw_stream.h); what stays HERE is the engine-capability policy:
    //
    //   depth/channels - any PCM depth this firmware can convert (u8/i16/i24/i32/f32), 1..8 channels.
    //                    Anything that is not already `kStreamFrameFormat` mono is adapted on the way
    //                    to the ring by ConvertingSource, in the main loop.
    //   sample rate    - any rate in kRateMin..kRateMax. An off-rate file is resampled to the device
    //                    rate on the way to the ring, so nothing downstream sees the difference: a
    //                    loop length, a RAM cap and a tempo-synced buffer still count 48 kHz frames.
    //                    The bound exists to refuse a nonsense header rather than divide by it.
    //
    // A reject here becomes the deck's error flash (via start_play), not a mis-play.
    bool begin(IByteFile* f) {
        _f = f; _remaining = 0; _data_start = 0; _data_size = 0;
        _src_fmt = kStreamFrameFormat; _src_ch = kStreamFrameChannels;

        WavInfo info;
        if (!parse_wav(f, info)) return false;                       // parse_wav leaves f at the body

        PcmFormat fmt;
        if (!pcm_format_of(info.audio_format, info.bits_per_sample, fmt)) return false;
        if (info.channels < 1 || info.channels > kMaxChannels)       return false;
        if (info.sample_rate < kRateMin || info.sample_rate > kRateMax) return false;

        _src_fmt    = fmt;
        _src_ch     = info.channels;
        _rate       = info.sample_rate;
        _data_start = info.data_start;
        _data_size  = info.data_size;
        _remaining  = info.data_size;
        return true;
    }

    // What the file holds, for the caller deciding whether a ConvertingSource is needed.
    PcmFormat src_format()      const { return _src_fmt; }
    uint16_t  src_channels()    const { return _src_ch; }
    uint32_t  src_rate()        const { return _rate; }
    uint32_t  src_frame_bytes() const { return static_cast<uint32_t>(pcm_bytes(_src_fmt)) * _src_ch; }

    // Length in SOURCE frames. Depth and channel conversion do not change how many frames there are;
    // RESAMPLING does, so a caller reporting the length an engine will receive passes this through
    // ConvertingSource::out_frames (see StreamDeck::loop_frames).
    uint32_t frames() const { const uint32_t fb = src_frame_bytes(); return fb ? _data_size / fb : 0; }

    uint32_t read(uint8_t* dst, uint32_t n) override {
        if (n > _remaining) n = _remaining;
        const uint32_t got = _f->read(dst, n);
        _remaining -= got;
        return got;
    }
    bool eof() const override { return _remaining == 0; }

    // Loop support: seek back to the data-chunk start and refill the body counter so reading repeats.
    void rewind() override { if (_f && _f->seek(_data_start)) _remaining = _data_size; }

    uint32_t body_remaining() const { return _remaining; }
    uint32_t data_bytes()     const { return _data_size; }   // total body size (loop length, in bytes)

private:
    IByteFile* _f = nullptr;
    PcmFormat  _src_fmt = kStreamFrameFormat;   // the FILE's sample format (not the engine's)
    uint16_t   _src_ch  = kStreamFrameChannels;
    uint32_t   _rate    = kPlaybackSampleRate;  // the FILE's rate (resampled to the device rate)
    uint32_t   _remaining  = 0;  // body bytes not yet read
    uint32_t   _data_start = 0;  // byte offset of the data-chunk body (rewind target)
    uint32_t   _data_size  = 0;  // total body bytes (DataSize)
};

// Writes a WAV body as a byte stream: emit a placeholder header, append body bytes (counting them),
// then on finalize() rewrite the header with the real DataSize/RIFF size.
class WavStreamWriter : public IChunkSink {
public:
    // `f` must be an open, writable file positioned at 0. `channels` is written into the header (and
    // re-used on finalize) - the streaming body is channel-agnostic, but the header must state the
    // truth so the file opens correctly elsewhere (the tape engine records one mono file per deck).
    bool begin(IByteFile* f, uint16_t channels = 2) {
        _f = f; _body = 0; _channels = channels;
        const WavHeader h = wav_header(0, _channels);  // placeholder (DataSize 0) - patched in finalize()
        return _f->write(&h, sizeof(h)) == sizeof(h);
    }

    uint32_t write(const uint8_t* src, uint32_t n) override {
        const uint32_t w = _f->write(src, n);
        _body += w;
        return w;
    }

    void finalize() override {
        const WavHeader h = wav_header(_body, _channels);  // real sizes now known
        if (_f->seek(0)) _f->write(&h, sizeof(h));
        // The concrete file's owner closes it (flush happens there).
    }

    uint32_t body_bytes() const { return _body; }

private:
    IByteFile* _f = nullptr;
    uint32_t   _body = 0;       // body bytes written so far
    uint16_t   _channels = 2;   // header channel count (2 = stereo default, 1 = mono per tape deck)
};

} // namespace spotykach
