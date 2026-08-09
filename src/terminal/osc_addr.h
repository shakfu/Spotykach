// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// Layer [2] entry point for the OSC codec: one SLIP-framed packet in, one typed OSC reply out. The
// address is resolved to a line in the EXISTING grammar and handed to the EXISTING dispatch_line(), so
// there is no second verb table and no second error taxonomy. See docs/dev/terminal-osc.md.

#include "engine/iengine.h"
#include "terminal/term_state.h"

#if SPK_TERMINAL_OSC

#include "terminal/osc_sink.h"

namespace spotykach {

// Capacity of the static scratch the `describe` bundle is assembled in, and therefore the hard ceiling
// on a descriptor. MEASURED, not estimated: an engine on the default all-live masks produces 5392 B
// (64 rows), which host/test_terminal_osc.cpp asserts against this constant so the guard cannot rot as
// params or queries are added. Real engines mask hard and land near 1 KB.
//
// The TX FIFO must be at least this big too (see tx_fifo.h): a bundle cannot be streamed the way lines
// can, so the whole descriptor has to fit at once.
constexpr size_t kOscBundleCap = 6144;

// Decode and dispatch one complete OSC packet (a message, or a bundle whose elements dispatch
// immediately and in order). Writes at most one reply per contained message through `sink`.
void osc_dispatch_packet(const uint8_t* p, size_t n, IEngine& engine, OscSink& sink, TermState& state);

}  // namespace spotykach

#endif  // SPK_TERMINAL_OSC
