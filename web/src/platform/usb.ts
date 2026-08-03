// usb.ts - WebUSB, wearing the DfuDevice port.
//
// The thin part, like serial.ts: everything that decides anything is in core/dfu.ts and
// core/image.ts. This file is four control transfers and the descriptor walk that finds which
// interface to send them to.
//
// Chromium only, and for the same reason the Terminal tab is: WebUSB is not implemented in Firefox or
// Safari and is not going to be. There is no zip-shaped fallback for talking to hardware, so this half
// of the app reports that the browser cannot do it and points at dfu-util.
//
// One platform note that costs people an afternoon: on Linux, WebUSB can only claim an interface the
// kernel has not already bound, and a udev rule is needed for a non-root browser to open the device at
// all. The view says so rather than showing a chooser that is mysteriously empty - Electro-Smith ship
// the rule, and it is the same one dfu-util needs.

import type { DfuDevice, DfuStatus, UsbDfu } from '../core/ports.ts';
import { DFU_ABORT, DFU_CLRSTATUS, DFU_DNLOAD, DFU_GETSTATUS, DFU_UPLOAD } from '../core/dfu.ts';

/** STMicroelectronics, and the PID both the ST ROM loader and the Daisy bootloader present. */
export const DFU_VID = 0x0483;
export const DFU_PID = 0xdf11;

/** USB DFU is class 0xFE ("application specific"), subclass 1. Interface protocol 2 is DFU mode. */
const DFU_CLASS = 0xfe;
const DFU_SUBCLASS = 1;

// Minimal shapes for WebUSB, which is not in the standard DOM lib.
interface UsbAlternate {
  alternateSetting: number;
  interfaceClass: number;
  interfaceSubclass: number;
  interfaceProtocol: number;
  interfaceName?: string;
}
interface UsbInterface {
  interfaceNumber: number;
  alternate: UsbAlternate;
  alternates: UsbAlternate[];
}
interface UsbConfiguration {
  interfaces: UsbInterface[];
}
interface UsbInTransferResult {
  data?: DataView;
  status?: string;
}
interface UsbOutTransferResult {
  status?: string;
  bytesWritten?: number;
}
interface UsbControlSetup {
  requestType: 'standard' | 'class' | 'vendor';
  recipient: 'device' | 'interface' | 'endpoint' | 'other';
  request: number;
  value: number;
  index: number;
}
interface UsbDeviceLike {
  productName?: string;
  vendorId: number;
  productId: number;
  configuration: UsbConfiguration | null;
  opened: boolean;
  open(): Promise<void>;
  close(): Promise<void>;
  selectConfiguration(n: number): Promise<void>;
  claimInterface(n: number): Promise<void>;
  releaseInterface(n: number): Promise<void>;
  selectAlternateInterface(iface: number, alt: number): Promise<void>;
  controlTransferIn(setup: UsbControlSetup, length: number): Promise<UsbInTransferResult>;
  controlTransferOut(setup: UsbControlSetup, data?: Uint8Array): Promise<UsbOutTransferResult>;
}
interface UsbLike {
  requestDevice(opts: { filters: Array<{ vendorId: number; productId?: number }> }): Promise<UsbDeviceLike>;
}
type UsbNavigator = Navigator & { usb?: UsbLike };

/**
 * Check the value, not the key - same trap as WebSerial. `'usb' in navigator` is true on a browser
 * that declares the property and leaves it undefined, which hides the "this browser cannot" notice
 * behind a button that then throws.
 */
export const supported = (): boolean =>
  typeof navigator !== 'undefined' && (navigator as UsbNavigator).usb != null;

/** The DFU interface, and the alt setting that maps the region we mean to write. */
function findDfuInterface(dev: UsbDeviceLike): { iface: number; alt: number } {
  const config = dev.configuration;
  if (!config) throw new Error('the device offered no USB configuration');
  for (const iface of config.interfaces) {
    for (const alt of iface.alternates) {
      if (alt.interfaceClass === DFU_CLASS && alt.interfaceSubclass === DFU_SUBCLASS) {
        // Alt 0 only, deliberately. On this hardware alt 0 is what `dfu-util -a 0` targets and what
        // the bootloader maps QSPI through; the higher alt settings on an ST ROM loader are internal
        // flash and the option bytes, and this page has no business writing either.
        if (alt.alternateSetting === 0) {
          return { iface: iface.interfaceNumber, alt: alt.alternateSetting };
        }
      }
    }
  }
  throw new Error('no DFU interface on that device - is it in bootloader mode?');
}

