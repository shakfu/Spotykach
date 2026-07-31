// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#include "terminal/usb_diag.h"

#if SPK_TERMINAL

#if defined(__arm__)
#include "sys/system.h"       // daisy::System - bootloader version / program memory region
#include "stm32h7xx_hal.h"    // RCC / PWR / USB_OTG register definitions + HAL_PWREx_*
#endif

#pragma GCC optimize("Os")

namespace spotykach {

#if !defined(__arm__)

// Host build (host/test_terminal.cpp): there is no OTG peripheral to read, but the dispatcher links
// against these because `query usb` refreshes before reporting. Provide inert definitions so the
// off-target harness can exercise the codec/dispatch layers - the fields keep their defaults, which is
// exactly what a host test should see.
void usb_diag_capture_pre(UsbDiag&) {}
void usb_diag_capture_post(UsbDiag& d) { usb_diag_refresh(d); }
void usb_diag_refresh(UsbDiag&) {}
bool usb_supply_bringup() { return false; }

#else

namespace {

// The core this build talks to, and the pads it drives. OTG_HS is used in its embedded full-speed
// mode (PCD_SPEED_FULL / USB_OTG_EMBEDDED_PHY, see usbd_conf.c), so the register semantics below are
// identical for both - only the base address, the GPIO port/pins and the alternate function differ.
#if SPK_TERMINAL_PORT_EXTERNAL
#define SPK_OTG_CORE   USB_OTG_HS
static constexpr uint32_t kDmPin = 14;   // PB14 = USB_OTG_HS_DM
static constexpr uint32_t kDpPin = 15;   // PB15 = USB_OTG_HS_DP
static constexpr uint32_t kUsbAf = 0xC;  // GPIO_AF12_OTG2_FS
#define SPK_OTG_GPIO   GPIOB
#else
#define SPK_OTG_CORE   USB_OTG_FS
static constexpr uint32_t kDmPin = 11;   // PA11 = USB_OTG_FS_DM
static constexpr uint32_t kDpPin = 12;   // PA12 = USB_OTG_FS_DP
static constexpr uint32_t kUsbAf = 0xA;  // GPIO_AF10_OTG1_FS
#define SPK_OTG_GPIO   GPIOA
#endif

// The device-mode register block sits at a fixed offset inside the core's address space.
inline USB_OTG_DeviceTypeDef* otg_device() {
    return reinterpret_cast<USB_OTG_DeviceTypeDef*>(reinterpret_cast<uint32_t>(SPK_OTG_CORE)
                                                    + USB_OTG_DEVICE_BASE);
}

// True if `pin` on the USB GPIO port is still in alternate-function mode with the USB AF selected.
inline bool pad_owned_by_usb(uint32_t pin) {
    const bool alt = ((SPK_OTG_GPIO->MODER >> (pin * 2u)) & 0x3u) == 0x2u;
    const bool af  = ((SPK_OTG_GPIO->AFR[pin >> 3u] >> ((pin & 0x7u) * 4u)) & 0xFu) == kUsbAf;
    return alt && af;
}

}  // namespace

void usb_diag_capture_pre(UsbDiag& d) {
    const auto boot   = daisy::System::GetBootloaderVersion();
    const auto region = daisy::System::GetProgramMemoryRegion();
    d.boot_version  = static_cast<uint8_t>(boot);
    d.memory_region = static_cast<uint8_t>(region);

    // Mirrors the condition in daisy_seed.cpp that sets syscfg.skip_clocks: when it holds,
    // System::Init() never runs ConfigureClocks(), which is the only place HSI48 is enabled and
    // RCC_USBCLKSOURCE_HSI48 is selected. In that case the USB clock is whatever the bootloader left.
    d.clocks_configured = !(boot == daisy::System::BootInfo::Version::LT_v6_0
                            && region != daisy::System::MemoryRegion::INTERNAL_FLASH);

    d.hsi48_ready    = (RCC->CR & RCC_CR_HSI48RDY) != 0;
    d.usb_clk_source = static_cast<uint8_t>((RCC->D2CCIP2R & RCC_D2CCIP2R_USBSEL)
                                            >> RCC_D2CCIP2R_USBSEL_Pos);   // 3 == HSI48
    d.usb33_detector = (PWR->CR3 & PWR_CR3_USB33DEN) != 0;
    d.usb33_ready    = (PWR->CR3 & PWR_CR3_USB33RDY) != 0;
}

void usb_diag_capture_post(UsbDiag& d) {
    usb_diag_refresh(d);
}

void usb_diag_refresh(UsbDiag& d) {
    d.transceiver_on  = (SPK_OTG_CORE->GCCFG & USB_OTG_GCCFG_PWRDWN) != 0;
    d.pullup_asserted = (otg_device()->DCTL & USB_OTG_DCTL_SDIS) == 0;
    d.vbus_sensing    = (SPK_OTG_CORE->GCCFG & USB_OTG_GCCFG_VBDEN) != 0;

    // Pad ownership: the HAL's HAL_PCD_MspInit puts D-/D+ into alternate-function mode with the USB
    // AF. If either reverts to plain GPIO, the PHY is no longer driving the pad and no pullup reaches
    // the connector, no matter what DCTL says.
    d.dm_af_ok = pad_owned_by_usb(kDmPin);
    d.dp_af_ok = pad_owned_by_usb(kDpPin);

    // Host activity, sticky: the interrupt handler clears GINTSTS, so latch anything we catch. Absence
    // of both means the host has never addressed this device - i.e. it never saw an attach.
    const uint32_t gintsts = SPK_OTG_CORE->GINTSTS;
    if (gintsts & USB_OTG_GINTSTS_USBRST) d.usb_reset_seen = true;
    if (gintsts & USB_OTG_GINTSTS_SOF)    d.sof_seen       = true;
}

bool usb_supply_bringup() {
    HAL_PWREx_EnableUSBVoltageDetector();
    // Bounded spin - a few milliseconds at 480 MHz. Never block boot on a supply that never comes up;
    // the caller proceeds either way and the diag records the outcome.
    for (uint32_t spins = 0; spins < 1000000u; ++spins) {
        if (PWR->CR3 & PWR_CR3_USB33RDY) return true;
    }
    return (PWR->CR3 & PWR_CR3_USB33RDY) != 0;
}

#endif  // __arm__

}  // namespace spotykach

#endif  // SPK_TERMINAL
