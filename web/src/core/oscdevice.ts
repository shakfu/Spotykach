// oscdevice.ts - the command API for a device built with `TERMINAL=1 OSC=1`.
//
// A port of tools/skdev/oscdevice.py, and it exposes the SAME method surface as `Device` over the
// OSC+SLIP codec instead of the line codec. That is not a convenience, it is the acceptance
// criterion: layer [3] is shared byte for byte between the two firmware builds, so anything the OSC
// path answers differently is a codec bug by definition. The Python pair proved this on hardware -
// 63/63 identical - and every one of the five defects that sweep found was a CLIENT difference
// wearing a codec's clothes, which is the failure mode a second pair of clients can reintroduce.
//
// See `docs/dev/terminal-osc.md`.
//
// Two differences from the line client are protocol, not implementation:
//
//   * A successful write is SILENT. There is no `/sk/ok`; a rig streaming fader moves at 100 Hz does
//     not want an ack per message. This client turns acks on at connect (`/sk/dev/mode/ack ,T`) so a
//     write is assertable without a read-back.
//   * Errors arrive on `/sk/err`, carrying the request address alongside the reason, rather than as
//     an `err <reason>` line.

import { decode, decodePacket, encode, oscInt, type OscArg, type OscValue } from './osc.ts';
import { CommandError, Timeout, type ConfigDesc, type Descriptor, type ParamDesc, type QueryDesc }
  from './protocol.ts';
import type { FrameTransport } from './ports.ts';
import { DEFAULT_TIMEOUT_MS, type CpuReading } from './device.ts';

export interface OscDeviceOptions {
  timeout?: number;
  logSink?: (line: string) => void;
  /**
   * Ask the device to acknowledge writes. On by default and for the same reason the Python client
   * does it: without acks a write cannot be asserted without a read-back, and a UI cannot tell a
   * rejected address from an applied one.
   */
  ack?: boolean;
}

/** The platform reads that live under `/sk/dev` rather than under a deck's `/state`. */
const DEV_QUERIES = new Set(['cpu', 'cpumin', 'cpumax', 'usb']);

interface Waiter {
  resolve: (p: Uint8Array) => void;
  reject: (e: Error) => void;
  timer: ReturnType<typeof setTimeout> | null;
}

export class OscDevice {
  private readonly timeout: number;
  private readonly logSink: ((line: string) => void) | null;
  private frames: Uint8Array[] = [];
  private waiters: Waiter[] = [];
  private busy: Promise<void> | null = null;
  /** Lazily filled from describe(); see `prefix()`. */
  private globals: { params: Set<string>; queries: Set<string> } | null = null;

  constructor(private readonly transport: FrameTransport, opts: OscDeviceOptions = {}) {
    this.timeout = opts.timeout ?? DEFAULT_TIMEOUT_MS;
    this.logSink = opts.logSink ?? null;
    transport.onFrame((f) => this.push(f));
    if (opts.ack ?? true) void this.send('/sk/dev/mode/ack', true);
  }

  // --- framing --------------------------------------------------------------

  private push(frame: Uint8Array): void {
    const w = this.waiters.shift();
    if (w) {
      if (w.timer) clearTimeout(w.timer);
      w.resolve(frame);
    } else {
      this.frames.push(frame);
    }
  }

