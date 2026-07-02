// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#include "terminal/terminal.h"

#if SPK_TERMINAL

#include "terminal/rx_ring.h"
#include "terminal/dispatch.h"

// Size-optimize the whole terminal service: it is control-plane glue (line parsing, dispatch,
// formatting), not per-sample DSP, so -Os reclaims SRAM_EXEC. Same idiom as core.ui.*.cpp.
#pragma GCC optimize("Os")

namespace spotykach {

// File-scope: the RX callback is a plain C function pointer with no context arg (libDaisy's
// SetReceiveCallback), so the producer ring must be a file-scope single instance. There is exactly
// one CDC device and one Terminal, so a single ring is correct. See docs/dev/terminal-transport.md.
static RxRing g_rx;

// Whether the terminal brings up the internal (USB-C) CDC device itself. Default yes: in this
// firmware the Logger (when INFS_LOG is set) targets LOGGER_EXTERNAL (see the Makefile's
// -DINFS_LOG_TARGET=daisy::LOGGER_EXTERNAL), so nothing else owns FS_INTERNAL and the terminal must
// init it. If a build ever puts the Logger on the INTERNAL port, define SPK_TERMINAL_INIT_USB=0 so the
// terminal attaches only its RX callback and does not re-init the live device.
#ifndef SPK_TERMINAL_INIT_USB
#define SPK_TERMINAL_INIT_USB 1
#endif

void Terminal::RxTrampoline(uint8_t* buf, uint32_t* len) {   // USB IRQ context
    g_rx.push(buf, static_cast<size_t>(*len));               // copy out, publish, return - nothing else
}

void Terminal::init(IEngine& engine) {
    _engine = &engine;
#if SPK_TERMINAL_INIT_USB
    _usb.Init(daisy::UsbHandle::FS_INTERNAL);   // no logger owns the internal port here; we do
#endif
    _usb.SetReceiveCallback(&Terminal::RxTrampoline, daisy::UsbHandle::FS_INTERNAL);
}

void Terminal::write(const char* s, size_t n) {
    _tx.enqueue(s, n);
}

void Terminal::emit_err(const char* reason) {
    TextSink sink(*this);
    sink.err(reason);
}

void Terminal::on_line(char* line, size_t len) {
    (void)len;
    TextSink sink(*this);
    dispatch_line(line, *_engine, sink, _state);
}

void Terminal::process() {
    if (!_engine) return;

    if (g_rx.take_overflow()) emit_err("overflow");   // report a dropped RX burst before the next line

    uint8_t chunk[64];
    size_t  n;
    while ((n = g_rx.pop(chunk, sizeof(chunk))) > 0) {
        for (size_t i = 0; i < n; ++i) {
            switch (_asm.feed(chunk[i])) {
                case LineAssembler::Feed::Ready:
                    on_line(_asm.line(), _asm.len());
                    _asm.reset();
                    break;
                case LineAssembler::Feed::TooLong:
                    emit_err("line-too-long");
                    break;
                case LineAssembler::Feed::Pending:
                    break;
            }
        }
    }

    flush_tx();
}

void Terminal::flush_tx() {
    // A dropped reply (TX FIFO was full) is unrecoverable mid-stream; clear the latch so it does not
    // wedge. A connected, draining host never hits this - the FIFO holds a full describe dump.
    _tx.take_overflow();

    size_t n = _tx.peek(_scratch, sizeof(_scratch));
    if (n && _usb.TransmitInternal(_scratch, n) == daisy::UsbHandle::Result::OK)
        _tx.commit(n);   // advance only on a successful transmit; retry next process() otherwise
}

}  // namespace spotykach

#endif  // SPK_TERMINAL
