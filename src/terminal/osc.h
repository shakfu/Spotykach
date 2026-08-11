// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// OSC 1.0 wire format: the message reader and the message/bundle writer used by the SPK_TERMINAL_OSC
// codec. Nothing here knows what an address MEANS - that is osc_addr.cpp. See docs/dev/terminal-osc.md.
//
// OSC is big-endian and 4-byte aligned; both cores this runs on (STM32H7, and the desktop host that
// runs the off-target tests) are little-endian, so every scalar is byte-swapped explicitly rather than
// memcpy'd. Strings are NUL-terminated and padded with NULs to the next multiple of four.

#include <cstddef>
#include <cstdint>

namespace spotykach {

// The OSC type tags this codec accepts. Blobs, arrays and timetag arguments are rejected outright
// (docs/dev/terminal-osc.md, "Type coercion") - a control surface has no reason to send them, and
// accepting them would mean carrying a variable-length argument store.
enum class OscType : uint8_t { Int32, Float, Double, String, True, False };

struct OscArg {
    OscType     type = OscType::Int32;
    int32_t     i    = 0;
    float       f    = 0.f;
    double      d    = 0.0;
    const char* s    = nullptr;   // points INTO the packet buffer; valid while the packet is
};

// One parsed OSC message. Args point into the caller's packet buffer - no copying, no allocation.
class OscMessage {
  public:
    // The widest INBOUND message in the address space is `/sk/midi/msg ,iii`, but the same reader is
    // what a host uses on the way back, and the widest OUTBOUND message is a describe param row
    // (`,ssffs`, five). Six covers both with headroom, so an over-long message is still caught as
    // too-many-args by the per-address arity check rather than becoming an opaque bad-packet.
    static constexpr uint8_t kMaxArgs = 6;

    // Parse one message out of `p[0..n)`. Returns false on anything malformed (bad alignment, missing
    // NUL terminator, truncated argument, unsupported tag) - the caller answers `bad-packet`.
    bool parse(const uint8_t* p, size_t n);

    const char*   address() const { return _address; }
    uint8_t       argc()    const { return _argc; }
    const OscArg& arg(uint8_t i) const { return _args[i < _argc ? i : 0]; }

    // Coercions, per the spec's type table. Each returns false when the argument cannot be represented
    // (a non-finite float, a string where a number is wanted), which the caller reports as `bad-arg`.
    bool as_f32 (uint8_t i, float& out)   const;
    bool as_i32 (uint8_t i, int32_t& out) const;
    bool as_bool(uint8_t i, bool& out)    const;
    const char* as_str(uint8_t i) const;   // nullptr unless the argument really is a string

  private:
    const char* _address = "";
    OscArg      _args[kMaxArgs];
    uint8_t     _argc = 0;
};

// Builds one OSC message into a caller-owned buffer. `begin()` takes the whole tag string up front
// (every emission site in this codec knows its own types), which keeps the writer single-pass: address,
// then tags, then arguments, straight into the buffer with no staging.
//
// Every method is a no-op once the buffer has overflowed, and ok() reports it - so a caller may write a
// whole message and check once at the end instead of testing each append.
class OscWriter {
  public:
    OscWriter(uint8_t* buf, size_t cap) : _buf(buf), _cap(cap) {}

    // `tags` WITHOUT the leading comma: begin("/sk/err", "ss").
    bool begin(const char* address, const char* tags);
    void i32(int32_t v);
    void f32(float v);
    void str(const char* s);

    bool           ok()   const { return !_over; }
    size_t         size() const { return _len; }
    const uint8_t* data() const { return _buf; }
    void           reset() { _len = 0; _over = false; }

  private:
    void raw(const void* p, size_t n);     // append n bytes verbatim
    void pad();                            // NUL-pad to the next 4-byte boundary

    uint8_t* _buf;
    size_t   _cap;
    size_t   _len  = 0;
    bool     _over = false;
};

// A bundle is `#bundle`, an 8-byte timetag, then each element prefixed by its int32 size. Only
// `describe` sends one, and it must arrive whole (a bundle cannot be streamed the way lines can), which
// is what forces the TX FIFO to 4 KB under OSC.
//
// The timetag is always the OSC "immediately" value (0x0000000000000001). Inbound timetags are IGNORED
// rather than scheduled - the device has neither a dispatch queue nor a clock synced to the host, and
// honouring one would mean building both. See docs/dev/terminal-osc.md ("Type coercion").
class OscBundleWriter {
  public:
    OscBundleWriter(uint8_t* buf, size_t cap) : _buf(buf), _cap(cap) { open(); }

    // Start an element; write it with the returned writer, then call close_element().
    OscWriter element();
    void      close_element(const OscWriter& w);

    bool           ok()   const { return !_over; }
    size_t         size() const { return _len; }
    const uint8_t* data() const { return _buf; }

  private:
    void open();

    uint8_t* _buf;
    size_t   _cap;
    size_t   _len  = 0;
    bool     _over = false;
};

// True if a packet is a bundle rather than a message (first byte '#'). Bundle contents dispatch
// immediately and in order.
bool osc_is_bundle(const uint8_t* p, size_t n);

// Walk a bundle's elements. `cursor` starts at 0; returns false when there are no more. On success
// `elem`/`elem_len` point at one element (itself a message, or a nested bundle).
bool osc_bundle_next(const uint8_t* p, size_t n, size_t& cursor, const uint8_t*& elem, size_t& elem_len);

// --- `/sk/log` -------------------------------------------------------------------------------------
//
// Log output as OSC, because on this codec it cannot be ASCII: the Logger and the terminal share one
// CDC device, and raw bytes landing inside a SLIP frame corrupt it. See osc_log.cpp.

class ITextOut;   // spotykach::ITextOut, defined in engine/terminal_io.h

// Point the log path at a byte sink (the live Terminal), flushing anything stashed from before it
// existed. Passing nullptr detaches, which is what a test wants between cases.
void osc_log_bind(ITextOut* out);

// Format one log line and emit it as `/sk/log ,s` in its own SLIP frame. Best-effort: a line is
// dropped rather than allowed to displace a reply or half-write a frame. `common.h` routes LOG_TAGGED
// here on an OSC build, gated on INFS_LOG exactly as the Logger path is.
void osc_log_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace spotykach
