// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// Dispatcher-owned state consulted by the platform (shared by terminal.h and dispatch.cpp). Phase 1:
// the input-isolation flag toggled by `mode test` / `mode run`, plus the USB bring-up snapshot the
// `query usb` verb reports. See docs/dev/terminal-dispatch.md.

#include <cstdint>

#include "terminal/usb_diag.h"

namespace spotykach {

struct TermState {
    bool     test_mode = false;   // false at boot; set by `mode test`, cleared by `mode run`
    UsbDiag  usb;                 // captured by Terminal::init(); reported by `query usb`

    // Dead-man switch for `mode test`. Test mode freezes knobs/CV/gate, so a harness that dies or
    // disconnects mid-run leaves the instrument with no working physical input and no way back except a
    // power cycle. Terminal::process() reverts to `mode run` after this long without a command. Any
    // command resets it, so a live session is never interrupted.
    uint32_t last_cmd_ms = 0;     // System::GetNow() at the last dispatched line
};

}  // namespace spotykach
