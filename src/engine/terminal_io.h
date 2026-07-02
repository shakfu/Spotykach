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
