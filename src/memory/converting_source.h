#pragma once

// Format adaptation for the streaming play path: wraps any IChunkSource whose bytes are in the FILE's
// sample format and presents the same audio in the ENGINE's frame format, so the engines keep receiving
// exactly the frames they receive today while the card is allowed to hold something else.
//
// Depth, channel count, and - optionally - sample RATE. Resampling here rather than in each engine's
// playhead is what keeps the rate change invisible downstream: the ring receives frames at the DEVICE
// rate, so a loop length, a RAM cap and a tempo-synced buffer all still count 48 kHz frames whatever
// the file held. `StreamDeck::loop_frames` reports the OUTPUT count for the same reason. The scanned
// engines (radio/bard/pstretch) do NOT use this - they rebase pitch from the file's own header rate
// themselves, and resampling here too would correct it twice. See docs/dev/unified-wav-reader.md.
//
// Main-loop only, like every other IChunkSource: PlayStream::pump drives this while the audio ISR
// touches nothing but the lock-free ring. A file already in the engine's format should bypass the
// decorator entirely (see `is_identity`) so the common case keeps its straight memcpy path.

#include "memory/audio_stream.h"   // IChunkSource
#include "memory/pcm_convert.h"

#include <cstdint>
#include <cstring>

namespace spotykach {

class ConvertingSource : public IChunkSource {
public:
    // Staging for partially-read source frames. A source (FatFs behind a ring) may hand back a byte
    // count that ends mid-frame, so the tail has to be carried to the next read rather than converted
    // as if it were whole. Sized in whole source frames with room to spare: the widest frame in play is
    // 8 bytes (stereo f32/i32), so this is 64+ frames of carry and one memmove per pump at worst.
    static constexpr uint32_t kStageBytes = 512;

    // True when `src` needs no adaptation at all - identical depth, channel count AND rate. The caller
    // should then hand the raw source straight to PlayStream instead of wrapping it, which is what
    // keeps a native file on its exact byte-for-byte memcpy path.
    static bool is_identity(PcmFormat src_fmt, uint16_t src_ch, PcmFormat dst_fmt, uint16_t dst_ch,
                            uint32_t src_rate = 0, uint32_t dst_rate = 0) {
        return src_fmt == dst_fmt && src_ch == dst_ch && src_rate == dst_rate;
    }

    // Output frames produced from `src_frames` at the configured rates - what a caller reports as the
    // playing file's length, since that is what the engine will actually receive.
    static uint32_t out_frames(uint32_t src_frames, uint32_t src_rate, uint32_t dst_rate) {
        if (!src_rate || src_rate == dst_rate) return src_frames;
        return static_cast<uint32_t>(static_cast<uint64_t>(src_frames) * dst_rate / src_rate);
    }

    // `src` supplies file-format bytes; reads from this object come back as `dst_fmt`/`dst_ch` frames.
    // Channel folding: N -> 1 averages every channel (a real downmix, not a channel drop), 1 -> N
    // duplicates, and N -> 2 takes the first two. Returns false for a shape it cannot serve.
    // Pass equal rates (or leave them 0) for no resampling. A non-zero, differing pair turns on the
    // linear-interpolating resampler - the same interpolation the engines' own varispeed playheads use,
    // so this is not a quality step down from rebasing a playhead; it just happens one layer earlier.
    bool begin(IChunkSource* src, PcmFormat src_fmt, uint16_t src_ch, PcmFormat dst_fmt, uint16_t dst_ch,
               uint32_t src_rate = 0, uint32_t dst_rate = 0) {
        if (!src || src_ch == 0 || src_ch > kMaxChannels || dst_ch == 0 || dst_ch > kMaxChannels) return false;
        _src = src; _src_fmt = src_fmt; _dst_fmt = dst_fmt; _src_ch = src_ch; _dst_ch = dst_ch;
        _src_frame = static_cast<uint32_t>(pcm_bytes(src_fmt)) * src_ch;
        _dst_frame = static_cast<uint32_t>(pcm_bytes(dst_fmt)) * dst_ch;
        _resample  = (src_rate && dst_rate && src_rate != dst_rate);
        _step      = _resample ? static_cast<double>(src_rate) / static_cast<double>(dst_rate) : 1.0;
        _have = 0;
        _primed = false;
        _phase = 0.0;
        return _src_frame <= kStageBytes;
    }

    // Drop any carried partial frame. Call after seeking the underlying source, or the first frame out
    // is stitched from bytes either side of the seek.
    void reset_carry() { _have = 0; _primed = false; _phase = 0.0; }

    uint32_t src_frame_bytes() const { return _src_frame; }
    uint32_t dst_frame_bytes() const { return _dst_frame; }

