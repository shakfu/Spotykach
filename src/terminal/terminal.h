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
#include "terminal/usb_diag.h"     // UsbDiag - OTG_FS bring-up probe (docs/dev/terminal-impl.md)

#include "hid/usb.h"               // daisy::UsbHandle

// MidiUsbHandler claims the SAME peripheral as the default terminal port - Periph::EXTERNAL, i.e.
// OTG_HS (see hw/hardware.cpp) - and USB_MIDI defaults ON for every BOOT_QSPI build. Two USB device
// stacks on one core is not a thing; fail at compile time rather than at 3am on a bench.
// Fix by choosing one: USB_MIDI=0, or TERMPORT=int if that board's jack is really on OTG_FS.
// NOTE the SPK_TERMINAL term: terminal.cpp includes this header BEFORE its own `#if SPK_TERMINAL`,
// so this file is parsed by every build. Without it the guard fired on any USB_MIDI=1 build - i.e.
// every BOOT_QSPI engine in `make dist` - even though no terminal was being compiled. There is no
// conflict unless the channel is actually built.
#if SPK_TERMINAL && defined(SPK_USB_MIDI) && SPK_TERMINAL_PORT_EXTERNAL
#error "SPK_TERMINAL (external port) and SPK_USB_MIDI both claim OTG_HS as a USB device. \
Build with USB_MIDI=0, or TERMPORT=int if this board's terminal jack is on OTG_FS."
#endif

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

    // The OTG_FS bring-up snapshot taken during init(). Readable without a working USB port (the
    // TERM_USBDIAG probe in app.cpp), and reported by `query usb` once the port does come up.
    const UsbDiag& usb_diag() const { return _state.usb; }

    // Same, but re-reading everything that can change after init (core state, D+/D- pad ownership,
    // sticky host activity). Call from the main loop; see usb_diag.h for why a snapshot is not enough.
    const UsbDiag& refresh_usb_diag() { usb_diag_refresh(_state.usb); return _state.usb; }

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

    // TX staging, ping-ponged. CDC_Transmit_FS returns OK once the packet is QUEUED; in non-DMA mode
    // the HAL copies out of this buffer later, from the TX-FIFO-empty interrupt. The main loop runs far
    // faster than a USB frame, so a single buffer would be refilled while the peripheral was still
    // reading it. Two slots, swapped only on a successful transmit, make that impossible: a transmit
    // only succeeds when TxState is clear, which proves the other slot's transfer has completed.
    static constexpr size_t kScratch = 64;   // one USB FS packet per flush
    uint8_t          _scratch[2][kScratch];
    uint8_t          _slot = 0;              // the slot the next peek() stages into
};

}  // namespace spotykach
