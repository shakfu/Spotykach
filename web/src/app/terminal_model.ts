// terminal_model.ts - the device session: connect, send, poll, disconnect.
//
// The largest of the view-models and the one that gained most from being pulled out of its view. The
// terminal has real state - a connection, a descriptor, a bounded console, a CPU history, a poll timer
// - and every interesting question about it used to need a browser AND a device. Now the transport is
// a port and the timer is a port, so "does an empty port chooser explain itself" and "does an unplugged
// device tear the session down" are ordinary unit tests.

import { Device, type CpuReading } from '../core/device.ts';
import { OscDevice } from '../core/oscdevice.ts';
import { lineClient, oscClient, type Codec, type DeviceClient } from '../core/client.ts';
import { parseUsbDiag, CommandError, Timeout, type Descriptor } from '../core/protocol.ts';
import type { Clock, SerialPorts } from '../core/ports.ts';
import { Store } from './store.ts';

export const CPU_HISTORY = 240; // samples kept in the plot
const CONSOLE_LIMIT = 500;
const POLL_MS = 500;

export type LineKind = 'sent' | 'ok' | 'err' | 'log' | 'meta';

export interface ConsoleLine {
  text: string;
  kind: LineKind;
}

export interface UsbRow {
  key: string;
  value: string;
}

export interface TerminalState {
  connected: boolean;
  /** Port identification, e.g. `USB 0x0483:0x5740`. Empty when disconnected. */
  port: string;
  status: string;
  lines: ConsoleLine[];
  descriptor: Descriptor | null;
  cpu: CpuReading | null;
  cpuHistory: number[];
  cpuAvailable: boolean;
  polling: boolean;
  usb: UsbRow[];
  usbAvailable: boolean;
  /** Set once a filtered chooser came back empty, to reveal the unfiltered retry. */
  offerAllPorts: boolean;
  error: string | null;
  /**
   * Which codec the NEXT connection will use, and which the current one does.
   *
   * A property of the firmware rather than of the session: an `OSC=1` build never speaks lines and a
   * line build never speaks SLIP, so this is chosen before connecting and cannot be toggled during a
   * session. Getting it wrong produces a connection that never answers, which is why the UI says
   * which it picked rather than leaving the user to infer it from silence.
   */
  codec: Codec;
  /** What the console accepts in this codec, shown as the input's placeholder. */
  example: string;
}

export interface TerminalDeps {
  serial: SerialPorts;
  clock: Clock;
  /** Asks the user to confirm a destructive verb. Injected so a test can answer without a dialog. */
  confirm: (what: string) => boolean;
}

const INITIAL: TerminalState = {
  connected: false, port: '', status: '', lines: [], descriptor: null,
  cpu: null, cpuHistory: [], cpuAvailable: true, polling: false,
  usb: [], usbAvailable: true, offerAllPorts: false, error: null,
  codec: 'line', example: 'set param speed a 0.5',
};

export class TerminalModel {
  readonly store = new Store<TerminalState>({ ...INITIAL });
  private device: DeviceClient | null = null;
  private stopPoll: (() => void) | null = null;

  constructor(private readonly deps: TerminalDeps) {}

  supported(): boolean {
    return this.deps.serial.supported();
  }

  /** Whether this build of the page can open an OSC session at all (see `SerialPorts.requestFrames`). */
  oscSupported(): boolean {
    return this.deps.serial.requestFrames != null;
  }

  /** Choose the codec for the next connection. Ignored while connected - it is not a live toggle. */
  setCodec(codec: Codec): void {
    if (this.store.get().connected) return;
    this.store.set({ codec, example: codec === 'osc' ? '/sk/a/param/speed 0.5' : 'set param speed a 0.5' });
  }

  write(text: string, kind: LineKind = 'meta'): void {
    const lines = [...this.store.get().lines, { text, kind }];
    // Bounded: a device that logs steadily must not grow the page without limit.
    this.store.set({ lines: lines.slice(-CONSOLE_LIMIT) });
  }