  private readFrame(): Promise<Uint8Array> {
    const buffered = this.frames.shift();
    if (buffered !== undefined) return Promise.resolve(buffered);
    return new Promise<Uint8Array>((resolve, reject) => {
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
   * Discard anything already waiting before sending. Same rationale as `Device.drainStale`: one
   * timeout would otherwise offset every subsequent reply for the life of the session.
   */
  private drainStale(): void {
    if (!this.frames.length) return;
    if (this.logSink) {
      for (const f of this.frames) {
        const { address } = decode(f);
        this.logSink(`[stale] ${address}`);
      }
    }
    this.frames = [];
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

  /** Send one OSC message. Does not wait for anything. */
  send(address: string, ...args: OscArg[]): Promise<void> {
    return this.transport.send(encode(address, ...args));
  }

  /**
   * Read one reply frame, forwarding any `/sk/log` frames to the log sink first.
   *
   * Phase 1 of the OSC build forces `INFS_LOG=0` (the Makefile errors on `OSC=1 DEBUG=1`), so no log
   * frame can arrive today. Handling it anyway costs three lines and is what lets the deferred
   * `/sk/log` framing land without a client change - the same shape as `Device.readReply` skipping
   * `[tag]` lines.
   */
  private async readReply(): Promise<{ address: string; args: OscValue[] }> {
    for (;;) {
      const msg = decode(await this.readFrame());
      if (msg.address === '/sk/log') {
        if (this.logSink) this.logSink(String(msg.args[0] ?? ''));
        continue;
      }
      if (msg.address === '/sk/err') {
        // The request address is echoed, which is what makes an error actionable: nothing else
        // correlates a rejection back to what caused it.
        throw new CommandError(String(msg.args.length > 1 ? msg.args[1] : 'unknown'));
      }
      return msg;
    }
  }

  /** Send, then read exactly one reply. Throws `CommandError` on `/sk/err`. */
  request(address: string, ...args: OscArg[]): Promise<OscValue | OscValue[]> {
    return this.exclusive(async () => {
      this.drainStale();
      await this.send(address, ...args);
      const { args: vals } = await this.readReply();
      return vals.length === 1 ? vals[0] : vals;
    });
  }

  /** Send a write and consume its ack (this client enables acks at connect). */
  private write(address: string, ...args: OscArg[]): Promise<OscValue | OscValue[]> {
    return this.request(address, ...args);
  }

  // --- scope ----------------------------------------------------------------
  //
  // The one place the two address spaces genuinely differ. The line codec makes a caller pass a deck
  // for a GLOBAL param and then discards it; the OSC address space encodes scope STRUCTURALLY, so a
  // global carries no deck segment at all and `/sk/a/param/crossfade` is `unknown-address`. Callers
  // written against the line surface pass a deck regardless, so drop it here - otherwise every global
  // would fail against OSC and pass against lines, for no reason a caller could see.

  private async scope(): Promise<{ params: Set<string>; queries: Set<string> }> {
    if (!this.globals) {
      const d = await this.describe();
      this.globals = {
        params: new Set([...d.params.values()].filter((p) => p.scope === 'global').map((p) => p.name)),
        queries: new Set([...d.queries.values()].filter((q) => q.scope === 'global').map((q) => q.name)),
      };
    }
    return this.globals;
  }

  private async prefix(deck: string, name?: string, kind: 'param' | 'cfg' | 'state' = 'param'):
  Promise<string> {
    if (name !== undefined) {
      const { params, queries } = await this.scope();
      // `route` is the only global config, and it is global for the same reason the others are: it is
      // the instrument's channel topology, not a property of either deck.
      const global = kind === 'cfg' ? name === 'route'
        : kind === 'param' ? params.has(name) : queries.has(name);
      if (global) return '/sk';
    }
    return deck ? `/sk/${deck.toLowerCase()}` : '/sk';
  }

  // --- stimulus (target A) ---------------------------------------------------

  async setParam(name: string, deck: string, value: number): Promise<string> {
    await this.write(`${await this.prefix(deck, name)}/param/${name}`, Number(value));
    return '';
  }

  async getParam(name: string, deck: string): Promise<number> {
    return Number(await this.request(`${await this.prefix(deck, name)}/param/${name}`));
  }

  async setConfig(name: string, deck: string, v: string | number): Promise<boolean> {
    await this.write(`${await this.prefix(deck, name, 'cfg')}/cfg/${name}`, oscInt(Number(v)));
    return true; // the OSC codec does not report the changed flag; an ack means applied
  }

  async cv(kind: string, deck: string, value: number): Promise<string> {
    await this.write(`${await this.prefix(deck)}/cv/${kind}`, Number(value));
    return '';
  }

  async gate(deck: string): Promise<string> {
    await this.write(`${await this.prefix(deck)}/gate`);
    return '';
  }

  async midiNote(ch: number, note: number): Promise<string> {
    await this.write('/sk/midi/note', oscInt(ch), oscInt(note));
    return '';
  }

  /**
   * Press a pad, returning the reply payload exactly as `Device.pad` does.
   *
   * `play` answers with the deck's emptiness. The OSC reply carries the whole line the line codec
   * would have sent (`"ok empty=0"`) because that handler composes free-form text, which the typed
   * sink cannot describe; the line client strips the `ok ` framing, so strip it here too. Without
   * this the two clients report a different value for a press the two CODECS agree about.
   */
  async pad(action: string, deck: string, rev = false): Promise<string> {
    const addr = `${await this.prefix(deck)}/pad/${action}`;
    if (action === 'play') {
      const r = await this.request(addr, rev);
      return typeof r === 'string' && r.startsWith('ok ') ? r.slice(3) : String(r);
    }
    if (rev) await this.write(addr, true);
    else await this.write(addr);
    return ''; // bare ok, exactly as the line client reports it
  }

  async fx(kind: string, deck: string, on: boolean): Promise<string> {
    await this.write(`${await this.prefix(deck)}/fx/${kind}`, on);
    return '';
  }

  // --- observation -----------------------------------------------------------

  /**
   * Read a state address, returning TEXT - exactly what `Device.query` returns.
   *
   * OSC replies are typed, so the value arrives as a number/string/boolean rather than a string.
   * Rendering it back to text is what keeps callers codec-agnostic: they test things like
   * `reply === '1'` for a bool, and a client handing back `true` would fail every one of those
   * against OSC and pass against lines - a difference in the CLIENT masquerading as one in the
   * firmware.
   */
  async query(name: string, deck = ''): Promise<string> {
    const v = DEV_QUERIES.has(name)
      ? await this.request(`/sk/dev/${name}`)
      : await this.request(`${await this.prefix(deck, name, 'state')}/state/${name}`);
    if (typeof v === 'boolean') return v ? '1' : '0';
    if (typeof v === 'number') {
      return Number.isInteger(v) ? String(v) : v.toFixed(4); // the line codec's append_f32 default
    }
    return String(v);
  }

  async caps(): Promise<number> {
    return Number(await this.request('/sk/dev/caps'));
  }

  async resetCpu(): Promise<string> {
    await this.request('/sk/dev/reset/cpu');
    return '';
  }

  /** The three CPU readings. Sequential, not parallel: the protocol allows one command in flight. */
  async cpu(): Promise<CpuReading> {
    return {
      avg: Number(await this.query('cpu')),
      min: Number(await this.query('cpumin')),
      max: Number(await this.query('cpumax')),
    };
  }

  // --- introspection ---------------------------------------------------------

  /**
   * The raw decoded rows of the describe bundle.
   *
   * One bundle, so one SLIP frame - the descriptor arrives atomically or not at all, which is why the
   * device sizes its TX FIFO to hold a whole one.
   */
  describeRows(): Promise<Array<{ address: string; args: OscValue[] }>> {
    return this.exclusive(async () => {
      this.drainStale();
      await this.send('/sk/dev/describe');
      return decodePacket(await this.readFrame());
    });
  }

  /**
   * The SAME `Descriptor` the line codec's `parseDescribe` produces.
   *
   * This is what makes the parity claim testable rather than rhetorical: `TerminalModel` consumes a
   * descriptor and generates a surface from it, so producing an identical model here lets the whole
   * UI run against either codec, and any difference in what it renders is a codec bug.
   *
   * The bundle carries full addresses where the line codec carries bare names, and deck-scoped rows
   * arrive once per deck (the device expands them so a host need not know decks exist). Both are
   * reduced back to the line codec's shape here: the name is the address's last segment, and the two
   * deck rows collapse into one entry.
   */
  async describe(): Promise<Descriptor> {
    return describeFromRows(await this.describeRows());
  }

  // --- determinism -----------------------------------------------------------

  /** `mode test` freezes the physical input path so injected stimulus is the only driver. */
  async testMode(on: boolean): Promise<string> {
    await this.write(`/sk/dev/mode/${on ? 'test' : 'run'}`);
    return '';
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

/** `0:slice 1:reel` -> Map, as the line codec's `enumValues` does for its own token soup. */
function labelMap(s: string): Map<number, string> {
  const out = new Map<number, string>();
  for (const t of String(s).split(/\s+/).filter((x) => x.includes(':'))) {
    const i = t.indexOf(':');
    const k = Number(t.slice(0, i));
    if (Number.isInteger(k)) out.set(k, t.slice(i + 1));
  }
  return out;
}

/**
 * Reduce a decoded describe bundle to the shared `Descriptor` model.
 *
 * Exported separately from the client so it can be tested against the firmware's own sample bytes
 * with no transport at all - the same arrangement `parseDescribe` has on the line side.
 */
export function describeFromRows(rows: Array<{ address: string; args: OscValue[] }>): Descriptor {
  const d: Descriptor = {
    engine: '', version: '', masked: false,
    params: new Map<string, ParamDesc>(),
    configs: new Map<string, ConfigDesc>(),
    queries: new Map<string, QueryDesc>(),
    caps: 0,
  };
  const leaf = (addr: OscValue): string => String(addr).split('/').pop() ?? '';
  for (const { address, args } of rows) {
    switch (address) {
      case '/sk/reply/dev/describe':
        if (args.length >= 3) {
          d.engine = String(args[0]);
          d.version = String(args[1]);
          d.masked = args[2] === 'masked=1';
        }
        break;
      case '/sk/reply/dev/describe/param': {
        if (args.length < 5) break;
        const name = leaf(args[0]);
        d.params.set(name, { name, scope: String(args[4]), lo: Number(args[2]), hi: Number(args[3]) });
        break;
      }
      case '/sk/reply/dev/describe/cfg': {
        if (args.length < 3) break;
        const name = leaf(args[0]);
        d.configs.set(name, { name, values: labelMap(String(args[2])) });
        break;
      }
      case '/sk/reply/dev/describe/state': {
        if (args.length < 3) break;
        const name = leaf(args[0]);
        const scope = ['a', 'b'].includes(String(args[0]).split('/')[2]) ? 'deck' : 'global';
        // A 4th string carries an Enum query's selector labels, empty for other kinds. Older firmware
        // sent only three, so treat it as optional rather than requiring it.
        d.queries.set(name, {
          name, scope, kind: String(args[2]),
          values: labelMap(args.length >= 4 ? String(args[3]) : ''),
        });
        break;
      }
      case '/sk/reply/dev/describe/caps':
        if (args.length) d.caps = Number(args[0]);
        break;
      default:
        break; // forward-compatible: a row this page does not know is not an error
    }
  }
  return d;
}
