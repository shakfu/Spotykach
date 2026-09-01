#pragma once
// Pure accounting for streaming an audio file body into the loop buffer, converting the samples when
// the file's format differs from the buffer's storage. The FatFS I/O stays in the caller (card.cpp);
// this holds the size/offset/frame/termination math so the exact logic that ships can be host-tested.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>
#include "pcm_convert.h"

namespace spotykach {

class PcmLoader {
public:
    // file_data_bytes: WAV DataSize. src_fmt/src_ch: what the file holds. dst + dst_capacity_bytes:
    // the destination loop buffer, in dst_fmt/dst_ch frames. Loads min(file, capacity) frames.
    void begin(size_t file_data_bytes, PcmFormat src_fmt, uint16_t src_ch,
               uint8_t* dst, size_t dst_capacity_bytes, PcmFormat dst_fmt, uint16_t dst_ch,
               uint32_t src_rate = 0, uint32_t dst_rate = 0)
    {
        _src_fmt = src_fmt; _dst_fmt = dst_fmt;
        _src_ch  = src_ch;  _dst_ch  = dst_ch;
        _src_frame = static_cast<size_t>(pcm_bytes(src_fmt)) * src_ch;
        _dst_frame = static_cast<size_t>(pcm_bytes(dst_fmt)) * dst_ch;
        _resample  = (src_rate && dst_rate && src_rate != dst_rate);
        _step      = _resample ? static_cast<double>(src_rate) / static_cast<double>(dst_rate) : 1.0;
        // Identical formats AND no rate change: a straight memcpy, no conversion at all.
        _copy = (src_fmt == dst_fmt && src_ch == dst_ch && !_resample);
        _dst  = dst;

        size_t file_frames = _src_frame ? file_data_bytes / _src_frame : 0;
        if (_resample) {   // the buffer is filled at the DEVICE rate, so the source's length converts
            file_frames = static_cast<size_t>(static_cast<uint64_t>(file_frames) * dst_rate / src_rate);
        }
        const size_t cap_frames = _dst_frame ? dst_capacity_bytes / _dst_frame : 0;
        _size    = std::min(file_frames, cap_frames) * _dst_frame;
        _offset  = 0;
        _carry_n = 0;
        _primed  = false;
        _phase   = 0.0;
    }

    // Width-only form for a caller that knows both sides as byte widths (4 = 32-bit float, 2 = 16-bit
    // PCM) and is stereo on both sides - the loop buffer's original shape.
    void begin(size_t file_data_bytes, int file_bps, uint8_t* dst, size_t dst_capacity_bytes, int dst_bps)
    {
        begin(file_data_bytes, file_bps == 4 ? PcmFormat::f32 : PcmFormat::i16, 2,
              dst, dst_capacity_bytes, dst_bps == 4 ? PcmFormat::f32 : PcmFormat::i16, 2, 0, 0);
    }

    // Consume one chunk of file bytes (already read into `chunk`). Writes converted/copied frames to
    // the destination at the current offset, up to remaining capacity. Returns true once the
    // destination is full (the caller also stops on end-of-file).
    //
    // Frames can STRADDLE chunks: the caller reads fixed-size blocks off the card, and a 3- or 6-byte
    // frame does not divide them. A partial trailing frame is carried into the next call rather than
    // converted as if it were whole - which would smear every subsequent sample by a byte or two.
    bool feed(const uint8_t* chunk, size_t chunk_bytes)
    {
        if (_copy) {                                        // identical formats: byte-for-byte
            const size_t len = std::min(_size - _offset, chunk_bytes);
            std::memcpy(_dst + _offset, chunk, len);
            _offset += len;
            return _offset >= _size;
        }

        while (chunk_bytes && _offset < _size) {
            if (_carry_n || chunk_bytes < _src_frame) {     // finish (or start) a straddling frame
                const size_t take = std::min(_src_frame - _carry_n, chunk_bytes);
                std::memcpy(_carry + _carry_n, chunk, take);
                _carry_n    += take;
                chunk       += take;
                chunk_bytes -= take;
                if (_carry_n < _src_frame) break;           // still partial: wait for the next chunk
                _emit(_carry, 1);
                _carry_n = 0;
            }
            else if (_resample) {
                _emit_resampled(chunk, 1);          // one source frame at a time; it may yield 0..n out
                chunk       += _src_frame;
                chunk_bytes -= _src_frame;
            }
            else {
                size_t frames = chunk_bytes / _src_frame;
                // _size is a whole number of destination frames and _offset advances a frame at a
                // time, so _offset < _size means at least one full frame of room.
                const size_t room = (_size - _offset) / _dst_frame;
                if (frames > room) frames = room;
                if (!frames) break;
                _emit(chunk, frames);
                chunk       += frames * _src_frame;
                chunk_bytes -= frames * _src_frame;
            }
        }
        return _offset >= _size;
    }

