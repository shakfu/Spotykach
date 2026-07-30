#pragma once

#include <cstddef>
#include <cstdint>

// A small mono room for the `bard` engine's Grit layer - the space a voice is read in.
//
// LICENSING NOTE, and the reason this file exists at all: the obvious candidate was the existing
// `src/dsp/diffuser.h`, but that file is **GPLv3** (a port of qdelay's Diffusor), and linking it would
// relicense the whole bard engine away from the repository's MIT. So this is written from the classic
// published reverberator structure instead - Schroeder's parallel comb bank followed by series allpass
// diffusers (Schroeder, "Natural Sounding Artificial Reverberation", JAES 1962; Moorer, "About This
// Reverberation Business", Computer Music Journal 1979). Textbook topology, no code, coefficient tables
// or size mapping taken from any GPL source, so bard stays MIT.
//
// Deliberately modest: four parallel feedback combs (mutually prime lengths, so their comb peaks do not
// line up into a metallic ring) into two series allpasses, plus a damping one-pole in each comb's
// feedback path so the tail loses treble the way a real room does. One mono instance per deck; the
// stereo placement is the routing switch's job, not the room's.
//
// Memory comes from the caller (the SDRAM arena on target), sized by capacity_floats(). Access is
// sequential per delay line - one read and one write at monotonically advancing indices - which is the
// pattern SDRAM handles well, unlike the scattered access that made pstretch's FFT unusable there.

namespace spotykach {
namespace bard {

class Room {
public:
    enum class Character : uint8_t { Plate, Hall, Slap };

    static constexpr int kCombs     = 4;
    static constexpr int kAllpasses = 2;

    // Comb lengths in samples at 48 kHz, mutually prime. Plate = short and dense; Hall = the same set
    // scaled up for a longer, more spacious tail. Slap uses only the first comb, as a single tap.
    static constexpr int kPlate[kCombs] = { 1279, 1481, 1693, 1889 };
    static constexpr int kHall[kCombs]  = { 2477, 2801, 3169, 3491 };
    static constexpr int kSlap          = 9601;    // ~200 ms at 48 kHz
    static constexpr int kAp[kAllpasses] = { 557, 359 };

    // Floats one instance needs at sample rate `sr`. Sized for the LONGEST character so switching
    // plate/hall/slap live never reallocates - each line just uses a prefix of its buffer.
    static size_t capacity_floats(float sr) {
        const float k = sr / 48000.f;
        size_t n = 0;
        for (int i = 0; i < kCombs; i++) {
            const int longest = (kHall[i] > kPlate[i]) ? kHall[i] : kPlate[i];
            n += static_cast<size_t>(longest * k) + 4u;
        }
        n += static_cast<size_t>(kSlap * k) + 4u;                       // the slap tap has its own line
        for (int i = 0; i < kAllpasses; i++) n += static_cast<size_t>(kAp[i] * k) + 4u;
        return n;
    }

    // Carve the delay lines out of `mem` (>= capacity_floats(sr) floats). `mem` may be null - the arena
    // can be exhausted or absent (the host test passes none), and process() then passes audio through
    // untouched rather than faulting.
    void init(float* mem, float sr) {
        _active = (mem != nullptr);
        _sr = sr > 0.f ? sr : 48000.f;
        if (!_active) return;
        const float k = _sr / 48000.f;
        float* p = mem;
        for (int i = 0; i < kCombs; i++) {
            const int longest = (kHall[i] > kPlate[i]) ? kHall[i] : kPlate[i];
            const int cap = static_cast<int>(longest * k) + 4;
            _comb[i].init(p, cap); p += cap;
        }
        {
            const int cap = static_cast<int>(kSlap * k) + 4;
            _slap.init(p, cap); p += cap;
        }
        for (int i = 0; i < kAllpasses; i++) {
            const int cap = static_cast<int>(kAp[i] * k) + 4;
            _allpass[i].init(p, cap); p += cap;
        }
        set_character(Character::Plate);
    }

    void set_character(Character c) {
        _character = c;
        if (!_active) return;
        const float k = _sr / 48000.f;
        const int* len = (c == Character::Hall) ? kHall : kPlate;
        for (int i = 0; i < kCombs; i++) _comb[i].set_length(static_cast<int>(len[i] * k));
        for (int i = 0; i < kAllpasses; i++) _allpass[i].set_length(static_cast<int>(kAp[i] * k));
        _slap.set_length(static_cast<int>(kSlap * k));
        clear();
    }

