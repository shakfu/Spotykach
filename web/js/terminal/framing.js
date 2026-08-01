// framing.js - the line protocol, with no transport attached.
//
// The one protocol invariant, mirrored from tools/skdev/protocol.py: log lines begin with `[` (the
// firmware's LOG_TAGGED format `[tag] ...`); reply lines never do. Everything else about the wire is
// line-oriented ASCII with `ok ...` / `err ...` replies, so the interesting part is only ever "where
// does a line end" and "is this a reply or noise".
//
// Kept separate from the WebSerial code because chunk-boundary bugs are the classic serial defect and
// they are unfixable-by-inspection but trivial to test: split a reply across two reads, or hand it a
// `\r\n` that straddles the boundary, and see whether the assembler still produces one line.

export const LOG_PREFIX = '[';

/** True if `line` is a `[tag] ...` log line rather than a reply. */
export const isLog = (line) => line.startsWith(LOG_PREFIX);

/**
 * Incremental newline framer. Feed it whatever the transport hands over - chunks of any size, split
 * anywhere - and it yields complete lines.
 */
export class LineAssembler {
  constructor() {
    this.buf = '';
  }

  /**
   * @param {string} chunk
   * @returns {string[]} complete lines, with the terminator and any trailing CR stripped
   */
  push(chunk) {
    this.buf += chunk;
    const out = [];
    let i;
    while ((i = this.buf.indexOf('\n')) >= 0) {
      out.push(this.buf.slice(0, i).replace(/\r$/, ''));
      this.buf = this.buf.slice(i + 1);
    }
    return out;
  }

  /** Anything received but not yet terminated - a partial line at close, useful in diagnostics. */
  get pending() {
    return this.buf;
  }
}

/** The device replied `err <reason>`. `reason` is the bare token, which is a fixed vocabulary
 * (unknown-verb, unknown-param, bad-deck, bad-arg, overflow, ...), so UI can match on it. */
export class CommandError extends Error {
  constructor(reason) {
    super(reason);
    this.reason = reason;
  }
}

export class Timeout extends Error {}

/**
 * Classify one reply line.
 * @returns {{kind: "ok", value: string}} for `ok` / `ok <payload>`
 * @throws {CommandError} for `err <reason>` and for anything that is neither
 */
export function parseReply(line) {
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
export function isDestructive(line) {
  const s = line.trim().toLowerCase();
  return DESTRUCTIVE.some((re) => re.test(s));
}
