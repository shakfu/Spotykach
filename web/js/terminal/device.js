// device.js - the command API, over any transport.
//
// A port of tools/skdev/device.py, and deliberately transport-agnostic for the same reason that file's
// tests are: the framing rules (one command in flight, skip interleaved `[tag]` log lines, discard
// stale bytes before sending) are where the bugs live, and they are testable against a scripted
// stream. web/test/device.test.js drives this class with a fake device and no hardware at all,
// mirroring how tools/conftest.py skips cleanly when nothing is attached.
//
// A transport is any object with:
//     write(text: string): Promise<void>
//     onLine(cb: (line: string) => void): void    - called for each complete line received
//     close(): Promise<void>

import { isLog, parseReply, CommandError, Timeout } from './framing.js';

/**
 * Default read timeout. Generous on purpose, exactly as in the Python client: the channel is pumped
 * from the firmware's main loop, so its latency is bounded by the SLOWEST main-loop consumer - a
 * streaming engine scanning the SD card in prepare() can stall replies for a good fraction of a second.
 */
export const DEFAULT_TIMEOUT_MS = 3000;

export class Device {
  /**
   * @param {{write: Function, onLine: Function, close: Function}} transport
   * @param {{timeout?: number, logSink?: (line: string) => void}} [opts]
   */
  constructor(transport, opts = {}) {
    this.transport = transport;
    this.timeout = opts.timeout ?? DEFAULT_TIMEOUT_MS;
    this.logSink = opts.logSink || null;
    /** @type {string[]} */
    this._lines = [];
    /** @type {Array<{resolve: Function, reject: Function, timer: any}>} */
    this._waiters = [];
    this._busy = null; // serializes commands; the protocol allows exactly one in flight
    transport.onLine((line) => this._push(line));
  }

  _push(line) {
    const w = this._waiters.shift();
    if (w) {
      clearTimeout(w.timer);
      w.resolve(line);
    } else {
      this._lines.push(line);
    }
  }

  _readLine() {
    if (this._lines.length) return Promise.resolve(this._lines.shift());
    return new Promise((resolve, reject) => {
      const waiter = { resolve, reject, timer: null };
      waiter.timer = setTimeout(() => {
        const i = this._waiters.indexOf(waiter);
        if (i >= 0) this._waiters.splice(i, 1);
        reject(new Timeout('no reply'));
      }, this.timeout);
      this._waiters.push(waiter);
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
  _drainStale() {
    if (!this._lines.length) return;
    if (this.logSink) for (const ln of this._lines) this.logSink(`[stale] ${ln}`);
    this._lines = [];
  }

  async _exclusive(fn) {
    const prev = this._busy || Promise.resolve();
    let release;
    this._busy = new Promise((r) => { release = r; });
    await prev.catch(() => {});
    try {
      return await fn();
    } finally {
      release();
    }
  }

  async _readReply() {
    for (;;) {
      const line = await this._readLine();
      if (isLog(line)) {
        if (this.logSink) this.logSink(line);
        continue;
      }
      return parseReply(line).value;
    }
  }

  /** Send one command line; resolve with its reply payload (`""` for a bare `ok`). */
  cmd(line) {
    return this._exclusive(async () => {
      this._drainStale();
      await this.transport.write(line + '\r\n');
      return this._readReply();
    });
  }

  // --- stimulus (target A) ---------------------------------------------------
  setParam(name, deck, value) {
    return this.cmd(`set param ${name} ${deck} ${fmt(value)}`);
  }

  async getParam(name, deck) {
    return Number(await this.cmd(`get param ${name} ${deck}`));
  }

  async setConfig(name, deck, v) {
    return (await this.cmd(`config ${name} ${deck} ${v}`)) === '1';
  }

  cv(kind, deck, value) {
    return this.cmd(`cv ${kind} ${deck} ${fmt(value)}`);
  }

  gate(deck) {
    return this.cmd(`gate ${deck}`);
  }

  pad(action, deck, rev = false) {
    return this.cmd(`pad ${action} ${deck}${rev ? ' rev' : ''}`);
  }

  fx(kind, deck, on) {
    return this.cmd(`fx ${kind} ${deck} ${on ? 'on' : 'off'}`);
  }

  // --- observation -----------------------------------------------------------
  query(name, deck = '') {
    return this.cmd(`query ${name} ${deck}`.trimEnd());
  }

  async caps() {
    return parseInt(await this.cmd('caps'), 16);
  }

  /** `reset cpu` -> drive the engine -> `query cpumax` is the sequence a measurement wants; without
   * the reset the peak is whatever the boot transient happened to be. */
  resetCpu() {
    return this.cmd('reset cpu');
  }

  /** The three CPU readings. Sequential, not parallel: the protocol allows one command in flight. */
  async cpu() {
    const avg = Number(await this.query('cpu'));
    const min = Number(await this.query('cpumin'));
    const max = Number(await this.query('cpumax'));
    return { avg, min, max };
  }

  // --- introspection ---------------------------------------------------------
  /** Run `describe` and return the raw block lines (parse with descriptor.js). */
  describeLines() {
    return this._exclusive(async () => {
      this._drainStale();
      await this.transport.write('describe\r\n');
      const lines = [];
      for (;;) {
        const line = await this._readLine();
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
  testMode(on) {
    return this.cmd(`mode ${on ? 'test' : 'run'}`);
  }

  close() {
    for (const w of this._waiters) {
      clearTimeout(w.timer);
      w.reject(new Timeout('closed'));
    }
    this._waiters = [];
    return this.transport.close();
  }
}

/** Match the Python client's `{:.6g}` so the same value produces the same line on the wire. */
function fmt(v) {
  const n = Number(v);
  if (!Number.isFinite(n)) throw new CommandError('bad-arg');
  if (Number.isInteger(n) && Math.abs(n) < 1e6) return String(n);
  return String(Number(n.toPrecision(6)));
}
