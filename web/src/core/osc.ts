// osc.ts - OSC 1.0 wire format + SLIP framing, with no transport attached.
//
// The browser half of `tools/skdev/osc.py`, which is itself the host half of
// `src/terminal/osc_{decode,encode}.cpp` and `src/terminal/slip.h`. See `docs/dev/terminal-osc.md`.
//
// This sits beside `protocol.ts` for the same reason that file exists: framing is where the bugs live,
// and a framer separable from the port is one you can feed a frame split at every byte offset and
// assert it still comes out whole. Everything here is pure - no browser API, no transport - so the
// whole codec is testable under `bun` with no device and no DOM.
//
// Two things are worth knowing before reading:
//
//   * OSC is big-endian and 4-byte aligned. Strings are NUL-terminated and then padded with NULs to
//     the next multiple of four.
//   * A message with NO type-tag string at all is legal, and here it is meaningful: it is how a read
//     is spelled. `encode(addr)` with no arguments produces exactly that.

// --- SLIP (RFC 1055) --------------------------------------------------------------------------

export const END = 0xc0;
export const ESC = 0xdb;
export const ESC_END = 0xdc;
export const ESC_ESC = 0xdd;

/** Wrap `payload` in one SLIP frame, END-delimited at both ends. */
export function slipEncode(payload: Uint8Array): Uint8Array {
  // Worst case every byte escapes, plus the two delimiters.
  const out = new Uint8Array(payload.length * 2 + 2);
  let n = 0;
  out[n++] = END;
  for (const b of payload) {
    if (b === END) {
      out[n++] = ESC;
      out[n++] = ESC_END;
    } else if (b === ESC) {
      out[n++] = ESC;
      out[n++] = ESC_ESC;
    } else {
      out[n++] = b;
    }
  }
  out[n++] = END;
  return out.subarray(0, n);
}

/**
 * Incremental SLIP decoder: feed bytes, get back complete frames.
 *
 * Unbounded on purpose, exactly as in the Python client. The device's own assembler is capped at
 * 512 B because that bounds the INBOUND direction; the describe bundle travelling the other way is
 * an order of magnitude larger (~4 KB), so a host has to be able to receive what the device can send.
 */
export class SlipDecoder {
  private buf: number[] = [];
  private escaped = false;

  /** Consume bytes; return however many complete frames they completed (possibly none). */
  feed(data: Uint8Array): Uint8Array[] {
    const frames: Uint8Array[] = [];
    for (const b of data) {
      if (b === END) {
        // Back-to-back ENDs are legal padding, not a zero-length packet.
        if (this.buf.length) frames.push(Uint8Array.from(this.buf));
        this.buf = [];
        this.escaped = false;
      } else if (this.escaped) {
        this.buf.push(b === ESC_END ? END : b === ESC_ESC ? ESC : b);
        this.escaped = false;
      } else if (b === ESC) {
        this.escaped = true;
      } else {
        this.buf.push(b);
      }
    }
    return frames;
  }

  /** Bytes received inside an unterminated frame - useful in diagnostics, as `LineAssembler.pending`. */
  get pending(): number {
    return this.buf.length;
  }
}

// --- OSC --------------------------------------------------------------------------------------

/** A decoded OSC argument. The device sends `i`/`f`/`d` as numbers, `s`/`S` as strings, `T`/`F` as booleans. */
export type OscValue = number | string | boolean;

/**
 * An argument explicitly tagged `,i`.
 *
 * The one place this port cannot follow the Python. There, `isinstance(a, int)` separates `1` from
 * `1.0`; in JavaScript they are the same value, so an encoder that guessed from `Number.isInteger`
 * would send `,i` for a fader that happened to land on 1.0 and `,f` either side of it - the tag
 * flickering with the value. So a plain `number` is always `,f` (params, CV and levels, which is
 * nearly everything) and an int is spelled explicitly.
 *
 * The device coerces both directions anyway - `docs/dev/terminal-osc.md`, "Type coercion", accepts
 * `i` where a float is expected and truncates `f` where an int is - so this decides what goes on the
 * wire, not whether it is understood.
 */
export interface OscInt {
  readonly __osc: 'i';
  readonly value: number;
}

/** Tag `n` as an OSC 32-bit integer (`,i`) rather than the default float. */
export const oscInt = (n: number): OscInt => ({ __osc: 'i', value: Math.trunc(n) });

export type OscArg = number | string | boolean | OscInt;

const isOscInt = (a: OscArg): a is OscInt =>
  typeof a === 'object' && a !== null && (a as OscInt).__osc === 'i';

/** Pad to the next multiple of four. */
const padded = (n: number): number => (n + 3) & ~3;

