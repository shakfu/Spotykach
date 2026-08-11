// client.ts - one device surface over either codec.
//
// `Device` (line-ASCII) and `OscDevice` (OSC over SLIP) already expose the same method surface, which
// is the parity claim `docs/dev/terminal-osc.md` makes and the hardware sweep confirmed. This file is
// what lets the view-model above them stop caring which is underneath: a narrow interface covering
// exactly what a session does, plus a thin adapter for each codec.
//
// Adapters rather than a shared base class, and neither `Device` nor `OscDevice` is modified: both are
// ports of a Python client that the two suites hold to each other, and the value of that lineage is
// that reading either alongside its original is a diff, not an archaeology exercise.
//
// The one place the codecs cannot be made identical is free-text console input, and this file is
// where that difference is stated rather than smeared - see `exec` below.

import { Device } from './device.ts';
import { OscDevice } from './oscdevice.ts';
import { decode, encode, oscInt, type OscArg } from './osc.ts';
import { isDestructive, parseDescribe, type Descriptor } from './protocol.ts';
import type { CpuReading } from './device.ts';

export type Codec = 'line' | 'osc';

/** What a terminal session needs of a device, with the codec abstracted away. */
export interface DeviceClient {
  readonly codec: Codec;
  /** Run one line of console input, in this codec's own vocabulary. Resolves to the reply text. */
  exec(input: string): Promise<string>;
  /** Would this console input change something the user cannot get back? */
  destructive(input: string): boolean;
  describe(): Promise<Descriptor>;
  query(name: string, deck?: string): Promise<string>;
  cpu(): Promise<CpuReading>;
  resetCpu(): Promise<void>;
  close(): Promise<void>;
  /** What the console should show as its prompt hint, e.g. `set param speed a 0.5`. */
  readonly example: string;

  // --- the operations the generated control surface drives ---------------------
  //
  // Named operations rather than command strings, which is what lets the surface work over either
  // codec. It used to compose `set param ${name} ${deck} ${v}` inline, and that is a line-codec
  // sentence: against OSC it is not a different spelling of the same request, it is not a request at
  // all. Both clients already implement these - `Device` from tools/skdev/device.py, `OscDevice` from
  // oscdevice.py - so this interface is a restatement, not a new layer.

  setParam(name: string, deck: string, value: number): Promise<void>;
  getParam(name: string, deck: string): Promise<number>;
  setConfig(name: string, deck: string, value: number): Promise<void>;
  gate(deck: string): Promise<void>;
  pad(action: string, deck: string): Promise<string>;
}

export function lineClient(device: Device): DeviceClient {
  return {
    codec: 'line',
    exec: (input) => device.cmd(input),
    destructive: isDestructive,
    describe: async () => parseDescribe(await device.describeLines()),
    query: (name, deck = '') => device.query(name, deck),
    cpu: () => device.cpu(),
    resetCpu: async () => { await device.resetCpu(); },
    close: () => device.close(),
    example: 'set param speed a 0.5',
    setParam: async (name, deck, value) => { await device.setParam(name, deck, value); },
    getParam: (name, deck) => device.getParam(name, deck),
    setConfig: async (name, deck, value) => { await device.setConfig(name, deck, value); },
    gate: async (deck) => { await device.gate(deck); },
    pad: (action, deck) => device.pad(action, deck),
  };
}

export function oscClient(device: OscDevice): DeviceClient {
  return {
    codec: 'osc',
    exec: (input) => execOsc(device, input),
    destructive: isDestructiveAddress,
    describe: () => device.describe(),
    query: (name, deck = '') => device.query(name, deck),
    cpu: () => device.cpu(),
    resetCpu: async () => { await device.resetCpu(); },
    close: () => device.close(),
    example: '/sk/a/param/speed 0.5',
    setParam: async (name, deck, value) => { await device.setParam(name, deck, value); },
    getParam: (name, deck) => device.getParam(name, deck),
    setConfig: async (name, deck, value) => { await device.setConfig(name, deck, value); },
    gate: async (deck) => { await device.gate(deck); },
    pad: (action, deck) => device.pad(action, deck),
  };
}

// --- the OSC console ------------------------------------------------------------------------------

