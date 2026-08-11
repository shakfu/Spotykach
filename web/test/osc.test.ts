// osc.test.ts - the OSC wire format and SLIP framing, with no device and no browser.
//
// A port of tools/test_osc_codec.py's codec half, and deliberately so: the two front-ends implement
// the same codec against the same firmware, so they should be held to the same vectors. Anything
// asserted here that is not asserted there (or the reverse) is a place the two clients are free to
// drift, which is exactly the class of bug the 63/63 cross-codec parity sweep was run to find - five
// of its five defects were a CLIENT difference wearing a codec's clothes.
//
// The describe fixture is not hand-written. It is `host/build/describe_osc_sample.bin`, emitted by
// host/test_terminal_osc.cpp from the real firmware encode path, which is why these tests skip rather
// than fail when the host suites have not been built.

import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { suite, test, ok, eq, throws, skip } from './harness.ts';
import {
  END, ESC, SlipDecoder, slipEncode,
  encode, decode, decodePacket, isBundle, oscInt, type OscValue,
} from '../src/core/osc.ts';

suite('osc');

const HERE = dirname(fileURLToPath(import.meta.url));
const SAMPLE = join(HERE, '..', '..', 'host', 'build', 'describe_osc_sample.bin');

/** The describe bundle as the firmware encodes it, or a skip explaining how to produce it. */
function bundle(): Uint8Array {
  if (!existsSync(SAMPLE)) {
    skip('run `make -C host test-terminal-osc` to produce host/build/describe_osc_sample.bin');
  }
  return new Uint8Array(readFileSync(SAMPLE));
}

const bytes = (...b: number[]): Uint8Array => Uint8Array.from(b);
const ascii = (u: Uint8Array): string => [...u].map((c) => String.fromCharCode(c)).join('');

// --- SLIP ---------------------------------------------------------------------------------------

test('slip escapes both special bytes and round-trips', () => {
  const payload = bytes(0x01, END, 0x02, ESC, 0x03, END, ESC);
  const wire = slipEncode(payload);
  eq(wire[0], END, 'frame starts with END');
  eq(wire[wire.length - 1], END, 'frame ends with END');
  ok(!wire.subarray(1, -1).includes(END), 'every interior END was escaped');
  eq(new SlipDecoder().feed(wire), [payload]);
});

test('slip decoder is incremental at every byte boundary', () => {
  // The classic serial defect, and the reason the framer is separable from the port: the device
  // drains its FIFO in 64-byte chunks, so a frame boundary lands mid-chunk as a matter of course.
  const wire = new Uint8Array([...slipEncode(new TextEncoder().encode('abcd')),
    ...slipEncode(new TextEncoder().encode('efgh'))]);
  const dec = new SlipDecoder();
  const got: Uint8Array[] = [];
  for (let i = 0; i < wire.length; i++) got.push(...dec.feed(wire.subarray(i, i + 1)));
  eq(got.map(ascii), ['abcd', 'efgh']);
});

test('slip treats back-to-back ENDs as padding, not empty packets', () => {
  eq(new SlipDecoder().feed(bytes(END, END, END)), []);
});

test('slip decoder yields every frame in one chunk', () => {
  // The bug the Python client carries a comment about: one read can contain several frames, and
  // returning the first while dropping the rest desynchronizes the session silently - every later
  // request answered by its predecessor's reply, which reads as wrong values rather than an error.
  const wire = new Uint8Array([...slipEncode(bytes(1)), ...slipEncode(bytes(2)),
    ...slipEncode(bytes(3))]);
  eq(new SlipDecoder().feed(wire).map((f) => f[0]), [1, 2, 3]);
});

// --- OSC wire -----------------------------------------------------------------------------------

test('encode is big-endian and 4-byte aligned', () => {
  const pkt = encode('/sk/a/param/speed', 0.5);
  eq(pkt.length % 4, 0, 'padded to a multiple of four');
  ok(ascii(pkt).startsWith('/sk/a/param/speed\0'), 'address then NUL');
  ok(ascii(pkt).includes(',f'), 'a float tag');
  eq([...pkt.subarray(-4)], [0x3f, 0x00, 0x00, 0x00], '0.5 big-endian');
});

test('a read is the bare address with no type-tag string', () => {
  // Not an empty tag string - the absence of one. This is how a read is spelled, so getting it wrong
  // turns every get into a malformed packet.
  const pkt = encode('/sk/a/param/speed');
  eq(ascii(pkt), '/sk/a/param/speed\0\0\0');
  const { address, args } = decode(pkt);
  eq(address, '/sk/a/param/speed');
  eq(args, []);
});

test('encode/decode round-trips every supported argument type', () => {
  const cases: Array<{ args: Parameters<typeof encode> extends [string, ...infer A] ? A : never;
    want: OscValue[] }> = [
    { args: [0.5], want: [0.5] },
    { args: [oscInt(3)], want: [3] },
    { args: ['deck-a'], want: ['deck-a'] },
    { args: [true], want: [true] },
    { args: [false], want: [false] },
    { args: [oscInt(144), oscInt(60), oscInt(100)], want: [144, 60, 100] },
    { args: [0.25, true], want: [0.25, true] },
  ];
  for (const { args, want } of cases) {
    const { address, args: got } = decode(encode('/sk/x', ...args));
    eq(address, '/sk/x');
    eq(got, want, `round-trip of ${JSON.stringify(want)}`);
  }
});

