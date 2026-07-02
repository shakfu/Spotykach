// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// Dispatcher-owned state consulted by the platform (shared by terminal.h and dispatch.cpp). Phase 1:
// just the input-isolation flag toggled by `mode test` / `mode run`. See docs/dev/terminal-dispatch.md.

namespace spotykach {

struct TermState {
    bool test_mode = false;   // false at boot; set by `mode test`, cleared by `mode run`
};

}  // namespace spotykach
