// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// Layer [3] dispatch entry point: turn one tokenized line into an IEngine call (target A / L0/L1) or
// an engine-specific handle_command (target B), and write one deterministic reply. Platform verbs are
// tried first (every engine gets the reflective surface for free); unknown verbs fall through to the
// engine's own handler, then to "err unknown-verb". See docs/dev/terminal-dispatch.md.

#include "engine/iengine.h"
#include "engine/terminal_io.h"
#include "terminal/term_state.h"

namespace spotykach {

// `line` is a mutable, NUL-terminated line buffer ('\r'/'\n' already stripped by the assembler); the
// tokenizer splits it in place. One reply is written to `reply` per call.
void dispatch_line(char* line, IEngine& engine, TextSink& reply, TermState& state);

}  // namespace spotykach
