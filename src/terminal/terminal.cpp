// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#include "terminal/terminal.h"

#if SPK_TERMINAL

#include "terminal/rx_ring.h"
#include "terminal/dispatch.h"

#if SPK_TERMINAL_OSC
#include "terminal/osc_addr.h"
#include "terminal/osc_sink.h"
#endif

#include "sys/system.h"   // daisy::System::GetNow - the `mode test` dead-man switch

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

// Whether to bring the VDD33_USB level detector up (and wait for USB33RDY) before UsbHandle::Init.
// See usb_diag.h. Default on: it is strictly an ordering correction, and it is the cheapest test of
// whether the missing D+ pullup is a transceiver-supply problem. Set 0 to restore libDaisy's order.
#ifndef SPK_TERMINAL_USB33_PREINIT
#define SPK_TERMINAL_USB33_PREINIT 1
#endif

// How long `mode test` may go without a command before the platform takes its knobs back. Test mode
// freezes physical input, so a harness that crashes or unplugs mid-run would otherwise leave the
// instrument inert with no recovery short of a power cycle. Generous enough that a live session is
// never interrupted - any command resets the timer - and short enough to be forgiving of a crash.
#ifndef SPK_TERMINAL_TEST_MODE_TIMEOUT_MS
#define SPK_TERMINAL_TEST_MODE_TIMEOUT_MS 30000
#endif

// Which USB peripheral the channel lives on. Defined (with its default) in usb_diag.h so the probe and
// the channel cannot disagree about which core they mean. Default = FS_EXTERNAL: the Spotykach's panel
// USB-C is wired to OTG_HS (PB14/PB15), not to the Seed's own OTG_FS pins. `TERMPORT=int` for a bare
// Seed or Pod.

#if SPK_TERMINAL_PORT_EXTERNAL
static constexpr auto kUsbPeriph = daisy::UsbHandle::FS_EXTERNAL;
#else
static constexpr auto kUsbPeriph = daisy::UsbHandle::FS_INTERNAL;
#endif

void Terminal::RxTrampoline(uint8_t* buf, uint32_t* len) {   // USB IRQ context
    g_rx.push(buf, static_cast<size_t>(*len));               // copy out, publish, return - nothing else
}

void Terminal::init(IEngine& engine) {
    _engine = &engine;

    // Probe the clock/supply preconditions BEFORE the device is brought up, while they still reflect
    // what the boot path left behind. See usb_diag.h for why this is not answerable from source alone.
    usb_diag_capture_pre(_state.usb);

#if SPK_TERMINAL_INIT_USB
#if SPK_TERMINAL_USB33_PREINIT
    // Validate the VDD33_USB transceiver supply before anything asserts DevConnect. libDaisy's
    // UsbHandle::Init enables the level detector only AFTER InitFS() has already connected, and never
    // waits for USB33RDY, so the core can be told to present a pullup before its supply is ready.
    _state.usb.usb33_ready   = usb_supply_bringup();
    _state.usb.usb33_detector = true;
#endif
    _usb.Init(kUsbPeriph);   // nothing else owns this port in this firmware; we do
#endif
    _usb.SetReceiveCallback(&Terminal::RxTrampoline, kUsbPeriph);

    usb_diag_capture_post(_state.usb);   // transceiver powered? pullup presented to the host?
}

void Terminal::write(const char* s, size_t n) {
    _tx.enqueue(s, n);
}

size_t Terminal::writable() const {
    return _tx.free_space();
}

void Terminal::emit_err(const char* reason) {
#if SPK_TERMINAL_OSC
    // Transport-level errors have no request address of their own, so they carry the root. Framed as
    // an ordinary /sk/err so a host needs exactly one error parser.
    OscSink sink(*this);
    sink.emit_error("/sk", reason);
#else
    TextSink sink(*this);
    sink.err(reason);
#endif
}

void Terminal::on_line(char* line, size_t len) {
    (void)len;
    _state.last_cmd_ms = daisy::System::GetNow();   // feed the `mode test` dead-man switch
    TextSink sink(*this);
    dispatch_line(line, *_engine, sink, _state);
    sink.finish();
}

#if SPK_TERMINAL_OSC
void Terminal::on_packet(const uint8_t* p, size_t n) {
    _state.last_cmd_ms = daisy::System::GetNow();   // the dead-man switch is codec-independent
    OscSink sink(*this);
    osc_dispatch_packet(p, n, *_engine, sink, _state);
}
#endif

void Terminal::process() {
    if (!_engine) return;

    if (g_rx.take_overflow()) emit_err("overflow");   // report a dropped RX burst before the next line

    // A reply was dropped because the TX FIFO was full. Report it rather than swallowing it: the host
    // is synchronous (one command outstanding), so a silently lost reply reads as an unexplained
    // timeout. If this enqueue does not fit either, TxFifo re-latches and we retry next iteration.
    if (_tx.take_overflow()) emit_err("tx-overflow");

    // Dead-man switch: hand physical input back if a test session went silent (see term_state.h).
    // Reverting is silent - the protocol is one reply per command, so an unsolicited line here would
    // desynchronise a host that later reconnects.
    if (_state.test_mode
        && (daisy::System::GetNow() - _state.last_cmd_ms) > SPK_TERMINAL_TEST_MODE_TIMEOUT_MS) {
        _state.test_mode = false;
    }

    uint8_t chunk[64];
    size_t  n;
    while ((n = g_rx.pop(chunk, sizeof(chunk))) > 0) {
        for (size_t i = 0; i < n; ++i) {
#if SPK_TERMINAL_OSC
            switch (_asm.feed(chunk[i])) {
                case SlipAssembler::Feed::Ready:
                    on_packet(_asm.packet(), _asm.len());
                    _asm.reset();
                    break;
                // Resynchronize rather than truncate: a truncated OSC packet is undetectable garbage,
                // where a truncated line is merely wrong. The assembler has already discarded to the
                // frame boundary, so the next packet parses cleanly.
                case SlipAssembler::Feed::Overflow:
                    emit_err("slip-overflow");
                    _asm.reset();
                    break;
                case SlipAssembler::Feed::Pending:
                    break;
            }
#else
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
#endif
        }
    }

    flush_tx();
}

void Terminal::flush_tx() {
    // Stage into the slot that is definitely not in flight (see terminal.h). A successful transmit
    // means the stack accepted this slot AND had already finished with the other one, so it is only
    // then that we advance the FIFO and swap. A busy return leaves both the FIFO and the in-flight
    // slot untouched, so the next process() simply retries.
    uint8_t* buf = _scratch[_slot];
    size_t   n   = _tx.peek(buf, kScratch);
#if SPK_TERMINAL_PORT_EXTERNAL
    const auto res = n ? _usb.TransmitExternal(buf, n) : daisy::UsbHandle::Result::ERR;
#else
    const auto res = n ? _usb.TransmitInternal(buf, n) : daisy::UsbHandle::Result::ERR;
#endif
    if (n && res == daisy::UsbHandle::Result::OK) {
        _tx.commit(n);
        _slot ^= 1;
    }
}

}  // namespace spotykach

#endif  // SPK_TERMINAL