/**
 * Run one line of OSC console input: an address, then whitespace-separated arguments.
 *
 *     /sk/a/param/speed 0.5     write a float
 *     /sk/a/param/speed         read (no type-tag string at all)
 *     /sk/cfg/route 2           write an int
 *     /sk/a/pad/play true       write a boolean
 *
 * The console does NOT accept line-codec commands. Translating `set param speed a 0.5` into an
 * address would mean a second, hand-written copy of the address-composition rules that
 * `scripts/sk_osc.py` derives from the firmware tables - free to drift the moment a ParamId is added,
 * and drifting silently, since a wrong address answers `unknown-address` rather than failing loudly
 * at the point of the mistake. An OSC build speaks OSC; the generated control surface covers the
 * common cases and this is the escape hatch for the rest.
 */
export async function execOsc(device: OscDevice, input: string): Promise<string> {
  const [address, ...rest] = input.trim().split(/\s+/).filter(Boolean);
  if (!address) return '';
  if (!address.startsWith('/')) {
    throw new Error(`not an OSC address: ${JSON.stringify(address)}. `
      + 'This build speaks OSC - try /sk/dev/describe, or use the controls above.');
  }
  const reply = await device.request(address, ...rest.map(parseArg));
  const vals = Array.isArray(reply) ? reply : [reply];
  return vals.map(String).join(' ');
}

/**
 * Type one console argument from how it was SPELLED.
 *
 * The console is the one place the int/float ambiguity has a good answer. Everywhere else in this
 * codebase a plain number must default to `,f`, because JavaScript cannot tell 1 from 1.0 and a tag
 * that flickered as a fader crossed an integer would be worse than a wrong-but-stable one. Here the
 * user's text survives: `2` is an int, `2.0` is a float, and that is exactly the distinction they
 * meant to draw by typing it.
 */
function parseArg(tok: string): OscArg {
  if (tok === 'true') return true;
  if (tok === 'false') return false;
  if (/^-?\d+$/.test(tok)) return oscInt(Number(tok));
  if (/^-?(\d+\.?\d*|\.\d+)(e[-+]?\d+)?$/i.test(tok)) return Number(tok);
  return tok;
}

/**
 * The OSC analogue of `isDestructive`.
 *
 * Same rationale, restated for an address space: `docs/dev/terminal-target-b.md` warns that sweeping
 * a control surface can clear a recorded buffer or write the card, so these must not fire from a
 * single click. Kept beside the line list rather than derived from it, because the two vocabularies
 * are genuinely different - `reset cpu` is harmless and `/sk/dev/reset` is not, and they do not
 * correspond token for token.
 */
const DESTRUCTIVE_ADDRESSES = [
  /^\/sk(\/[ab])?\/pad\/clear\b/,
  /^\/sk(\/[ab])?\/seq\/clear\b/,
  /^\/sk(\/[ab])?\/clear\b/,
  /^\/sk\/dev\/preset\/save\b/,
  /^\/sk\/dev\/reset(?!\/cpu)/, // /sk/dev/reset/cpu only clears the meter extremes - harmless
];

export function isDestructiveAddress(input: string): boolean {
  const addr = input.trim().split(/\s+/)[0]?.toLowerCase() ?? '';
  return DESTRUCTIVE_ADDRESSES.some((re) => re.test(addr));
}

/**
 * What this console line puts on the wire, for tests and diagnostics.
 *
 * Returns the encoded packet as well as the decoded view, because the thing worth asserting about
 * console input is the one thing decoding throws away: whether `2` went out as `,i` and `2.0` as
 * `,f`. A decoded argument list cannot tell those apart - that is the whole reason `parseArg` reads
 * the spelling rather than the value.
 */
export function consoleLineToPacket(input: string):
{ address: string; args: unknown[]; packet: Uint8Array; tags: string } {
  const [address, ...rest] = input.trim().split(/\s+/).filter(Boolean);
  const packet = encode(address, ...rest.map(parseArg));
  const { args } = decode(packet);
  // The type-tag string, or '' for the read form, which has none at all.
  const text = String.fromCharCode(...packet);
  const i = text.indexOf(',');
  return { address, args, packet, tags: i < 0 ? '' : text.slice(i).replace(/\0.*$/s, '') };
}
