// terminal_model.ts - the device session: connect, send, poll, disconnect.
//
// The largest of the view-models and the one that gained most from being pulled out of its view. The
// terminal has real state - a connection, a descriptor, a bounded console, a CPU history, a poll timer
// - and every interesting question about it used to need a browser AND a device. Now the transport is
// a port and the timer is a port, so "does an empty port chooser explain itself" and "does an unplugged
// device tear the session down" are ordinary unit tests.

import { Device, type CpuReading } from '../core/device.ts';
import { isDestructive, parseDescribe, parseUsbDiag, CommandError, Timeout, type Descriptor }
  from '../core/protocol.ts';
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
};

export class TerminalModel {
  readonly store = new Store<TerminalState>({ ...INITIAL });
  private device: Device | null = null;
  private stopPoll: (() => void) | null = null;

  constructor(private readonly deps: TerminalDeps) {}

  supported(): boolean {
    return this.deps.serial.supported();
  }

  write(text: string, kind: LineKind = 'meta'): void {
    const lines = [...this.store.get().lines, { text, kind }];
    // Bounded: a device that logs steadily must not grow the page without limit.
    this.store.set({ lines: lines.slice(-CONSOLE_LIMIT) });
  }

  async connect({ filtered = true } = {}): Promise<boolean> {
    try {
      const transport = await this.deps.serial.request({ filtered });
      // Registered before the first command: a device unplugged during `describe` must not leave the
      // session claiming a connection it no longer has.
      transport.onClose((why) => this.lost(why));
      this.device = new Device(transport, { logSink: (l) => this.write(l, 'log') });
      this.store.set({
        connected: true,
        port: transport.info(),
        status: `connected (${transport.info()})`,
        offerAllPorts: false,
        error: null,
      });
      this.write('connected', 'meta');
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
    if (isDestructive(line) && !this.deps.confirm(`Send "${line}"?`)) {
      this.write(`cancelled: ${line}`, 'meta');
      return null;
    }
    if (!quiet) this.write(`> ${line}`, 'sent');
    try {
      const reply = await this.device.cmd(line);
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

  async refreshDescribe(): Promise<void> {
    if (!this.device) return;
    try {
      this.store.set({ descriptor: parseDescribe(await this.device.describeLines()) });
    } catch {
      // A build without TERMINAL=1 never answers; the caveat at the top of the tab already says so.
      this.store.set({ descriptor: null });
    }
  }

  async refreshUsb(): Promise<void> {
    const reply = await this.send('query usb', { quiet: true });
    if (reply == null) {
      this.store.set({ usb: [], usbAvailable: false });
      return;
    }
    this.store.set({ usb: parseUsbDiag(reply), usbAvailable: true });
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
    await this.send('reset cpu');
    this.store.set({ cpuHistory: [] });
  }
}