    Character character() const { return _character; }

    // 0..1 -> decay. Plate/Hall map it to comb feedback (roughly 0.4 s .. 4 s of tail); Slap maps it to
    // the echo's regeneration. Capped below 1.0 so the network cannot self-oscillate at the extreme.
    void set_size(float size01) {
        const float s = size01 < 0.f ? 0.f : (size01 > 1.f ? 1.f : size01);
        _feedback = 0.70f + 0.28f * s;
        _damp     = 0.35f - 0.25f * s;       // bigger room -> less treble loss per pass
    }

    void clear() {
        for (int i = 0; i < kCombs; i++) _comb[i].clear();
        for (int i = 0; i < kAllpasses; i++) _allpass[i].clear();
        _slap.clear();
    }

    // Wet-only: the caller owns the dry/wet mix (bard's GritMix).
    inline float process(float in) {
        if (!_active) return in;
        if (_character == Character::Slap) {
            const float tap = _slap.peek();
            _slap.push(in + tap * (_feedback - 0.25f));
            return tap;
        }
        float acc = 0.f;
        for (int i = 0; i < kCombs; i++) acc += _comb[i].process(in, _feedback, _damp);
        acc *= 0.25f;
        for (int i = 0; i < kAllpasses; i++) acc = _allpass[i].process(acc, 0.5f);
        return acc;
    }

private:
    // A feedback comb with a one-pole lowpass in the loop (the "damped comb" of Schroeder/Moorer).
    struct Comb {
        float* buf = nullptr;
        int    cap = 0, len = 1, pos = 0;
        float  store = 0.f;

        void init(float* mem, int c) { buf = mem; cap = c < 1 ? 1 : c; len = cap; pos = 0; clear(); }
        void set_length(int n) { len = n < 1 ? 1 : (n > cap ? cap : n); pos = 0; }
        void clear() { if (buf) for (int i = 0; i < cap; i++) buf[i] = 0.f; pos = 0; store = 0.f; }

        inline float process(float in, float fb, float damp) {
            const float out = buf[pos];
            store = out * (1.f - damp) + store * damp;      // damping one-pole in the feedback path
            buf[pos] = in + store * fb;
            if (++pos >= len) pos = 0;
            return out;
        }
    };

    // A Schroeder allpass: out = -in*g + buffered; buffer <- in + out*g.
    struct AllPass {
        float* buf = nullptr;
        int    cap = 0, len = 1, pos = 0;

        void init(float* mem, int c) { buf = mem; cap = c < 1 ? 1 : c; len = cap; pos = 0; clear(); }
        void set_length(int n) { len = n < 1 ? 1 : (n > cap ? cap : n); pos = 0; }
        void clear() { if (buf) for (int i = 0; i < cap; i++) buf[i] = 0.f; pos = 0; }

        inline float process(float in, float g) {
            const float buffered = buf[pos];
            const float out = buffered - in * g;
            buf[pos] = in + out * g;
            if (++pos >= len) pos = 0;
            return out;
        }
    };

    // A plain delay line for the slap-echo character.
    struct Tap {
        float* buf = nullptr;
        int    cap = 0, len = 1, pos = 0;

        void init(float* mem, int c) { buf = mem; cap = c < 1 ? 1 : c; len = cap; pos = 0; clear(); }
        void set_length(int n) { len = n < 1 ? 1 : (n > cap ? cap : n); pos = 0; }
        void clear() { if (buf) for (int i = 0; i < cap; i++) buf[i] = 0.f; pos = 0; }
        inline float peek() const { return buf ? buf[pos] : 0.f; }
        inline void  push(float v) { if (!buf) return; buf[pos] = v; if (++pos >= len) pos = 0; }
    };

    Comb      _comb[kCombs];
    AllPass   _allpass[kAllpasses];
    Tap       _slap;
    Character _character = Character::Plate;
    float     _sr = 48000.f;
    float     _feedback = 0.84f;
    float     _damp = 0.2f;
    bool      _active = false;
};

} // namespace bard
} // namespace spotykach
