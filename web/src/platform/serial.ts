// serial.ts - the WebSerial transport.
//
// The thin part, as promised by docs/dev/web-frontend.md: the device enumerates as a standard USB CDC
// port (VID 0x0483, per tools/skdev/protocol.py), so there is no driver, no pyserial, and no baud
// question - the rate is cosmetic over CDC and pyserial only demands a number because its API does.
// A TextDecoder over port.readable plus the newline framer in core/protocol.ts is most of the client.
//
// Chromium only. That is the constraint the card builder was designed to degrade around, but there is
// no zip-shaped fallback for talking to hardware, so this half of the app simply reports that the
// browser cannot do it.

import { LineAssembler } from '../core/protocol.ts';
import { SlipDecoder, slipEncode } from '../core/osc.ts';
import type { FrameTransport, SerialPorts, Transport } from '../core/ports.ts';

export const DAISY_VID = 0x0483; // STMicroelectronics (Daisy Seed CDC); PID 0x5740 typical
export const BAUD = 115200; // ignored by USB CDC; the API requires a value

// Minimal shapes for the WebSerial API, which is not in the standard DOM lib.
interface SerialPortLike {
  readable: ReadableStream<Uint8Array>;
  writable: WritableStream<Uint8Array>;
  open(opts: { baudRate: number }): Promise<void>;
  close(): Promise<void>;
  setSignals?(opts: { dataTerminalReady: boolean }): Promise<void>;
  getInfo?(): { usbVendorId?: number; usbProductId?: number };
}
interface SerialLike {
  requestPort(opts?: { filters?: Array<{ usbVendorId: number }> }): Promise<SerialPortLike>;
}
type SerialNavigator = Navigator & { serial?: SerialLike };

/**
 * Check the value, not the key: `'serial' in navigator` is true on a browser that declares the
 * property and leaves it undefined, which would hide the "this browser cannot do it" notice behind a
 * Connect button that then throws.
 */
export const supported = (): boolean =>
  typeof navigator !== 'undefined' && (navigator as SerialNavigator).serial != null;

/**
 * Prompt for a port and open it. Must be called from a user gesture.
 *
 * `filtered` narrows the chooser to the Daisy's vendor id, which is the right default - it saves
 * picking the Daisy out of a list of bluetooth modems. But a filter that matches nothing produces an
 * EMPTY chooser, and cancelling an empty chooser throws the same `NotFoundError` as cancelling a full
 * one, so the app cannot tell "no device" from "changed my mind" and the user is told neither. Anything
 * that puts a different bridge in front of the CDC endpoint - a board revision, a hub, a USB-serial
 * adapter on the panel jack - lands there with no way out, so callers can retry unfiltered.
 */
async function openPort({ filtered = true } = {}): Promise<SerialPortLike> {
  if (!supported()) {
    throw new Error('This browser has no WebSerial. Use Chrome, Edge or another Chromium browser.');
  }
  const serial = (navigator as SerialNavigator).serial!;
  const port = await serial.requestPort(
    filtered ? { filters: [{ usbVendorId: DAISY_VID }] } : {});
  await port.open({ baudRate: BAUD });
  // Some hosts and USB-serial bridges gate output on DTR. For this device it is cosmetic - the
  // firmware's CDC control handler ignores line state - but assert it explicitly anyway.
  try {
    await port.setSignals?.({ dataTerminalReady: true });
  } catch {
    // Not fatal; not every platform implements setSignals.
  }
  return port;
}

export async function requestPort(opts: { filtered?: boolean } = {}): Promise<Transport> {
  return new SerialTransport(await openPort(opts));
}

/**
 * The same port, framed for the OSC codec.
 *
 * A separate entry point rather than a flag, because the codec is a property of the FIRMWARE, not of
 * the session: an `OSC=1` build never speaks lines and a line build never speaks SLIP, so choosing
 * wrong is not a mode to toggle mid-session but a connection that will not answer. The UI picks
 * before connecting and says which it picked.
 */
export async function requestFramePort(opts: { filtered?: boolean } = {}): Promise<FrameTransport> {
  return new OscSerialTransport(await openPort(opts));
}

export class SerialTransport implements Transport {
  private readonly assembler = new LineAssembler();
  private onLineCb: (line: string) => void = () => {};
  private onCloseCb: (reason: string) => void = () => {};
  private closed = false;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;