    size_t offset() const { return _offset; }
    size_t size_bytes() const { return _size; }
    size_t frames() const { return _dst_frame ? _offset / _dst_frame : 0; }

private:
    void _emit(const uint8_t* src, size_t frames)
    {
        convert_pcm_frames(src, frames, _src_fmt, _src_ch, _dst + _offset, _dst_fmt, _dst_ch);
        _offset += frames * _dst_frame;
    }

    // Push-driven linear resampler: each call hands over ONE source frame, which may produce zero,
    // one or several output frames depending on the ratio. `_s0`/`_s1` straddle the output position
    // and `_phase` is the fraction between them, both carried across chunks - so a chunk boundary is
    // not a discontinuity. Mirrors ConvertingSource's pull-side resampler; the arithmetic is the same,
    // only the direction of control differs.
    void _emit_resampled(const uint8_t* src, size_t /*frames*/)
    {
        float in[kPcmMaxChannels];
        {
            uint8_t tmp[kPcmMaxChannels * 4];
            convert_pcm_frames(src, 1, _src_fmt, _src_ch, tmp, PcmFormat::f32, _dst_ch);
            std::memcpy(in, tmp, static_cast<size_t>(_dst_ch) * sizeof(float));
        }
        if (!_primed) {                              // the first frame only seeds the interpolator
            std::memcpy(_s0, in, sizeof(_s0));
            std::memcpy(_s1, in, sizeof(_s1));
            _primed = true;
            _phase  = 0.0;
            return;
        }
        std::memcpy(_s0, _s1, sizeof(_s0));
        std::memcpy(_s1, in, sizeof(_s1));
        while (_phase < 1.0 && _offset + _dst_frame <= _size) {
            const float f = static_cast<float>(_phase);
            for (uint16_t c = 0; c < _dst_ch; c++) {
                pcm_write1(_dst + _offset + c * pcm_bytes(_dst_fmt), _dst_fmt, _s0[c] + (_s1[c] - _s0[c]) * f);
            }
            _offset += _dst_frame;
            _phase  += _step;
        }
        // One source frame consumed, so every pending output position shifts down by one. NOT clamped:
        // at a ratio above 2 the phase legitimately stays >= 1 for several frames in a row, which is
        // exactly how those frames get skipped. Clamping here would emit every source frame instead.
        _phase -= 1.0;
    }

    uint8_t*  _dst     = nullptr;
    size_t    _size    = 0;
    size_t    _offset  = 0;
    size_t    _src_frame = 8, _dst_frame = 8;
    PcmFormat _src_fmt = PcmFormat::f32, _dst_fmt = PcmFormat::f32;
    uint16_t  _src_ch = 2, _dst_ch = 2;
    bool      _copy   = true;
    bool      _resample = false;
    double    _step   = 1.0;                 // source frames advanced per output frame
    double    _phase  = 0.0;
    bool      _primed = false;
    float     _s0[kPcmMaxChannels] = { 0.f };
    float     _s1[kPcmMaxChannels] = { 0.f };
    uint8_t   _carry[kPcmMaxChannels * 4];   // one source frame at most (8 channels x 4 bytes)
    uint8_t   _carry_n = 0;
};

}  // namespace spotykach