function encodeString(s: string): Uint8Array {
  // ASCII only, matching the Python's `.encode("ascii")`: the address space is generated from C
  // identifiers and the labels alongside it are ASCII too.
  const bytes = new Uint8Array(padded(s.length + 1));
  for (let i = 0; i < s.length; i++) bytes[i] = s.charCodeAt(i) & 0x7f;
  return bytes; // the NUL terminator and its padding are already zero
}

/**
 * Build one OSC message.
 *
 * Arguments are typed from their JavaScript type: `number` -> `f`, `string` -> `s`, `boolean` ->
 * `T`/`F`, and `oscInt(n)` -> `i`. Call with no arguments to produce the READ form - a message with
 * no type-tag string, which the device reads as "report the value at this address".
 */
export function encode(address: string, ...args: OscArg[]): Uint8Array {
  const addr = encodeString(address);
  if (!args.length) return addr;

  let tags = ',';
  let bodyLen = 0;
  for (const a of args) {
    if (typeof a === 'boolean') tags += a ? 'T' : 'F'; // no body bytes: the tag IS the value
    else if (isOscInt(a)) { tags += 'i'; bodyLen += 4; }
    else if (typeof a === 'number') { tags += 'f'; bodyLen += 4; }
    else if (typeof a === 'string') { tags += 's'; bodyLen += padded(a.length + 1); }
    else throw new TypeError(`unsupported OSC argument: ${JSON.stringify(a)}`);
  }

  const tagBytes = encodeString(tags);
  const out = new Uint8Array(addr.length + tagBytes.length + bodyLen);
  out.set(addr, 0);
  out.set(tagBytes, addr.length);

  const view = new DataView(out.buffer, out.byteOffset);
  let off = addr.length + tagBytes.length;
  for (const a of args) {
    if (typeof a === 'boolean') continue;
    if (isOscInt(a)) { view.setInt32(off, a.value, false); off += 4; }
    else if (typeof a === 'number') { view.setFloat32(off, a, false); off += 4; }
    else { const s = encodeString(a); out.set(s, off); off += s.length; }
  }
  return out;
}

/** Read one OSC-string at `off`; return it and the offset past its NUL padding. */
function readString(packet: Uint8Array, off: number): [string, number] {
  let end = off;
  while (end < packet.length && packet[end] !== 0) end++;
  if (end >= packet.length) throw new Error('malformed OSC packet: unterminated string');
  let s = '';
  for (let i = off; i < end; i++) s += String.fromCharCode(packet[i]);
  return [s, off + padded(end - off + 1)];
}

/**
 * Decode one OSC message into its address and arguments.
 *
 * Bundles are not handled here - use `decodePacket`, which dispatches.
 */
export function decode(packet: Uint8Array): { address: string; args: OscValue[] } {
  const [address, start] = readString(packet, 0);
  if (start >= packet.length) return { address, args: [] }; // no type-tag string: the read form

  const [tags, tagEnd] = readString(packet, start);
  const view = new DataView(packet.buffer, packet.byteOffset, packet.byteLength);
  const args: OscValue[] = [];
  let off = tagEnd;
  for (const t of tags.slice(1)) {
    switch (t) {
      case 'i': args.push(view.getInt32(off, false)); off += 4; break;
      case 'f': args.push(view.getFloat32(off, false)); off += 4; break;
      case 'd': args.push(view.getFloat64(off, false)); off += 8; break;
      case 's':
      case 'S': { const [s, next] = readString(packet, off); args.push(s); off = next; break; }
      case 'T': args.push(true); break;
      case 'F': args.push(false); break;
      default: throw new Error(`unsupported OSC type tag ${JSON.stringify(t)}`);
    }
  }
  return { address, args };
}

const BUNDLE = '#bundle\0';

export function isBundle(packet: Uint8Array): boolean {
  if (packet.length < 8) return false;
  for (let i = 0; i < 8; i++) if (packet[i] !== BUNDLE.charCodeAt(i)) return false;
  return true;
}

/**
 * Decode a message or a bundle into a flat list of address/args pairs.
 *
 * Bundle timetags are ignored, exactly as the device ignores them: contents are taken as immediate
 * and in order. This is what `describe` arrives as - one bundle, so one SLIP frame, so the descriptor
 * is atomic or absent rather than half-parsed.
 */
export function decodePacket(packet: Uint8Array): Array<{ address: string; args: OscValue[] }> {
  if (!isBundle(packet)) return [decode(packet)];
  const view = new DataView(packet.buffer, packet.byteOffset, packet.byteLength);
  const out: Array<{ address: string; args: OscValue[] }> = [];
  let off = 16; // "#bundle\0" + timetag
  while (off + 4 <= packet.length) {
    const size = view.getInt32(off, false);
    off += 4;
    if (size < 0 || off + size > packet.length) break; // truncated bundle: keep what parsed
    out.push(...decodePacket(packet.subarray(off, off + size)));
    off += size;
  }
  return out;
}
