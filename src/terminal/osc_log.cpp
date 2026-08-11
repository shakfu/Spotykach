// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#include "terminal/osc.h"

#if SPK_TERMINAL_OSC

// Log output as `/sk/log ,s`, one message per SLIP frame - option (b) from the "Logger coexistence"
// section of docs/dev/terminal-osc.md, replacing the phase-1 shortcut that made `OSC=1 DEBUG=1` a
// build error.
//
// The problem it solves: the Logger and the terminal share one CDC device, and the Logger writes raw
// ASCII. Harmless for the line codec, where a `[tag]` line merely interleaves with replies and the
// client skips anything starting with `[`. Fatal for SLIP, where those bytes land INSIDE a frame -
// the packet is then neither valid OSC nor recoverable, and the failure looks like a device that
// stopped answering rather than one that logged something.
//
// So on an OSC build nothing may reach the CDC except through this file and the reply sink. Two halves
// enforce that: the Makefile points the Logger at LOGGER_NONE (so a stray `Log::PrintLine` is
// discarded rather than corrupting the stream), and `common.h` reroutes LOG_TAGGED here.

#include "engine/terminal_io.h"
#include "terminal/slip.h"

#include <cstdarg>
#include <cstdio>

namespace spotykach {

namespace {

// The live terminal, as a byte sink. File-scope for the same reason the RX ring is: LOG_TAGGED is a
// macro reachable from any TU and cannot be handed a context pointer.
ITextOut* g_out = nullptr;

// Longest log line carried. The boot banner is the longest real one (~60 chars); the rest is room for
// engine diagnostics. A longer line is truncated rather than dropped - a clipped log still says what
// happened, where silence does not.
constexpr size_t kTextMax = 192;

// TX FIFO space kept free for replies. Logs are best-effort and MUST NOT displace an answer: a client
// blocked waiting for a read it will never get is a worse outcome than a missing log line. This is
// deliberately not large enough to also protect a `describe` bundle (~6 KB of the 8 KB FIFO) - that is
// requested explicitly and the FIFO drains every process() iteration, so the two rarely collide, and
// reserving for it would mean never logging at all.
constexpr size_t kReplyReserve = 1024;

// One message held from before the terminal exists. `LOG_TAGGED("boot", ...)` in app.cpp runs before
// `_terminal.init()` - it has to, so the banner is emitted even on a build where a logger owns the
// device - and the banner is the single most useful line to see over the channel. Anything earlier
// than the terminal beyond that one line is dropped.
char g_pending[kTextMax];
bool g_have_pending = false;

void emit(const char* text) {
    uint8_t    buf[kTextMax + 64];
    OscWriter  w(buf, sizeof buf);
    w.begin("/sk/log", "s");
    w.str(text);
    if (!w.ok()) return;
    // Whole frame or nothing, exactly as OscSink::send_packet does: a frame that ran out of FIFO
    // partway through would put corrupt bytes on the wire rather than simply losing a line.
    if (g_out->writable() < slip_encoded_size(w.data(), w.size()) + kReplyReserve) return;
    slip_encode(w.data(), w.size(), [](const char* b, size_t c) { g_out->write(b, c); });
}

}  // namespace

void osc_log_bind(ITextOut* out) {
    g_out = out;
    if (out && g_have_pending) {
        g_have_pending = false;
        emit(g_pending);
    }
}

void osc_log_printf(const char* fmt, ...) {
    char    text[kTextMax];
    va_list ap;
    va_start(ap, fmt);
    // vsnprintf always NUL-terminates and never overruns; a long line is truncated, which is why
    // kTextMax is a comfort limit rather than a correctness one.
    (void)std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);

    if (!g_out) {
        // Before the terminal exists. Keep the FIRST such line, not the last: the boot banner is the
        // one worth having, and a later flood must not evict it.
        if (!g_have_pending) {
            for (size_t i = 0; i < sizeof g_pending; ++i) {
                g_pending[i] = text[i];
                if (!text[i]) break;
            }
            g_pending[sizeof g_pending - 1] = '\0';
            g_have_pending = true;
        }
        return;
    }
    emit(text);
}

}  // namespace spotykach

#endif  // SPK_TERMINAL_OSC
