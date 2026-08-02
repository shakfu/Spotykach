// serial.js - the WebSerial transport.
//
// The thin part, as promised by docs/dev/web-frontend.md: the device enumerates as a standard USB CDC
// port (VID 0x0483, per tools/skdev/protocol.py), so there is no driver, no pyserial, and no baud
// question - the rate is cosmetic over CDC and pyserial only demands a number because its API does.
// A TextDecoderStream over port.readable plus the newline framer in framing.js is most of the client.
//
// Chromium only. That is the constraint the card builder was designed to degrade around, but there is
// no zip-shaped fallback for talking to hardware, so this half of the app simply reports that the
// browser cannot do it.

import { LineAssembler } from './framing.js';

export const DAISY_VID = 0x0483; // STMicroelectronics (Daisy Seed CDC); PID 0x5740 typical
export const BAUD = 115200; // ignored by USB CDC; the API requires a value

// Check the value, not the key: `'serial' in navigator` is true on a browser that declares the
// property and leaves it undefined, which would hide the "this browser cannot do it" notice behind a
// Connect button that then throws.
export const supported = () => typeof navigator !== 'undefined' && navigator.serial != null;

/**
 * Prompt for a port and open it. Must be called from a user gesture.
 *
 * `filtered` narrows the chooser to the Daisy's vendor id, which is the right default - it saves
 * picking the Daisy out of a list of bluetooth modems. But a filter that matches nothing produces an
 * EMPTY chooser, and cancelling an empty chooser throws the same `NotFoundError` as cancelling a full
 * one, so the app cannot tell "no device" from "changed my mind" and the user is told neither. Anything
 * that puts a different bridge in front of the CDC endpoint - a board revision, a hub, a USB-serial
 * adapter on the panel jack - lands there with no way out, so callers can retry unfiltered.
 *
 * @param {{filtered?: boolean}} [opts]
 * @returns {Promise<SerialTransport>}
 */
export async function requestPort({ filtered = true } = {}) {
  if (!supported()) {
    throw new Error('This browser has no WebSerial. Use Chrome, Edge or another Chromium browser.');
  }
  const port = await navigator.serial.requestPort(
    filtered ? { filters: [{ usbVendorId: DAISY_VID }] } : {});
  await port.open({ baudRate: BAUD });
  // Some hosts and USB-serial bridges gate output on DTR. For this device it is cosmetic - the
  // firmware's CDC control handler ignores line state - but assert it explicitly anyway.
  try {
    await port.setSignals({ dataTerminalReady: true });
  } catch {
    // Not fatal; not every platform implements setSignals.
  }
  return new SerialTransport(port);
}

export class SerialTransport {
  constructor(port) {
    this.port = port;
    this.assembler = new LineAssembler();
    this._onLine = () => {};
    this._onClose = () => {};
    this._closed = false;
    this._reader = null;
    this._pump();
  }

  onLine(cb) {
    this._onLine = cb;
  }

  /**
   * Called once if the port goes away on its own - unplugged, or reset into the bootloader.
   *
   * Without it the loss is silent in the worst way: the read loop ends, but the UI still shows a
   * connected device, the command line stays enabled, and the CPU poll keeps firing commands that now
   * time out three seconds at a time. A deliberate close() sets `_closed` first, so it does not come
   * back through here and cannot recurse into the caller's teardown.
   */
  onClose(cb) {
    this._onClose = cb;
  }

  async _pump() {
    const decoder = new TextDecoder();
    this._reader = this.port.readable.getReader();
    let reason = 'the port closed';
    try {
      for (;;) {
        const { value, done } = await this._reader.read();
        if (done) break;
        // stream: true so a multi-byte sequence split across two USB packets is not mangled.
        for (const line of this.assembler.push(decoder.decode(value, { stream: true }))) {
          this._onLine(line);
        }
      }
    } catch (e) {
      reason = e.message;
      if (!this._closed) this._onLine(`[transport] read failed: ${e.message}`);
    } finally {
      try {
        this._reader.releaseLock();
      } catch {
        /* already released */
      }
      if (!this._closed) this._onClose(reason);
    }
  }

  async write(text) {
    const writer = this.port.writable.getWriter();
    try {
      await writer.write(new TextEncoder().encode(text));
    } finally {
      writer.releaseLock();
    }
  }

  async close() {
    this._closed = true;
    try {
      await this._reader?.cancel();
    } catch {
      /* the pump may already have exited */
    }
    await this.port.close();
  }

  info() {
    const i = this.port.getInfo?.() || {};
    const hex = (v) => (v == null ? '?' : `0x${v.toString(16).padStart(4, '0')}`);
    return `USB ${hex(i.usbVendorId)}:${hex(i.usbProductId)}`;
  }
}
