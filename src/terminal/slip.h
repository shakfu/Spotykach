// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// Layer [1]/[2] seam for the OSC codec: SLIP framing (RFC 1055). The exact role line_assembler.h plays
// in the line build, with the same shape - fed one RX byte at a time from the same SPSC ring, reporting
// when a complete unit is available - except that the unit is a binary packet rather than a NUL-
// terminated line. See docs/dev/terminal-osc.md ("Framing: SLIP").
//
// An OSC packet carries no length prefix and is not self-delimiting over a byte stream, so without a
// framing layer the receiver cannot find a boundary. SLIP supplies one, and is what the OSC-over-serial
// implementations a host would use ([oscparse], liblo's serial transports, the Arduino OSC libs)
// already speak.
//
// Overflow RESYNCHRONIZES rather than truncating: bytes are discarded to the next END and the caller
// emits `slip-overflow`. That asymmetry with the line assembler (which keeps a truncated prefix) is
// deliberate - a truncated line is merely wrong, where a truncated OSC packet is undetectable garbage.

#include <cstddef>
#include <cstdint>

namespace spotykach {

namespace slip {
constexpr uint8_t kEnd    = 0xC0;   // packet delimiter (sent leading AND trailing)
constexpr uint8_t kEsc    = 0xDB;
constexpr uint8_t kEscEnd = 0xDC;   // ESC ESC_END -> literal 0xC0
constexpr uint8_t kEscEsc = 0xDD;   // ESC ESC_ESC -> literal 0xDB
}  // namespace slip

class SlipAssembler {
  public:
    enum class Feed : uint8_t {
        Pending,    // byte consumed, packet not complete
        Ready,      // END seen on a non-empty packet: packet()/len() are valid
        Overflow,   // an over-long packet terminated -> caller emits err slip-overflow
    };

    // OSC messages are 4-byte aligned and verbose, so the 128 B line budget is far too small; 512 B
    // covers every address in the spec with headroom (the biggest inbound message is a `,iii` midi msg).
    static constexpr size_t kMax = 512;

    Feed feed(uint8_t b) {
        if (b == slip::kEnd) {
            // A leading END flushes line noise, and back-to-back ENDs are legal padding, so an empty
            // packet is silently ignored rather than reported as a zero-length message.
            if (_overflowed) { reset(); return Feed::Overflow; }
            if (_len == 0)   { _escaped = false; return Feed::Pending; }
            const bool partial_escape = _escaped;
            _escaped = false;
            // A frame that ends mid-escape is malformed; drop it rather than deliver a truncated packet.
            if (partial_escape) { _len = 0; return Feed::Overflow; }
            return Feed::Ready;
        }
        if (_overflowed) return Feed::Pending;   // discarding until the next END

        uint8_t v = b;
        if (_escaped) {
            _escaped = false;
            // Per RFC 1055 an ESC followed by anything else is a protocol violation; the conventional
            // (and safest) reading is to take the byte literally rather than to drop the frame.
            if      (b == slip::kEscEnd) v = slip::kEnd;
            else if (b == slip::kEscEsc) v = slip::kEsc;
        } else if (b == slip::kEsc) {
            _escaped = true;
            return Feed::Pending;
        }

        if (_len >= kMax) { _overflowed = true; return Feed::Pending; }   // begin swallowing
        _buf[_len++] = v;
        return Feed::Pending;
    }

    const uint8_t* packet() const { return _buf; }
    size_t         len()    const { return _len; }
    void           reset() { _len = 0; _escaped = false; _overflowed = false; }

  private:
    uint8_t _buf[kMax];
    size_t  _len        = 0;
    bool    _escaped    = false;
    bool    _overflowed = false;
};

// Exact wire size of the frame slip_encode() would produce: the two ENDs plus one extra byte per
// escaped byte. Counted rather than bounded at 2n+2 because the worst case is ~2x and never occurs -
// OSC payloads here are ASCII strings, small ints and 0..1 floats, in which 0xC0/0xDB are vanishingly
// rare - so bounding would refuse descriptor bundles that fit comfortably.
inline size_t slip_encoded_size(const uint8_t* p, size_t n) {
    size_t extra = 0;
    for (size_t i = 0; i < n; ++i)
        if (p[i] == slip::kEnd || p[i] == slip::kEsc) ++extra;
    return n + extra + 2;
}

// Encode `n` bytes as one SLIP frame through `emit`, which takes (bytes, count). END is sent both
// leading and trailing: the leading one costs a byte and guarantees that a receiver which joined
// mid-stream (or saw a stray log byte) starts this frame from a clean state.
template <typename Emit>
void slip_encode(const uint8_t* p, size_t n, Emit&& emit) {
    const char kEndByte[1] = { static_cast<char>(slip::kEnd) };
    emit(kEndByte, size_t{1});
    // Escaping is per-byte but emission is chunked: runs with nothing to escape go out in one call,
    // which keeps the TX FIFO's per-write overhead off the common path.
    size_t run = 0;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] != slip::kEnd && p[i] != slip::kEsc) { ++run; continue; }
        if (run) { emit(reinterpret_cast<const char*>(p + i - run), run); run = 0; }
        const char esc[2] = { static_cast<char>(slip::kEsc),
                              static_cast<char>(p[i] == slip::kEnd ? slip::kEscEnd : slip::kEscEsc) };
        emit(esc, size_t{2});
    }
    if (run) { emit(reinterpret_cast<const char*>(p + n - run), run); }
    emit(kEndByte, size_t{1});
}

}  // namespace spotykach