class WebUsbDfuDevice implements DfuDevice {
  constructor(
    private dev: UsbDeviceLike,
    private iface: number,
    private xfer: number,
  ) {}

  private setup(request: number, value: number): UsbControlSetup {
    return { requestType: 'class', recipient: 'interface', request, value, index: this.iface };
  }

  async download(block: number, data: Uint8Array): Promise<void> {
    // A zero-length download is the manifest command and must still be sent, so the empty case is not
    // short-circuited away.
    const res = await this.dev.controlTransferOut(this.setup(DFU_DNLOAD, block), data);
    if (res.status && res.status !== 'ok') throw new Error(`download stalled (${res.status})`);
  }

  async upload(block: number, length: number): Promise<Uint8Array> {
    const res = await this.dev.controlTransferIn(this.setup(DFU_UPLOAD, block), length);
    if (res.status && res.status !== 'ok') throw new Error(`upload stalled (${res.status})`);
    if (!res.data) return new Uint8Array(0);
    return new Uint8Array(res.data.buffer, res.data.byteOffset, res.data.byteLength);
  }

  async getStatus(): Promise<DfuStatus> {
    const res = await this.dev.controlTransferIn(this.setup(DFU_GETSTATUS, 0), 6);
    if (!res.data || res.data.byteLength < 6) throw new Error('short GETSTATUS response');
    const d = res.data;
    // bwPollTimeout is a 24-bit little-endian field spanning bytes 1..4.
    const pollTimeout = d.getUint8(1) | (d.getUint8(2) << 8) | (d.getUint8(3) << 16);
    return { status: d.getUint8(0), state: d.getUint8(4), pollTimeout };
  }

  async clearStatus(): Promise<void> {
    await this.dev.controlTransferOut(this.setup(DFU_CLRSTATUS, 0));
  }

  async abort(): Promise<void> {
    await this.dev.controlTransferOut(this.setup(DFU_ABORT, 0));
  }

  async close(): Promise<void> {
    // Both can throw if the device already left DFU and reset itself, which is the normal end of a
    // successful flash rather than a fault.
    try { await this.dev.releaseInterface(this.iface); } catch { /* already gone */ }
    try { await this.dev.close(); } catch { /* already gone */ }
  }

  transferSize(): number {
    return this.xfer;
  }

  info(): string {
    const name = this.dev.productName || 'DFU device';
    const id = `${this.dev.vendorId.toString(16).padStart(4, '0')}:` +
      `${this.dev.productId.toString(16).padStart(4, '0')}`;
    return `${name} ${id}`;
  }
}

/**
 * Prompt for a DFU device, claim its interface, and hand back the port.
 *
 * Filtered to 0483:df11. An unfiltered chooser here would list every USB device on the machine and
 * invite someone to pick a webcam, and unlike the serial chooser there is no benign outcome to that.
 */
export async function request(): Promise<DfuDevice> {
  const usb = (navigator as UsbNavigator).usb;
  if (!usb) throw new Error('this browser has no WebUSB');

  const dev = await usb.requestDevice({ filters: [{ vendorId: DFU_VID, productId: DFU_PID }] });
  await dev.open();
  if (!dev.configuration) await dev.selectConfiguration(1);

  const { iface, alt } = findDfuInterface(dev);
  await dev.claimInterface(iface);
  await dev.selectAlternateInterface(iface, alt);

  // wTransferSize lives in the DFU functional descriptor, which WebUSB does not surface. 1024 is what
  // this bootloader advertises and what dfu-util negotiates with it; the protocol tolerates a smaller
  // value than the device's maximum, so a conservative constant is safe where a guess would not be.
  return new WebUsbDfuDevice(dev, iface, 1024);
}

export const webUsbDfu: UsbDfu = { supported, request };
