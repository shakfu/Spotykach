// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#include "terminal/osc.h"

#if SPK_TERMINAL_OSC

#include <cmath>
#include <cstring>

#pragma GCC optimize("Os")

namespace spotykach {

namespace {

// OSC scalars are big-endian; every core this builds for is little-endian, so read them byte-wise
// rather than casting - which also sidesteps the unaligned-load question entirely.
uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint64_t be64(const uint8_t* p) {
    return (uint64_t(be32(p)) << 32) | uint64_t(be32(p + 4));
}

float f32_from_bits(uint32_t b) {
    float f;
    std::memcpy(&f, &b, sizeof f);
    return f;
}

double f64_from_bits(uint64_t b) {
    double d;
    std::memcpy(&d, &b, sizeof d);
    return d;
}

size_t pad4(size_t n) { return (n + 3u) & ~size_t{3}; }

// Read one OSC-string starting at `off`. Returns false if it is unterminated inside the packet.
// On success `out` points at it (in place - the packet buffer supplies the NUL) and `off` advances
// past its 4-byte padding.
bool read_string(const uint8_t* p, size_t n, size_t& off, const char*& out) {
    if (off >= n) return false;
    size_t end = off;
    while (end < n && p[end] != '\0') ++end;
    if (end >= n) return false;                    // no terminator inside the packet
    out = reinterpret_cast<const char*>(p + off);
    const size_t padded = pad4(end - off + 1);     // include the NUL, then round up
    if (off + padded > n) return false;
    off += padded;
    return true;
}

}  // namespace

bool OscMessage::parse(const uint8_t* p, size_t n) {
    _argc    = 0;
    _address = "";
    if (!p || n < 4 || (n & 3u)) return false;     // OSC packets are a whole number of 4-byte words

    size_t off = 0;
    const char* addr = nullptr;
    if (!read_string(p, n, off, addr)) return false;
    if (addr[0] != '/') return false;              // an address pattern must be absolute
    _address = addr;

    // A message with no type-tag string at all is legal OSC 1.0 (and means "no arguments"). That is
    // exactly the READ form here, so it must be accepted rather than rejected as malformed.
    if (off >= n) return true;

    const char* tags = nullptr;
    if (!read_string(p, n, off, tags)) return false;
    if (tags[0] != ',') return false;

    for (const char* t = tags + 1; *t; ++t) {
        if (_argc >= kMaxArgs) return false;       // caller reports too-many-args
        OscArg a;
        switch (*t) {
            case 'i':
                if (off + 4 > n) return false;
                a.type = OscType::Int32;
                a.i    = static_cast<int32_t>(be32(p + off));
                off += 4;
                break;
            case 'f':
                if (off + 4 > n) return false;
                a.type = OscType::Float;
                a.f    = f32_from_bits(be32(p + off));
                off += 4;
                break;
            case 'd':
                if (off + 8 > n) return false;
                a.type = OscType::Double;
                a.d    = f64_from_bits(be64(p + off));
                off += 8;
                break;
            case 's':
            case 'S': {   // symbol: identical on the wire to a string
                const char* s = nullptr;
                if (!read_string(p, n, off, s)) return false;
                a.type = OscType::String;
                a.s    = s;
                break;
            }
            case 'T': a.type = OscType::True;  break;
            case 'F': a.type = OscType::False; break;
            default:
                // Blobs, timetags-as-arguments, arrays, nil, infinitum: rejected outright rather than
                // skipped, because skipping one silently shifts every argument after it.
                return false;
        }
        _args[_argc++] = a;
    }
    return true;
}

// --- coercions -------------------------------------------------------------------------------------
// The one place OSC is deliberately more permissive than the line codec, because control surfaces are
// inconsistent about what a button sends. See the type table in docs/dev/terminal-osc.md.

bool OscMessage::as_f32(uint8_t i, float& out) const {
    if (i >= _argc) return false;
    const OscArg& a = _args[i];
    switch (a.type) {
        case OscType::Float:  out = a.f; break;
        case OscType::Int32:  out = static_cast<float>(a.i); break;
        case OscType::Double: out = static_cast<float>(a.d); break;   // narrowed
        default: return false;
    }
    // Non-finite is rejected, matching parse_f32 in the line codec: NaN into set_param would propagate
    // through the DSP and never come back.
    return std::isfinite(out);
}

bool OscMessage::as_i32(uint8_t i, int32_t& out) const {
    if (i >= _argc) return false;
    const OscArg& a = _args[i];
    switch (a.type) {
        case OscType::Int32: out = a.i; break;
        case OscType::True:  out = 1;   break;
        case OscType::False: out = 0;   break;
        case OscType::Float:
            if (!std::isfinite(a.f)) return false;
            out = static_cast<int32_t>(a.f);      // truncated toward zero
            break;
        case OscType::Double:
            if (!std::isfinite(a.d)) return false;
            out = static_cast<int32_t>(a.d);
            break;
        default: return false;
    }
    return true;
}

bool OscMessage::as_bool(uint8_t i, bool& out) const {
    // ABSENT ARGUMENT = TRUE: a bare trigger. `/sk/a/gate` with no arguments is a gate, and this is
    // what makes the no-argument form of a stimulus verb mean "fire" rather than "read".
    if (i >= _argc) { out = true; return true; }
    const OscArg& a = _args[i];
    switch (a.type) {
        case OscType::True:  out = true;  break;
        case OscType::False: out = false; break;
        case OscType::Int32: out = (a.i != 0); break;
        // TouchOSC buttons send ,f 1.0 / ,f 0.0 in some configurations, so a zero float suppresses the
        // trigger rather than firing it.
        case OscType::Float:  out = (a.f != 0.f); break;
        case OscType::Double: out = (a.d != 0.0); break;
        default: return false;
    }
    return true;
}

const char* OscMessage::as_str(uint8_t i) const {
    if (i >= _argc) return nullptr;
    return _args[i].type == OscType::String ? _args[i].s : nullptr;
}

// --- bundles ---------------------------------------------------------------------------------------

bool osc_is_bundle(const uint8_t* p, size_t n) {
    return p && n >= 8 && std::memcmp(p, "#bundle", 8) == 0;
}

bool osc_bundle_next(const uint8_t* p, size_t n, size_t& cursor,
                     const uint8_t*& elem, size_t& elem_len) {
    if (!osc_is_bundle(p, n)) return false;
    if (cursor == 0) cursor = 16;              // "#bundle\0" (8) + timetag (8); the timetag is ignored
    if (cursor + 4 > n) return false;
    const uint32_t sz = be32(p + cursor);
    cursor += 4;
    if (sz == 0 || (sz & 3u) || cursor + sz > n) return false;
    elem     = p + cursor;
    elem_len = sz;
    cursor  += sz;
    return true;
}

}  // namespace spotykach

#endif  // SPK_TERMINAL_OSC
