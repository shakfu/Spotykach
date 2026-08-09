// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// The typed OSC reply sink: a TextSink whose overrides turn layer [3]'s reply calls into one typed OSC
// message, SLIP-framed, instead of a line of ASCII. See docs/dev/terminal-osc.md ("Implementation
// shape" and "Reads"). Layer [3] is unchanged - the verb handlers call the same methods they always
// did, and the typing information they already carried (ok_f32 vs ok_i32 vs a raw str) is what selects
// the OSC type tag.
//
// Nothing is written until finish(), because a reply's SHAPE is only known once the handler has
// returned: `ok_begin(); append_i32(); ok_end()` is an int reply, while the same frame around two
// appends and a literal (`query usb`) is a string one.

#include "engine/terminal_io.h"

#if SPK_TERMINAL_OSC

#include <cstddef>
#include <cstdint>

namespace spotykach {

class OscSink : public TextSink {
  public:
    explicit OscSink(ITextOut& out) : TextSink(out) {}

    // Bind this sink to one request. `request_address` must outlive the dispatch (it points into the
    // SLIP packet buffer, which the transport holds until the reply is flushed).
    void begin_request(const char* request_address, bool ack_mode);

    // Whether a successful, valueless outcome should produce a message at all. Reads set this true;
    // writes set it false, because a rig streaming fader moves at 100 Hz does not want an ack per
    // message. Errors are ALWAYS emitted regardless. See "Errors" in the spec: there is deliberately
    // no /sk/ok, and hosts that want acks opt in per-session with /sk/dev/mode/ack.
    void expect_reply(bool yes) { _expect = yes; }

    // True once err() has run, so the decoder can tell a rejected dispatch from a silent write.
    bool errored() const { return _kind == Kind::Err; }

    // --- TextSink overrides -------------------------------------------------------------------------
    void str(const char* s) override;
    void line(const char* s) override;
    void ok() override;
    void ok_i32(int32_t v) override;
    void ok_f32(float v, int decimals = 4) override;
    void ok_hex(uint32_t v) override;
    void err(const char* reason) override;

    void append_i32(int32_t v) override;
    void append_hex(uint32_t v) override;
    void append_f32(float v, int decimals = 4) override;

    void ok_begin() override;
    void ok_end() override;

    void finish() override;

    // Emit a transport-level error (`slip-overflow`, `bad-packet`) that has no request address of its
    // own. Framed exactly like any other error so a host needs one parser.
    void emit_error(const char* request_address, const char* reason);

    // Send a pre-built OSC packet (the describe bundle) as one SLIP frame. Bypasses the reply
    // accumulation entirely - a bundle is not a reply to type-tag, it is already a packet.
    void send_packet(const uint8_t* p, size_t n);

  private:
    // What the handler turned out to be saying. Text is the fallback: anything that wrote raw
    // characters, or more than one value, degrades to `,s` with the bytes the line codec would have
    // produced. That is the documented "only the generic str() path degrades to a string" rule.
    enum class Kind : uint8_t { None, Ok, Int, Float, Text, Err };

    void put(const char* s, size_t n);   // accumulate reply text
    void note_value(Kind k);             // record a typed value seen inside an ok_begin/ok_end frame

    // Long enough for the widest free-form reply layer [3] can produce - `query usb`'s field dump
    // (~120 chars) and the `help` verb line (~110). describe never arrives here: the OSC codec answers
    // it with a bundle before dispatch is ever called.
    static constexpr size_t kTextMax = 256;

    const char* _request = "/sk";
    bool        _ack     = false;
    bool        _expect  = true;

    Kind    _kind = Kind::None;
    int32_t _i    = 0;
    float   _f    = 0.f;

    const char* _reason = "";      // string literals from the fixed token set; never copied
    char        _text[kTextMax];
    size_t      _text_len = 0;

    bool    _armed  = false;       // inside an ok_begin()/ok_end() frame
    uint8_t _values = 0;           // typed values seen while armed
    bool    _raw    = false;       // raw text seen while armed -> degrade to a string
};

}  // namespace spotykach

#endif  // SPK_TERMINAL_OSC
