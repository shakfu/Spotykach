// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// Contract-side types for the SPK_TERMINAL control/test channel (docs/dev/terminal-*.md).
// These live on the engine side of the boundary (not in src/terminal/) so IEngine can declare
// handle_command() without the contract depending on the terminal service. The concrete Terminal
// implements ITextOut; the dispatcher wraps it in a TextSink and hands engines a read-only
// CommandView over the already-tokenized argv - so an engine never touches the codec or the USB path.
//
// Everything here is compiled only under SPK_TERMINAL; when the flag is off the header is inert and
// the IEngine hooks that reference these types do not exist (zero cost when off).

#include <cstddef>
#include <cstdint>

namespace spotykach {

// --- declared queries (target B) --------------------------------------------------------------------
// See docs/dev/terminal-target-b.md. An engine declares WHAT it can report; the platform does the
// matching, the deck validation, the reply framing and the `describe` emission - so an engine writes no
// parser and cannot get the wire grammar wrong, and dispatch and description cannot drift apart.

// How a host should parse a reply value.
enum class ValueKind : uint8_t { Bool, Int, Float, Enum, Text };

// Whether a query takes a deck. Deck -> the platform validates one and passes it; Global -> DeckRef::A.
enum class QueryScope : uint8_t { Global, Deck };

struct EngineQuery {
    const char* name;    // must not collide with a platform query; the platform set wins
    QueryScope  scope;
    ValueKind   kind;
    const char* labels;  // Enum only: "0:none 1:plain 2:faded"; nullptr otherwise
    bool        safe;    // idempotent AND side-effect free.
                         // Only `safe` entries are ADVERTISED in describe, which makes the generic
                         // sweep correct by construction: it calls everything it can see, and can only
                         // see what is safe to call. A latching read (one that self-clears, like
                         // take_param_reseed) must declare false - it stays reachable by name, it is
                         // simply never offered to a generic consumer.
};

struct EngineQueryTable {
    const EngineQuery* items;
    uint8_t            count;
};

// A read-only view over the tokenized command line the codec produced. argv[0] is the verb.
struct CommandView {
    const char* const* argv;
    uint8_t            argc;
    const char* arg(uint8_t i) const { return i < argc ? argv[i] : ""; }
};

// Abstract byte sink the reply path writes through. The Terminal implements this over its
// non-blocking TX FIFO; keeping it abstract means this contract header pulls in nothing from
// src/terminal/ and no USB/daisy types leak onto the engine side.
class ITextOut {
  public:
    virtual void write(const char* s, size_t n) = 0;

    // How many bytes can still be accepted without loss. The line codec never asks - a dropped line is
    // reported and the host retries - but the OSC codec must, because its `describe` reply is a single
    // SLIP frame assembled from many small writes: if the FIFO fills partway through, the frame on the
    // wire is corrupt rather than merely missing. Default "unbounded" suits the test doubles.
    virtual size_t writable() const { return static_cast<size_t>(-1); }
  protected:
    ~ITextOut() = default;
};

// TextSink as a COMPILE-TIME POLICY (docs/dev/terminal-osc.md, "Implementation shape"). The OSC codec
// needs the same verb handlers to emit typed OSC arguments instead of ASCII, and the typing information
// already exists at every call site - which is why the fix is to override these methods rather than to
// fork dispatch. Under SPK_TERMINAL_OSC the reply interface becomes virtual so OscSink can do that;
// in the line build the keyword expands to nothing and TextSink keeps its original non-virtual,
// vtable-free shape. Zero cost when OSC is off.
#if SPK_TERMINAL_OSC
#define SPK_SINK_VIRTUAL virtual
#else
#define SPK_SINK_VIRTUAL
#endif

// The reply interface handed to verb handlers and to IEngine::handle_command. Formats replies in
// the phase-1 grammar (ok / ok <value> / err <reason>, CRLF-framed). Floats are formatted by
// integer decomposition - the firmware does not link _printf_float, so "%f" is unavailable.
class TextSink {
  public:
    explicit TextSink(ITextOut& out) : _out(out) {}
    SPK_SINK_VIRTUAL ~TextSink() = default;

    SPK_SINK_VIRTUAL void str(const char* s);                     // raw, no newline
    SPK_SINK_VIRTUAL void line(const char* s);                    // s + "\r\n"
    SPK_SINK_VIRTUAL void ok();                                   // "ok\r\n"
    SPK_SINK_VIRTUAL void ok_i32(int32_t v);                      // "ok <int>\r\n"
    SPK_SINK_VIRTUAL void ok_f32(float v, int decimals = 4);      // "ok <float>\r\n"
    SPK_SINK_VIRTUAL void ok_hex(uint32_t v);                     // "ok 0x<hex>\r\n"
    SPK_SINK_VIRTUAL void err(const char* reason);                // "err <reason>\r\n"

    SPK_SINK_VIRTUAL void append_i32(int32_t v);                  // append a signed integer
    SPK_SINK_VIRTUAL void append_hex(uint32_t v);                 // append "0x" + hex
    SPK_SINK_VIRTUAL void append_f32(float v, int decimals = 4);  // append a float, no %f

    // Framing for a reply whose VALUE is appended by someone else - the `query` path, where the
    // platform writes the frame and the value comes from read_platform_query() or, for an engine's own
    // declared query, from IEngine::read_engine_query(). In the line codec these are exactly the
    // `str("ok ")` / `str("\r\n")` they replace, byte for byte.
    //
    // They exist because OSC has to type that value and cannot see the call site: between ok_begin()
    // and ok_end() the OSC sink watches which append_* runs. Exactly one append_i32 -> `,i`, exactly one
    // append_f32 -> `,f`, anything else (raw str(), or several values, as `query usb` does) -> `,s` with
    // the text the line codec would have produced. That is the "only the generic str() path degrades to
    // a string" rule in docs/dev/terminal-osc.md, made mechanical.
    SPK_SINK_VIRTUAL void ok_begin();                             // "ok "
    SPK_SINK_VIRTUAL void ok_end();                               // "\r\n"

    // Called by the transport once per received command, after dispatch has returned. The line codec
    // has already written every byte it is going to write, so this is a no-op; the OSC sink uses it to
    // emit the single typed message it has been accumulating. See docs/dev/terminal-osc.md.
    SPK_SINK_VIRTUAL void finish() {}

  private:
    ITextOut& _out;

  protected:
    // Subclasses reply through the same byte sink (OscSink writes SLIP frames to it).
    ITextOut& out() { return _out; }
};

}  // namespace spotykach