    // Convert source frames to `n` destination bytes' worth. Emits WHOLE destination frames only, so a
    // caller that always asks for a frame-aligned `n` stays frame-aligned; a short return means the
    // source is momentarily dry or at EOF (`eof()` distinguishes them), exactly as IChunkSource says.
    uint32_t read(uint8_t* dst, uint32_t n) override {
        if (!_src) return 0;
        if (_resample) return _read_resampled(dst, n);
        uint32_t out = 0;
        while (out + _dst_frame <= n) {
            if (_have < _src_frame) {                       // top up: we cannot convert a partial frame
                // Keep pulling until a whole frame is staged. A SHORT read does not mean "stop" - a
                // chunked source (FatFs, a ring) can return fewer bytes than asked without being at
                // EOF - so giving up on the first short read would stall the stream whenever a source
                // hands back less than one frame at a time. Only a zero-byte read ends the round.
                while (_have < _src_frame) {
                    const uint32_t got = _src->read(_stage + _have, kStageBytes - _have);
                    if (!got) break;                        // dry or EOF: keep the carry for next time
                    _have += got;
                }
                if (_have < _src_frame) break;
            }
            uint32_t frames = _have / _src_frame;
            const uint32_t room = (n - out) / _dst_frame;
            if (frames > room) frames = room;

            _convert(_stage, frames, dst + out);
            out += frames * _dst_frame;

            const uint32_t used = frames * _src_frame;
            _have -= used;
            if (_have) std::memmove(_stage, _stage + used, _have);   // carry the partial tail
        }
        return out;
    }

    // Only truly done once the source is exhausted AND no whole frame remains staged. A trailing
    // partial frame (a file truncated mid-sample) is dropped, not emitted as a half-sample. When
    // resampling, the one frame held in `_s1` can still produce output, so it counts as not-done.
    bool eof() const override {
        if (!_src) return true;
        if (!(_src->eof() && _have < _src_frame)) return false;
        return !(_resample && _primed && _phase < 1.0);
    }

    void rewind() override { if (_src) { _src->rewind(); reset_carry(); } }

private:
    static constexpr uint16_t kMaxChannels = kPcmMaxChannels;

    void _convert(const uint8_t* src, uint32_t frames, uint8_t* dst) const {
        convert_pcm_frames(src, frames, _src_fmt, _src_ch, dst, _dst_fmt, _dst_ch);
    }

    // Stage one source frame into `out` as destination-channel floats (depth + fold applied), or
    // return false if no whole source frame can be had right now.
    bool _next_frame(float* out) {
        while (_have < _src_frame) {
            const uint32_t got = _src->read(_stage + _have, kStageBytes - _have);
            if (!got) return false;                     // dry or EOF: keep the carry for next time
            _have += got;
        }
        uint8_t tmp[kMaxChannels * 4];
        convert_pcm_frames(_stage, 1, _src_fmt, _src_ch, tmp, PcmFormat::f32, _dst_ch);
        std::memcpy(out, tmp, static_cast<size_t>(_dst_ch) * sizeof(float));
        _have -= _src_frame;
        if (_have) std::memmove(_stage, _stage + _src_frame, _have);
        return true;
    }

    // Linear-interpolating resampler over the converted frames. `_s0`/`_s1` are the two source frames
    // straddling the output position and `_phase` is the fraction between them, all carried across
    // calls - so a chunk boundary is not a discontinuity. At end-of-source the final frame is emitted
    // once and then eof() goes true, rather than interpolating against silence.
    uint32_t _read_resampled(uint8_t* dst, uint32_t n) {
        uint32_t out = 0;
        while (out + _dst_frame <= n) {
            if (!_primed) {                              // first two frames seed the interpolator
                if (!_next_frame(_s0)) break;
                if (!_next_frame(_s1)) { std::memcpy(_s1, _s0, sizeof(_s1)); }
                _primed = true;
                _phase  = 0.0;
            }
            while (_phase >= 1.0) {                      // advance past whole source frames
                std::memcpy(_s0, _s1, sizeof(_s0));
                if (!_next_frame(_s1)) return out;       // source dry: stop here, keep the phase
                _phase -= 1.0;
            }
            const float f = static_cast<float>(_phase);
            for (uint16_t c = 0; c < _dst_ch; c++) {
                pcm_write1(dst + out + c * pcm_bytes(_dst_fmt), _dst_fmt, _s0[c] + (_s1[c] - _s0[c]) * f);
            }
            out   += _dst_frame;
            _phase += _step;
        }
        return out;
    }

    IChunkSource* _src = nullptr;
    PcmFormat _src_fmt = PcmFormat::f32, _dst_fmt = PcmFormat::f32;
    uint16_t  _src_ch = 1, _dst_ch = 1;
    uint32_t  _src_frame = 4, _dst_frame = 4;
    uint32_t  _have = 0;                    // staged source bytes not yet converted
    bool      _resample = false;
    double    _step  = 1.0;                 // source frames advanced per output frame
    double    _phase = 0.0;                 // fraction between _s0 and _s1
    bool      _primed = false;
    float     _s0[kMaxChannels] = { 0.f };  // the two source frames the output position straddles
    float     _s1[kMaxChannels] = { 0.f };
    uint8_t   _stage[kStageBytes];
};

} // namespace spotykach
