// protocol.ts - the line protocol and the describe model, with no transport attached.
//
// Two things that belong together and to nothing else: how a byte stream becomes lines, and what the
// device says about itself when asked. Both are pure, which is the point - chunk-boundary bugs are the
// classic serial defect, unfixable by inspection and trivial to test if the framer is separable from
// the port. Split a reply across two reads, or hand it a `\r\n` that straddles the boundary, and see
// whether one line still comes out.
//
// The one protocol invariant, mirrored from tools/skdev/protocol.py: log lines begin with `[` (the
// firmware's LOG_TAGGED format `[tag] ...`); reply lines never do.

export const LOG_PREFIX = '[';

/** True if `line` is a `[tag] ...` log line rather than a reply. */
export const isLog = (line: string): boolean => line.startsWith(LOG_PREFIX);

/**
 * Incremental newline framer. Feed it whatever the transport hands over - chunks of any size, split
 * anywhere - and it yields complete lines.
 */
export class LineAssembler {
  private buf = '';

  /** @returns complete lines, with the terminator and any trailing CR stripped */
  push(chunk: string): string[] {
    this.buf += chunk;
    const out: string[] = [];
    let i: number;
    while ((i = this.buf.indexOf('\n')) >= 0) {
      out.push(this.buf.slice(0, i).replace(/\r$/, ''));
      this.buf = this.buf.slice(i + 1);
    }
    return out;
  }

  /** Anything received but not yet terminated - a partial line at close, useful in diagnostics. */
  get pending(): string {
    return this.buf;
  }
}

/**
 * The device replied `err <reason>`. `reason` is the bare token, which is a fixed vocabulary
 * (unknown-verb, unknown-param, bad-deck, bad-arg, overflow, ...), so UI can match on it.
 */
export class CommandError extends Error {
  readonly reason: string;

  constructor(reason: string) {
    super(reason);
    this.reason = reason;
  }
}

export class Timeout extends Error {}

/**
 * Classify one reply line.
 *
 * @throws {CommandError} for `err <reason>` and for anything that is neither
 */
export function parseReply(line: string): { kind: 'ok'; value: string } {
  if (line === 'ok') return { kind: 'ok', value: '' };
  if (line.startsWith('ok ')) return { kind: 'ok', value: line.slice(3) };
  if (line.startsWith('err ')) throw new CommandError(line.slice(4));
  throw new CommandError(`unexpected: ${JSON.stringify(line)}`);
}

/**
 * Verbs that change something the user cannot get back with another command.
 *
 * docs/dev/terminal-target-b.md warns that sweeping a control surface can clear a recorded buffer or
 * write the card, so the generated UI must not fire these from a single click. This is the list the
 * console and the control surface both consult before sending.
 */
const DESTRUCTIVE = [
  /^pad\s+clear\b/,
  /^seq\s+clear\b/,
  /^clear\b/,
  /^preset\s+save\b/,
  /^reset\b(?!\s+cpu\b)/, // `reset cpu` only clears meter extremes - harmless
];

/** Does this command line need a confirmation before it is sent? */
export function isDestructive(line: string): boolean {
  const s = line.trim().toLowerCase();
  return DESTRUCTIVE.some((re) => re.test(s));
}

// --- describe -------------------------------------------------------------------------------------
//
// A port of tools/skdev/descriptor.py. This is what makes a web terminal worth more than a serial
// monitor: `describe` reports the engine's own control surface - every parameter with its real range
// and deck scope, every config with its enum labels, the query vocabulary, and a capability mask - so
// the UI can be GENERATED rather than hard-coded.
//
// Wire format (one item per line, terminated by a bare `end`, which the caller strips):
//
//     descr engine=<name> version=<ver> masked=<0|1>
//     param  <name> <deck|global> <lo>..<hi>
//     config <name> [scope] <int>:<label> ...
//     query  <name> <deck|global> [kind] [int:label ...]
//     caps   0x<hex>
//     end
//
// Unknown tags are ignored rather than rejected, so firmware that adds a line type does not break an
// older page.

export interface ParamDesc {
  name: string;
  scope: string;
  lo: number;
  hi: number;
}

export interface ConfigDesc {
  name: string;
  values: Map<number, string>;
}

export interface QueryDesc {
  name: string;
  scope: string;
  kind: string;
  values: Map<number, string>;
}