test('a plain number is a float and oscInt is an int', () => {
  // The one place this port cannot follow the Python, which separates 1 from 1.0 by type. Guessing
  // from Number.isInteger would make the tag flicker with the value as a fader crossed 1.0.
  ok(ascii(encode('/sk/x', 1)).includes(',f'), '1 is a float');
  ok(ascii(encode('/sk/x', 1.5)).includes(',f'), '1.5 is a float');
  ok(ascii(encode('/sk/x', oscInt(1))).includes(',i'), 'oscInt(1) is an int');
  // Booleans are the tag itself - T/F carry no body bytes, so a trigger is 0 payload.
  ok(ascii(encode('/sk/a/gate', true)).includes(',T'), 'true is T');
  eq(encode('/sk/a/gate', true).length, encode('/sk/a/gate', false).length);
});

test('strings are NUL-terminated and padded, including at an exact multiple of four', () => {
  // "abcd" is 4 bytes, so its NUL forces a whole extra word. Off-by-one here corrupts every
  // following argument rather than this one, which is what makes it hard to spot on a device.
  const { args } = decode(encode('/sk/x', 'abcd', 0.5));
  eq(args, ['abcd', 0.5]);
  eq(encode('/sk/x', 'abcd').length % 4, 0);
});

test('decode rejects an unknown type tag rather than guessing', () => {
  const pkt = new Uint8Array([...encode('/sk/x')]);
  const bad = new Uint8Array([...pkt, ...new TextEncoder().encode(',q\0\0'), 0, 0, 0, 0]);
  throws(() => decode(bad), 'unsupported OSC type tag');
});

test('decode rejects an unterminated string rather than reading past the end', () => {
  throws(() => decode(bytes(0x2f, 0x73, 0x6b)), 'unterminated');
});

test('decode reads a packet that is a view into a larger buffer', () => {
  // decodePacket recurses with subarray(), and slipEncode returns a subarray too, so a DataView
  // built on .buffer without honouring byteOffset would read the wrong bytes - and would do it only
  // for bundle members, i.e. only for describe.
  const pkt = encode('/sk/x', 0.5);
  const backing = new Uint8Array(pkt.length + 8);
  backing.set(pkt, 5);
  eq(decode(backing.subarray(5, 5 + pkt.length)).args, [0.5]);
});

// --- the describe bundle, from real firmware bytes ------------------------------------------------

test('the describe sample is one bundle', () => {
  // One bundle means one SLIP frame: the descriptor arrives atomically or not at all, which is why
  // the device sizes its TX FIFO to hold a whole one.
  ok(isBundle(bundle()), 'the sample is a #bundle');
});

test('the describe bundle decodes to the expected rows', () => {
  const addrs = decodePacket(bundle()).map((p) => p.address);
  eq(addrs.filter((a) => a === '/sk/reply/dev/describe').length, 1, 'one header row');
  eq(addrs.filter((a) => a === '/sk/reply/dev/describe/caps').length, 1, 'one caps row');
  ok(addrs.includes('/sk/reply/dev/describe/param'), 'param rows');
  ok(addrs.includes('/sk/reply/dev/describe/state'), 'state rows');
});

test('param rows carry address, label, range and scope', () => {
  const rows = decodePacket(bundle())
    .filter((p) => p.address === '/sk/reply/dev/describe/param').map((p) => p.args);
  ok(rows.length, 'the bundle has param rows at all');
  for (const [addr, label, lo, hi, scope] of rows) {
    ok(String(addr).startsWith('/sk/'), `${addr} is an sk address`);
    ok(String(addr).includes('/param/'), `${addr} is a param address`);
    ok(label, `${addr} has a label`); // never empty: falls back to the slot name
    eq([lo, hi], [0, 1], `${addr} is normalized`);
    ok(scope === 'deck' || scope === 'global', `${addr} scope ${scope}`);
    // Scope is encoded STRUCTURALLY: a global param carries no deck segment. This is the difference
    // that made the Python client drop the deck a line-codec caller passes for a global.
    eq(scope === 'deck', ['a', 'b'].includes(String(addr).split('/')[2]), `${addr} scope matches shape`);
  }
});

test('the whole bundle survives a SLIP round-trip', () => {
  // The end-to-end property a session depends on: ~2 KB of descriptor, escaped, reassembled from
  // 64-byte chunks the way the transport delivers it, still decodes to the identical rows.
  const raw = bundle();
  const wire = slipEncode(raw);
  const dec = new SlipDecoder();
  const frames: Uint8Array[] = [];
  for (let i = 0; i < wire.length; i += 64) frames.push(...dec.feed(wire.subarray(i, i + 64)));
  eq(frames.length, 1, 'one frame out');
  eq(decodePacket(frames[0]).length, decodePacket(raw).length, 'same row count');
  eq(frames[0], raw, 'byte-identical after escaping and unescaping');
});
