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
  protected:
    ~ITextOut() = default;
};

// The reply interface handed to verb handlers and to IEngine::handle_command. Formats replies in
// the phase-1 grammar (ok / ok <value> / err <reason>, CRLF-framed). Floats are formatted by
// integer decomposition - the firmware does not link _printf_float, so "%f" is unavailable.
class TextSink {
  public:
    explicit TextSink(ITextOut& out) : _out(out) {}

    void str(const char* s);                     // raw, no newline
    void line(const char* s);                    // s + "\r\n"
    void ok();                                   // "ok\r\n"
    void ok_i32(int32_t v);                      // "ok <int>\r\n"
    void ok_f32(float v, int decimals = 4);      // "ok <float>\r\n"
    void ok_hex(uint32_t v);                     // "ok 0x<hex>\r\n"
    void err(const char* reason);                // "err <reason>\r\n"

    void append_i32(int32_t v);                  // append a signed integer
    void append_hex(uint32_t v);                 // append "0x" + hex
    void append_f32(float v, int decimals = 4);  // append a float, no %f

  private:
    ITextOut& _out;
};

}  // namespace spotykach
