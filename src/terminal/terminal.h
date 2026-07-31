// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once
#include <cstdint>   // uint*_t (transitive-include hygiene; host build)
#include <cstddef>   // size_t (transitive-include hygiene; host build)

// The SPK_TERMINAL service: a bidirectional text/command channel over the Daisy USB-C CDC port, used
// primarily to test engine features from a host script. See docs/dev/terminal-control.md for the why
// and docs/dev/terminal-{transport,dispatch}.md for the layers realized here.
//
// Everything is compiled only under SPK_TERMINAL; when the flag is off this header/TU is inert and
// app.cpp never references it (zero cost when off - the SPK_USE_STREAM / METER pattern).

#include "engine/iengine.h"
#include "engine/terminal_io.h"    // ITextOut / TextSink / CommandView (contract-side)
#include "terminal/term_state.h"   // TermState (shared with dispatch.cpp)
#include "terminal/tx_fifo.h"
#include "terminal/line_assembler.h"

#include "hid/usb.h"               // daisy::UsbHandle

namespace spotykach {

class Terminal : public ITextOut {
  public:
    Terminal() = default;

    // Bring up the channel. Stores &engine (bound as IEngine& for the life of the program - the app
    // holds one ActiveEngine) and attaches the USB RX callback. Call AFTER Log::StartLog so, in a build
    // where a logger owns the CDC device, it is already up before the callback is attached.
    void init(IEngine& engine);

    // Main-loop pump: drain RX -> assemble lines -> dispatch -> flush TX. Non-blocking throughout.
    void process();

    // ITextOut: enqueue reply bytes on the non-blocking TX FIFO (used by TextSink / dispatch).
    void write(const char* s, size_t n) override;

    // Read-only for the platform's `mode test` input isolation (app.cpp pushes it to CoreUI).
    bool test_mode() const { return _state.test_mode; }

  private:
    void on_line(char* line, size_t len);   // tokenize + dispatch one complete line
    void flush_tx();                        // drain the TX FIFO, non-blocking
    void emit_err(const char* reason);      // transport-level error reply (overflow / line-too-long)

    static void RxTrampoline(uint8_t* buf, uint32_t* len);   // USB IRQ context -> g_rx.push

    daisy::UsbHandle _usb;
    LineAssembler    _asm;
    TxFifo           _tx;
    TermState        _state;
    IEngine*         _engine = nullptr;
    uint8_t          _scratch[64];   // one USB FS packet per flush; describe drains over iterations
};

}  // namespace spotykach