  async connect({ filtered = true } = {}): Promise<boolean> {
    const codec = this.store.get().codec;
    try {
      // Registered before the first command in both branches: a device unplugged during `describe`
      // must not leave the session claiming a connection it no longer has.
      const log = (l: string): void => this.write(l, 'log');
      let transport;
      if (codec === 'osc') {
        const requestFrames = this.deps.serial.requestFrames;
        if (!requestFrames) throw new Error('this page cannot open an OSC session');
        transport = await requestFrames({ filtered });
        transport.onClose((why) => this.lost(why));
        this.device = oscClient(new OscDevice(transport, { logSink: log }));
      } else {
        transport = await this.deps.serial.request({ filtered });
        transport.onClose((why) => this.lost(why));
        this.device = lineClient(new Device(transport, { logSink: log }));
      }
      this.store.set({
        connected: true,
        port: transport.info(),
        status: `connected (${transport.info()}, ${codec === 'osc' ? 'OSC' : 'line'} codec)`,
        example: this.device.example,
        offerAllPorts: false,
        error: null,
      });
      this.write(`connected using the ${codec === 'osc' ? 'OSC' : 'line'} codec`, 'meta');
      await this.refreshDescribe();
      await this.refreshUsb();
      this.startPolling();
      return true;
    } catch (e) {
      const err = e as Error;
      // NotFoundError means "no port came back", which is BOTH a cancelled chooser and an empty one -
      // WebSerial does not distinguish them. Returning silently is wrong for the empty case: a user
      // whose device enumerates under another vendor id gets no dialog worth reading and no message,
      // which is indistinguishable from the tab being broken.
      if (err.name === 'NotFoundError') {
        if (filtered) {
          this.write('no port chosen. If the chooser was empty, nothing on this machine is reporting '
            + 'the Daisy\'s USB vendor id - check the device is on and running a TERMINAL=1 build, or '
            + 'list every serial port instead.', 'meta');
          this.store.set({ offerAllPorts: true });
        } else {
          this.write('no port chosen.', 'meta');
        }
        return false;
      }
      this.store.set({ error: err.message, status: '' });
      return false;
    }
  }

  /** The port went away by itself. Tear down exactly as a manual disconnect would, and say why. */
  private lost(why: string): void {
    if (!this.device) return;
    this.write(`device disconnected: ${why}`, 'err');
    void this.disconnect();
  }

  async disconnect(): Promise<void> {
    this.stopPolling();
    try {
      await this.device?.close();
    } catch {
      /* the port may already be gone */
    }
    this.device = null;
    this.store.set({
      connected: false, port: '', status: 'disconnected', descriptor: null,
      cpu: null, cpuHistory: [], usb: [],
    });
    this.write('disconnected', 'meta');
  }

  /**
   * Send one command line. Destructive verbs are confirmed first - docs/dev/terminal-target-b.md
   * flags that sweeping a control surface can clear a recorded buffer or write the card.
   *
   * @returns the reply payload, or null on refusal, error or timeout
   */
  async send(line: string, { quiet = false } = {}): Promise<string | null> {
    if (!this.device) return null;
    if (this.device.destructive(line) && !this.deps.confirm(`Send "${line}"?`)) {
      this.write(`cancelled: ${line}`, 'meta');
      return null;
    }
    if (!quiet) this.write(`> ${line}`, 'sent');
    try {
      const reply = await this.device.exec(line);
      if (!quiet) this.write(reply === '' ? 'ok' : `ok ${reply}`, 'ok');
      return reply;
    } catch (e) {
      if (!quiet) {
        this.write(e instanceof CommandError ? `err ${e.reason}`
          : e instanceof Timeout ? 'timeout - no reply' : String(e), 'err');
      }
      return null;
    }
  }

  // --- operations the generated control surface drives ----------------------
  //
  // These exist so the surface stops composing command strings. `set param speed A 0.5` is a
  // line-codec sentence; against OSC it is not a different spelling of the same request but no
  // request at all, so a surface built from strings silently stops working the moment the codec
  // changes - and stops working by sending nothing, which looks like a dead device.
  //
  // The console echo is a DESCRIPTION, not wire text: `speed A = 0.5` rather than either codec's
  // spelling of it. Echoing one codec's syntax for an action the other performed would be worse than
  // not echoing at all, and the free-text console above still shows exactly what was typed.

