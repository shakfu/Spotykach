// device.ts - the command API, over any transport.
//
// A port of tools/skdev/device.py, and deliberately transport-agnostic for the same reason that file's
// tests are: the framing rules (one command in flight, skip interleaved `[tag]` log lines, discard
// stale bytes before sending) are where the bugs live, and they are testable against a scripted
// stream. The test suite drives this class with a fake device and no hardware at all, mirroring how
// tools/conftest.py skips cleanly when nothing is attached.

import { isLog, parseReply, CommandError, Timeout } from './protocol.ts';
import type { Transport } from './ports.ts';

/**
 * Default read timeout. Generous on purpose, exactly as in the Python client: the channel is pumped
 * from the firmware's main loop, so its latency is bounded by the SLOWEST main-loop consumer - a
 * streaming engine scanning the SD card in prepare() can stall replies for a good fraction of a second.
 */
export const DEFAULT_TIMEOUT_MS = 3000;

export interface DeviceOptions {
  timeout?: number;
  logSink?: (line: string) => void;
}

export interface CpuReading {
  avg: number;
  min: number;
  max: number;
}

interface Waiter {
  resolve: (line: string) => void;
  reject: (e: Error) => void;
  timer: ReturnType<typeof setTimeout> | null;
}

export class Device {
  private readonly timeout: number;
  private readonly logSink: ((line: string) => void) | null;
  private lines: string[] = [];
  private waiters: Waiter[] = [];
  /** Serializes commands; the protocol allows exactly one in flight. */
  private busy: Promise<void> | null = null;

  constructor(private readonly transport: Transport, opts: DeviceOptions = {}) {
    this.timeout = opts.timeout ?? DEFAULT_TIMEOUT_MS;
    this.logSink = opts.logSink ?? null;
    transport.onLine((line) => this.push(line));
  }

  private push(line: string): void {
    const w = this.waiters.shift();
    if (w) {
      if (w.timer) clearTimeout(w.timer);
      w.resolve(line);
    } else {
      this.lines.push(line);
    }
  }

  private readLine(): Promise<string> {
    const buffered = this.lines.shift();
    if (buffered !== undefined) return Promise.resolve(buffered);
    return new Promise<string>((resolve, reject) => {
      const waiter: Waiter = { resolve, reject, timer: null };
      waiter.timer = setTimeout(() => {
        const i = this.waiters.indexOf(waiter);
        if (i >= 0) this.waiters.splice(i, 1);
        reject(new Timeout('no reply'));
      }, this.timeout);
      this.waiters.push(waiter);
    });
  }

  /**
   * Discard anything already waiting before sending.
   *
   * The protocol is synchronous - one command in flight - so nothing should ever be pending here.
   * Anything that is, is either a late reply from a command that timed out or an unsolicited transport
   * error (`err overflow`). Without this, a SINGLE timeout offsets every subsequent reply for the life
   * of the session: each command reads the previous one's answer, and the failures then surface far
   * from their cause as nonsense parse errors.
   */
  private drainStale(): void {
    if (!this.lines.length) return;
    if (this.logSink) for (const ln of this.lines) this.logSink(`[stale] ${ln}`);
    this.lines = [];
  }

  private async exclusive<T>(fn: () => Promise<T>): Promise<T> {
    const prev = this.busy ?? Promise.resolve();
    let release!: () => void;
    this.busy = new Promise<void>((r) => { release = r; });
    await prev.catch(() => {});
    try {
      return await fn();
    } finally {
      release();
    }
  }

  private async readReply(): Promise<string> {
    for (;;) {
      const line = await this.readLine();
      if (isLog(line)) {
        if (this.logSink) this.logSink(line);
        continue;
      }
      return parseReply(line).value;
    }
  }

  /** Send one command line; resolve with its reply payload (`""` for a bare `ok`). */
  cmd(line: string): Promise<string> {
    return this.exclusive(async () => {
      this.drainStale();
      await this.transport.write(`${line}\r\n`);
      return this.readReply();
    });
  }

  // --- stimulus (target A) ---------------------------------------------------

  setParam(name: string, deck: string, value: number): Promise<string> {
    return this.cmd(`set param ${name} ${deck} ${fmt(value)}`);
  }

  async getParam(name: string, deck: string): Promise<number> {
    return Number(await this.cmd(`get param ${name} ${deck}`));
  }

  async setConfig(name: string, deck: string, v: string | number): Promise<boolean> {
    return (await this.cmd(`config ${name} ${deck} ${v}`)) === '1';
  }

  cv(kind: string, deck: string, value: number): Promise<string> {
    return this.cmd(`cv ${kind} ${deck} ${fmt(value)}`);
  }

  gate(deck: string): Promise<string> {
    return this.cmd(`gate ${deck}`);
  }

  pad(action: string, deck: string, rev = false): Promise<string> {
    return this.cmd(`pad ${action} ${deck}${rev ? ' rev' : ''}`);
  }

  fx(kind: string, deck: string, on: boolean): Promise<string> {
    return this.cmd(`fx ${kind} ${deck} ${on ? 'on' : 'off'}`);
  }

  // --- observation -----------------------------------------------------------

  query(name: string, deck = ''): Promise<string> {
    return this.cmd(`query ${name} ${deck}`.trimEnd());
  }

  async caps(): Promise<number> {
    return parseInt(await this.cmd('caps'), 16);
  }

  /**
   * `reset cpu` -> drive the engine -> `query cpumax` is the sequence a measurement wants; without the
   * reset the peak is whatever the boot transient happened to be.
   */
  resetCpu(): Promise<string> {
    return this.cmd('reset cpu');
  }

  /** The three CPU readings. Sequential, not parallel: the protocol allows one command in flight. */
  async cpu(): Promise<CpuReading> {
    const avg = Number(await this.query('cpu'));
    const min = Number(await this.query('cpumin'));
    const max = Number(await this.query('cpumax'));
    return { avg, min, max };
  }

  // --- introspection ---------------------------------------------------------

  /** Run `describe` and return the raw block lines (parse with `parseDescribe`). */
  describeLines(): Promise<string[]> {
    return this.exclusive(async () => {
      this.drainStale();
      await this.transport.write('describe\r\n');
      const lines: string[] = [];
      for (;;) {
        const line = await this.readLine();
        if (isLog(line)) {
          if (this.logSink) this.logSink(line);
          continue;
        }
        if (line === 'end') return lines;
        lines.push(line);
      }
    });
  }

  // --- determinism -----------------------------------------------------------

  /** `mode test` freezes the physical input path so injected stimulus is the only driver. */
  testMode(on: boolean): Promise<string> {
    return this.cmd(`mode ${on ? 'test' : 'run'}`);
  }

  close(): Promise<void> {
    for (const w of this.waiters) {
      if (w.timer) clearTimeout(w.timer);
      w.reject(new Timeout('closed'));
    }
    this.waiters = [];
    return this.transport.close();
  }
}

/** Match the Python client's `{:.6g}` so the same value produces the same line on the wire. */
function fmt(v: number): string {
  const n = Number(v);
  if (!Number.isFinite(n)) throw new CommandError('bad-arg');
  if (Number.isInteger(n) && Math.abs(n) < 1e6) return String(n);
  return String(Number(n.toPrecision(6)));
}