  constructor(private readonly port: SerialPortLike) {
    void this.pump();
  }

  onLine(cb: (line: string) => void): void {
    this.onLineCb = cb;
  }

  /**
   * Called once if the port goes away on its own - unplugged, or reset into the bootloader.
   *
   * Without it the loss is silent in the worst way: the read loop ends, but the UI still shows a
   * connected device, the command line stays enabled, and the CPU poll keeps firing commands that now
   * time out three seconds at a time. A deliberate close() sets `closed` first, so it does not come
   * back through here and cannot recurse into the caller's teardown.
   */
  onClose(cb: (reason: string) => void): void {
    this.onCloseCb = cb;
  }

  private async pump(): Promise<void> {
    const decoder = new TextDecoder();
    this.reader = this.port.readable.getReader();
    let reason = 'the port closed';
    try {
      for (;;) {
        const { value, done } = await this.reader.read();
        if (done) break;
        // stream: true so a multi-byte sequence split across two USB packets is not mangled.
        for (const line of this.assembler.push(decoder.decode(value, { stream: true }))) {
          this.onLineCb(line);
        }
      }
    } catch (e) {
      reason = (e as Error).message;
      if (!this.closed) this.onLineCb(`[transport] read failed: ${reason}`);
    } finally {
      try {
        this.reader.releaseLock();
      } catch {
        /* already released */
      }
      if (!this.closed) this.onCloseCb(reason);
    }
  }

  async write(text: string): Promise<void> {
    const writer = this.port.writable.getWriter();
    try {
      await writer.write(new TextEncoder().encode(text));
    } finally {
      writer.releaseLock();
    }
  }

  async close(): Promise<void> {
    this.closed = true;
    try {
      await this.reader?.cancel();
    } catch {
      /* the pump may already have exited */
    }
    await this.port.close();
  }

  info(): string {
    return portInfo(this.port);
  }
}

/**
 * The OSC transport: identical port handling, different framing.
 *
 * The duplication against `SerialTransport` is deliberate and small. Factoring the shared pump into a
 * base class would have to abstract over the one thing that differs - bytes in, units out - and the
 * result reads worse than two twenty-line pumps. Note the decoder is fed RAW bytes here, with no
 * TextDecoder anywhere: running a describe bundle through UTF-8 decoding would mangle exactly the
 * high bytes SLIP uses as delimiters.
 */
export class OscSerialTransport implements FrameTransport {
  private readonly decoder = new SlipDecoder();
  private onFrameCb: (packet: Uint8Array) => void = () => {};
  private onCloseCb: (reason: string) => void = () => {};
  private closed = false;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;

  constructor(private readonly port: SerialPortLike) {
    void this.pump();
  }

  onFrame(cb: (packet: Uint8Array) => void): void {
    this.onFrameCb = cb;
  }

  onClose(cb: (reason: string) => void): void {
    this.onCloseCb = cb;
  }

  private async pump(): Promise<void> {
    this.reader = this.port.readable.getReader();
    let reason = 'the port closed';
    try {
      for (;;) {
        const { value, done } = await this.reader.read();
        if (done) break;
        for (const frame of this.decoder.feed(value)) this.onFrameCb(frame);
      }
    } catch (e) {
      reason = (e as Error).message;
    } finally {
      try {
        this.reader.releaseLock();
      } catch {
        /* already released */
      }
      if (!this.closed) this.onCloseCb(reason);
    }
  }

  async send(packet: Uint8Array): Promise<void> {
    const writer = this.port.writable.getWriter();
    try {
      await writer.write(slipEncode(packet));
    } finally {
      writer.releaseLock();
    }
  }

  async close(): Promise<void> {
    this.closed = true;
    try {
      await this.reader?.cancel();
    } catch {
      /* the pump may already have exited */
    }
    await this.port.close();
  }

  info(): string {
    return portInfo(this.port);
  }
}

function portInfo(port: SerialPortLike): string {
  const i = port.getInfo?.() ?? {};
  const hex = (v: number | undefined): string =>
    (v == null ? '?' : `0x${v.toString(16).padStart(4, '0')}`);
  return `USB ${hex(i.usbVendorId)}:${hex(i.usbProductId)}`;
}

export const webSerial: SerialPorts = {
  supported,
  request: requestPort,
  requestFrames: requestFramePort,
};