export interface Descriptor {
  engine: string;
  version: string;
  /**
   * masked=1 means the engine declared which ids it actually implements. With masked=0 the descriptor
   * is the whole ParamId enum, so a generated surface would show controls the engine ignores - worth
   * flagging in the UI rather than silently rendering dead sliders.
   */
  masked: boolean;
  params: Map<string, ParamDesc>;
  configs: Map<string, ConfigDesc>;
  queries: Map<string, QueryDesc>;
  caps: number;
}

/** Parse a describe block. Log lines already filtered out; `end` need not be present. */
export function parseDescribe(lines: string[]): Descriptor {
  const d: Descriptor = {
    engine: '',
    version: '',
    masked: false,
    params: new Map(),
    configs: new Map(),
    queries: new Map(),
    caps: 0,
  };
  for (const line of lines) {
    const tok = line.trim().split(/\s+/).filter(Boolean);
    if (!tok.length) continue;
    switch (tok[0]) {
      case 'descr': {
        const kv = new Map(tok.slice(1).filter((t) => t.includes('=')).map((t): [string, string] => {
          const i = t.indexOf('=');
          return [t.slice(0, i), t.slice(i + 1)];
        }));
        d.engine = kv.get('engine') ?? '';
        d.version = kv.get('version') ?? '';
        d.masked = kv.get('masked') === '1';
        break;
      }
      case 'param': { // param <name> <scope> <lo>..<hi>
        if (tok.length < 4) break;
        const [lo, hi] = tok[3].split('..').map(Number);
        d.params.set(tok[1], { name: tok[1], scope: tok[2], lo, hi });
        break;
      }
      case 'config': { // config <name> [scope] i:label i:label ...
        if (tok.length < 2) break;
        d.configs.set(tok[1], { name: tok[1], values: enumValues(tok.slice(2)) });
        break;
      }
      case 'query': { // query <name> <scope> [kind] [i:label ...]
        if (tok.length < 2) break;
        d.queries.set(tok[1], {
          name: tok[1],
          scope: tok.length > 2 ? tok[2] : 'global',
          kind: tok.length > 3 ? tok[3] : 'text',
          values: enumValues(tok.slice(4)),
        });
        break;
      }
      case 'caps':
        if (tok.length > 1) d.caps = parseInt(tok[1], 16) || 0;
        break;
      default:
        break; // forward-compatible: a tag this page does not know is not an error
    }
  }
  return d;
}

/** `0:slice 1:reel 2:drift` -> Map. Non-`int:label` tokens (an optional scope) are skipped. */
function enumValues(tokens: string[]): Map<number, string> {
  const out = new Map<number, string>();
  for (const t of tokens) {
    const i = t.indexOf(':');
    if (i < 0) continue;
    const k = Number(t.slice(0, i));
    if (Number.isInteger(k)) out.set(k, t.slice(i + 1));
  }
  return out;
}

/**
 * The command vocabulary a descriptor implies, for console completion. Mirrors what tools/skterm.py
 * completes against, so the two front-ends offer the same words.
 */
export function vocabulary(desc: Descriptor): string[] {
  const words = new Set([
    'set', 'get', 'param', 'config', 'query', 'cv', 'gate', 'midi', 'pad', 'seq', 'fx',
    'reset', 'preset', 'caps', 'mode', 'describe', 'help', 'note', 'msg', 'transport',
    'test', 'run', 'save', 'load', 'play', 'rec', 'stop', 'clear', 'trig', 'arm', 'disarm',
    'flux', 'grit', 'lock', 'gritmode', 'voct', 'mix', 'size', 'xfade', 'cpu', 'A', 'B',
  ]);
  for (const k of desc.params.keys()) words.add(k);
  for (const k of desc.configs.keys()) words.add(k);
  for (const k of desc.queries.keys()) words.add(k);
  return [...words].sort();
}

/**
 * Parse the `query usb` reply - a flag soup like
 * `boot=1 region=2 clkcfg=1 hsi48=1 usbsel=0 usb33den=1 usb33rdy=1 phy=1 pullup=1` - into ordered
 * pairs, so the UI can render the UsbDiag bring-up snapshot as a table rather than a wall of text.
 */
export function parseUsbDiag(reply: string): Array<{ key: string; value: string }> {
  return reply.trim().split(/\s+/).filter((t) => t.includes('=')).map((t) => {
    const i = t.indexOf('=');
    return { key: t.slice(0, i), value: t.slice(i + 1) };
  });
}