  /** Run one named operation, with the console echo and error handling `send()` gives a typed line. */
  private async perform<T>(label: string, fn: (d: DeviceClient) => Promise<T>,
    { quiet = false, confirmAs = '' } = {}): Promise<T | null> {
    const device = this.device;
    if (!device) return null;
    if (confirmAs && !this.deps.confirm(`Send "${confirmAs}"?`)) {
      this.write(`cancelled: ${label}`, 'meta');
      return null;
    }
    if (!quiet) this.write(`> ${label}`, 'sent');
    try {
      const out = await fn(device);
      if (!quiet) this.write(out === '' || out === undefined ? 'ok' : `ok ${out}`, 'ok');
      return out;
    } catch (e) {
      if (!quiet) {
        this.write(e instanceof CommandError ? `err ${e.reason}`
          : e instanceof Timeout ? 'timeout - no reply' : String(e), 'err');
      }
      return null;
    }
  }

  setParam(name: string, deck: string, value: number, { quiet = false } = {}): Promise<void | null> {
    return this.perform(`${name} ${deck} = ${value}`, (d) => d.setParam(name, deck, value), { quiet });
  }

  getParam(name: string, deck: string, { quiet = false } = {}): Promise<number | null> {
    return this.perform(`read ${name} ${deck}`, (d) => d.getParam(name, deck), { quiet });
  }

  setConfig(name: string, deck: string, value: number): Promise<void | null> {
    return this.perform(`${name} ${deck} = ${value}`, (d) => d.setConfig(name, deck, value));
  }

  gate(deck: string): Promise<void | null> {
    return this.perform(`gate ${deck}`, (d) => d.gate(deck));
  }

  /** Press a pad. `clear` is destructive and is confirmed, exactly as the typed line would be. */
  pad(action: string, deck: string): Promise<string | null> {
    return this.perform(`pad ${action} ${deck}`, (d) => d.pad(action, deck),
      { confirmAs: action === 'clear' ? `pad clear ${deck}` : '' });
  }

  /** Read one state value, rendered as text in either codec. */
  queryValue(name: string, deck: string): Promise<string | null> {
    return this.perform(`query ${name} ${deck}`.trimEnd(), (d) => d.query(name, deck), { quiet: true });
  }

  async refreshDescribe(): Promise<void> {
    if (!this.device) return;
    try {
      this.store.set({ descriptor: await this.device.describe() });
    } catch {
      // A build without TERMINAL=1 never answers, and neither does one built for the other codec;
      // the caveat at the top of the tab already says so.
      this.store.set({ descriptor: null });
    }
  }

  async refreshUsb(): Promise<void> {
    // Through the client's `query`, not a hand-written command line: the two codecs spell this
    // differently (`query usb` against `/sk/dev/usb`) and the model has no business knowing which.
    if (!this.device) return;
    try {
      this.store.set({ usb: parseUsbDiag(await this.device.query('usb')), usbAvailable: true });
    } catch {
      this.store.set({ usb: [], usbAvailable: false });
    }
  }

  // --- CPU meter ------------------------------------------------------------
  //
  // The reason this is a history and not three numbers: the P2 bench workflow in TODO.md is "read the
  // numbers repeatedly and notice whether max is still climbing". A rising max is the signal that
  // matters, and a plot answers convergence at a glance - which is precisely the question that
  // mattered for pstretch at 8192.

  startPolling(): void {
    this.stopPolling();
    this.stopPoll = this.deps.clock.every(POLL_MS, () => void this.pollCpu());
    this.store.set({ polling: true });
    void this.pollCpu();
  }

  stopPolling(): void {
    this.stopPoll?.();
    this.stopPoll = null;
    this.store.set({ polling: false });
  }

  togglePolling(): void {
    if (this.store.get().polling) this.stopPolling();
    else this.startPolling();
  }

  async pollCpu(): Promise<void> {
    if (!this.device) return;
    try {
      const cpu = await this.device.cpu();
      const cpuHistory = [...this.store.get().cpuHistory, cpu.avg].slice(-CPU_HISTORY);
      this.store.set({ cpu, cpuHistory, cpuAvailable: true });
    } catch {
      // A build without TERMINAL=1 answers `err unknown-verb`; stop hammering it.
      this.stopPolling();
      this.store.set({ cpuAvailable: false });
    }
  }

  async resetCpu(): Promise<void> {
    if (!this.device) return;
    this.write('> reset cpu', 'sent');
    try {
      await this.device.resetCpu();
      this.write('ok', 'ok');
    } catch (e) {
      this.write(e instanceof CommandError ? `err ${e.reason}` : String(e), 'err');
      return;
    }
    this.store.set({ cpuHistory: [] });
  }
}
