// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// USB bring-up probe for the terminal channel. See docs/dev/terminal-impl.md.
//
// Why this exists: on-hardware bring-up found the app's USB device never enumerating, while the DFU
// bootloader enumerated fine on the same cable and port - and the channel that would normally report
// on itself was the very thing that was down. This header captures the registers that decide whether
// a device can appear at all, so the answer can be read out by other means (the TERM_USBDIAG panel /
// LED readout, or `query usb` once the port works).
//
// It earned its keep: the fault was that the channel was on the wrong OTG core entirely, and the
// signature that identified it - a completely healthy core, pads still in the USB alternate function,
// and zero host activity - is only visible if you can read all of those at once, live.
//
// The clock entries matter more than they look. libDaisy only configures HSI48 and the USB clock
// source inside System::ConfigureClocks() (sys/system.cpp), and daisy_seed.cpp sets
// syscfg.skip_clocks = true whenever the bootloader is < v6.0 AND the program runs from somewhere
// other than internal flash. Every build in this repo is APP_TYPE=BOOT_SRAM, so whether the app
// configures the USB clock at all is a runtime property of the installed bootloader, not something
// the source can be read to decide. `clocks_configured` records which way it went.

#include <cstdint>

// Which OTG core this probe (and the channel) targets. 1 = OTG_HS-as-FS on PB14/PB15 (Seed pins
// D29/D30); 0 = OTG_FS on PA11/PA12 (the Daisy Seed's own USB connector).
//
// DEFAULT IS EXTERNAL, because that is where the Spotykach's panel USB-C jack is wired - verified on
// hardware 2026-07-31, after the channel spent two sessions talking to PA11/PA12, which this board
// does not connect to anything. The tell was a diagnostic reporting a completely healthy OTG_FS core
// (clocks, supply, transceiver, pullup asserted, pads still in AF10) alongside zero host activity.
// The same signal was in the tree all along: the Makefile routes the logger to LOGGER_EXTERNAL
// unconditionally, and this board runs libDaisy's `extdfu` bootloader, which serves DFU over OTG_HS.
//
// Build a bare Daisy Seed or Pod - whose USB connector really is OTG_FS - with `TERMPORT=int`.
#ifndef SPK_TERMINAL_PORT_EXTERNAL
#define SPK_TERMINAL_PORT_EXTERNAL 1
#endif

namespace spotykach {

// A snapshot of everything that has to be true before the host can see a D+ pullup. Every field is
// captured; nothing here changes behaviour. `ok()` is the expected-good verdict.
struct UsbDiag {
    // --- captured before UsbHandle::Init() ---
    uint8_t boot_version     = 0xFF;   // System::BootInfo::Version (0 = LT_v6_0, 1 = NONE, 2 = v6_0, ...)
    uint8_t memory_region    = 0xFF;   // System::MemoryRegion (0 = INTERNAL_FLASH)
    bool    clocks_configured = false; // false => System::Init took the skip_clocks path
    bool    hsi48_ready       = false; // RCC_CR.HSI48RDY
    uint8_t usb_clk_source    = 0xFF;  // RCC_D2CCIP2R.USBSEL (3 = HSI48, which is what libDaisy sets)
    bool    usb33_detector    = false; // PWR_CR3.USB33DEN - VDD33_USB level detector enabled
    bool    usb33_ready       = false; // PWR_CR3.USB33RDY - transceiver supply validated

    // --- captured after UsbHandle::Init(), then REFRESHED every main loop by usb_diag_refresh() ---
    // Refreshed because an init-time snapshot cannot see later damage: plenty runs after
    // Terminal::init() (TIM5, DAC, audio start, UI calibration), and any of it could re-assert SDIS,
    // power the transceiver down, or steal PA11/PA12 back from the USB alternate function.
    bool    transceiver_on    = false; // USB_OTG_FS GCCFG.PWRDWN set = transceiver powered
    bool    pullup_asserted   = false; // DCTL.SDIS clear = D+ pullup presented to the host
    bool    vbus_sensing      = true;  // GCCFG.VBDEN - core gates the pullup on VBUS at the sense pin

    // --- pin ownership: does the USB PHY still drive the D+/D- pads? -------------------------------
    // The core can believe it is connected while the pins have been handed back to GPIO, in which case
    // no pullup reaches the wire and the host sees nothing at all - which is exactly the symptom when
    // the registers otherwise look healthy.
    bool    dp_af_ok          = false; // PA12 (USB_OTG_FS_DP) in alternate-function mode, AF10
    bool    dm_af_ok          = false; // PA11 (USB_OTG_FS_DM) in alternate-function mode, AF10

    // --- host activity, sticky since boot ---------------------------------------------------------
    // Distinguishes "the host never saw us" from "the host talked to us and enumeration failed later".
    bool    usb_reset_seen    = false; // GINTSTS.USBRST - the host issued a bus reset
    bool    sof_seen          = false; // GINTSTS.SOF    - the host is sending frames

    // Expected-good: clocks live, supply validated, transceiver powered, pullup up.
    bool ok() const {
        return hsi48_ready && usb_clk_source == 3 && usb33_ready && transceiver_on && pullup_asserted;
    }
};

// Read the pre-Init half (clocks + transceiver supply). Safe to call before the USB device exists.
void usb_diag_capture_pre(UsbDiag& d);

// Read the post-Init half (GCCFG / DCTL). Only meaningful after UsbHandle::Init(FS_INTERNAL), since
// the OTG_FS peripheral clock is enabled by the HAL's MspInit.
void usb_diag_capture_post(UsbDiag& d);

// Re-read everything that can change after init - core state, pad ownership, and the sticky host-
// activity flags - and OR the activity bits in. Cheap (a handful of register reads); call it from the
// main loop so the readout reflects now, not boot.
void usb_diag_refresh(UsbDiag& d);

// Enable the VDD33_USB level detector and wait (bounded) for USB33RDY. libDaisy's UsbHandle::Init
// calls HAL_PWREx_EnableUSBVoltageDetector() *after* InitFS() - i.e. after USBD_Start has already
// asserted DevConnect - and never waits for the ready flag, so the core can be told to connect before
// its supply is validated. Calling this first removes that ordering hazard. Returns USB33RDY.
bool usb_supply_bringup();

}  // namespace spotykach
