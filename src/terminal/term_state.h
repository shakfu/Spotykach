// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// Dispatcher-owned state consulted by the platform (shared by terminal.h and dispatch.cpp). Phase 1:
// the input-isolation flag toggled by `mode test` / `mode run`, plus the USB bring-up snapshot the
// `query usb` verb reports. See docs/dev/terminal-dispatch.md.

#include "terminal/usb_diag.h"

namespace spotykach {

struct TermState {
    bool    test_mode = false;   // false at boot; set by `mode test`, cleared by `mode run`
    UsbDiag usb;                 // captured by Terminal::init(); reported by `query usb`
};

}  // namespace spotykach
