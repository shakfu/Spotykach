#pragma once

#include "memory/audio_stream.h"  // IChunkSource
#include "memory/byte_file.h"
#include "memory/wav_source.h"
#include "memory/pcm_convert.h"

#include <cstdint>
#include <cstring>

namespace spotykach {

// Streaming reader for a 16-bit signed mono PCM body, little-endian, used by the radio engine. It serves
// two layouts behind one int16 streaming path:
//   - begin()     : HEADERLESS raw (the RadioMusic ".raw" format) - the whole file is the body, so the
//                   length is filesize/2 and the body starts at byte 0.
//   - begin_wav() : a 16-bit-mono PCM ".wav" - the header is parsed (chunk walk) to find where the body
//                   starts and how long it is, and the file's own sample rate is reported out.
// Either way the body bytes are int16 mono, so the engine converts them the same way; only the body
// offset differs (raw = 0, wav = past the header). seek_to_frame()/rewind() are relative to that offset,
// so the free-running-playhead jump and looping work identically for both. Endianness: byte-oriented,
// and device (STM32) + host are little-endian, matching the format, so bytes pass straight through.
// The frame format the scanned-bank engines (radio, bard, pstretch) consume: int16 mono, the format a
// headerless `.raw` body IS. A `.wav` in another shape is converted to this on the way to the ring.
inline constexpr PcmFormat kRawFrameFormat   = PcmFormat::i16;
inline constexpr uint16_t  kRawFrameChannels = 1;

class RawStreamReader : public IChunkSource {
public:
    static constexpr uint32_t kBytesPerFrame = 2;  // a RAW frame: signed 16-bit, mono
    static constexpr uint16_t kMaxChannels   = kPcmMaxChannels;

    // Headerless raw. `filesize` is the total byte length (from f_stat). Body = the whole file. A raw
    // file states nothing about itself, so its format is fixed by convention - int16 mono, always.
    bool begin(IByteFile* f, uint32_t filesize) {
        _f = f;
        _src_fmt = kRawFrameFormat; _src_ch = kRawFrameChannels; _src_frame = kBytesPerFrame;
        _data_start = 0;
        _data_size  = filesize & ~(kBytesPerFrame - 1u);   // floor to a whole frame
        _remaining  = _data_size;
        return _f != nullptr && _data_size >= kBytesPerFrame;
    }

    // A PCM WAV. Parses the header through the shared chunk walk (wav_source.h), accepts any depth this
    // firmware can decode (u8/i16/i24/i32/f32) and 1..8 channels, sets the body offset+length, and
    // reports the file's sample rate in `out_rate`. Returns false on a missing/invalid/undecodable
    // header (the caller then treats it as a non-station). `filesize` clamps the data chunk to what is
    // actually present (a truncated file).
    //
    // Unlike the raw path, the body here is NOT necessarily int16 mono - see src_format()/src_channels().
    // A caller whose file is not already in that shape wraps this in a ConvertingSource; every offset
    // and length below stays in SOURCE frames either way, so seeking is unaffected by the conversion.
    // Any RATE is accepted, as before: these engines resample by the file's own rate.
    bool begin_wav(IByteFile* f, uint32_t filesize, uint32_t& out_rate) {
        _f = f; _data_start = 0; _data_size = 0; _remaining = 0; out_rate = 0;
        _src_fmt = kRawFrameFormat; _src_ch = kRawFrameChannels; _src_frame = kBytesPerFrame;

        WavInfo info;
        if (!parse_wav(f, info)) return false;                              // leaves f at the body

        PcmFormat fmt;
        if (!pcm_format_of(info.audio_format, info.bits_per_sample, fmt)) return false;
        if (info.channels < 1 || info.channels > kMaxChannels)              return false;
        const uint32_t frame = static_cast<uint32_t>(pcm_bytes(fmt)) * info.channels;

        uint32_t ds = info.data_size;
        if (info.data_start + ds > filesize) {                              // truncated file
            ds = (filesize > info.data_start) ? (filesize - info.data_start) : 0;
        }
        ds -= ds % frame;                                                   // floor to a whole frame
        if (ds < frame) return false;

        _src_fmt = fmt; _src_ch = info.channels; _src_frame = frame;
        _data_start = info.data_start; _data_size = ds; _remaining = ds; out_rate = info.sample_rate;
        return true;
    }

    // What the body holds, for the caller deciding whether a ConvertingSource is needed.
    PcmFormat src_format()      const { return _src_fmt; }
    uint16_t  src_channels()    const { return _src_ch; }
    uint32_t  src_frame_bytes() const { return _src_frame; }

    uint32_t read(uint8_t* dst, uint32_t n) override {
        if (n > _remaining) n = _remaining;
        const uint32_t got = _f->read(dst, n);
        _remaining -= got;
        return got;
    }
    bool eof() const override { return _remaining == 0; }

    // Loop support: seek back to the body start so a looping station repeats seamlessly.
    void rewind() override { if (_f && _f->seek(_data_start)) _remaining = _data_size; }

    // Seek to source frame `frame` (clamped into the body); subsequent reads continue from there. The
    // radio's free-running playhead calls this on a station change before streaming forward. The byte
    // offset is computed in 64 bits: a wide frame (stereo f32 is 8 bytes) times a frame index near the
    // 4 GB file ceiling overflows a uint32 product, which would wrap the seek back into the file.
    bool seek_to_frame(uint32_t frame) {
        uint64_t off = static_cast<uint64_t>(frame) * _src_frame;
        if (off > _data_size) off = _data_size;           // clamp; an empty tail just reads as eof
        if (!_f || !_f->seek(_data_start + static_cast<uint32_t>(off))) return false;
        _remaining = _data_size - static_cast<uint32_t>(off);
        return true;
    }

    // Length in SOURCE frames - the count the engine ends up with, since conversion changes a frame's
    // width and channel count but never how many frames there are.
    uint32_t frames()     const { return _src_frame ? _data_size / _src_frame : 0; }
    uint32_t data_bytes() const { return _data_size; }

private:
    IByteFile* _f = nullptr;
    PcmFormat  _src_fmt   = kRawFrameFormat;      // the BODY's sample format (raw is always int16 mono)
    uint16_t   _src_ch    = kRawFrameChannels;
    uint32_t   _src_frame = kBytesPerFrame;       // bytes per source frame
    uint32_t   _remaining  = 0;   // body bytes not yet read
    uint32_t   _data_start = 0;   // byte offset of the body (0 raw, past the header for wav)
    uint32_t   _data_size  = 0;   // whole-frame-floored body length, in bytes
};

} // namespace spotykach
