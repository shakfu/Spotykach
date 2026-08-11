// oscdevice.test.ts - the OSC command client, driven by a scripted device.
//
// The parity claim this file exists to defend: `OscDevice` and `Device` expose the same surface, so
// the view-model above them cannot tell which codec is underneath. The Python pair proved that on
// hardware (63/63) and every defect the sweep found was a CLIENT difference - a client returning a
// typed value where the other returned text, addressing a global with a deck segment, disagreeing
// about a reply's framing. Those five shapes are the tests below, asserted here rather than
// rediscovered on a bench.

import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { suite, test, ok, eq, rejects, skip } from './harness.ts';
import { decode, encode, oscInt, slipEncode, SlipDecoder, type OscArg } from '../src/core/osc.ts';
import { OscDevice, describeFromRows } from '../src/core/oscdevice.ts';
import { decodePacket } from '../src/core/osc.ts';
import { CommandError, Timeout } from '../src/core/protocol.ts';
import type { FrameTransport } from '../src/core/ports.ts';
import { OscSerialTransport, DAISY_VID } from '../src/platform/serial.ts';

suite('oscdevice');

const HERE = dirname(fileURLToPath(import.meta.url));
const SAMPLE = join(HERE, '..', '..', 'host', 'build', 'describe_osc_sample.bin');

function bundleBytes(): Uint8Array {
  if (!existsSync(SAMPLE)) {
    skip('run `make -C host test-terminal-osc` to produce host/build/describe_osc_sample.bin');
  }
  return new Uint8Array(readFileSync(SAMPLE));
}

/**
 * A scripted device: it records what was sent and answers from a handler.
 *
 * The handler returns the packets to reply with, so a test can answer nothing (a silent write with
 * acks off), one frame, or several in one delivery - which is the case that broke `_recv()` in the
 * Python client and is worth holding this one to.
 */
class FakeDevice implements FrameTransport {
  readonly sent: Array<{ address: string; args: unknown[] }> = [];
  private cb: (p: Uint8Array) => void = () => {};
  private closeCb: (r: string) => void = () => {};
  closed = false;

  constructor(private readonly reply: (address: string, args: unknown[]) => Uint8Array[] = () => []) {}

  async send(packet: Uint8Array): Promise<void> {
    const { address, args } = decode(packet);
    this.sent.push({ address, args });
    // Asynchronous, as a real port is: the client must be waiting before the answer lands.
    queueMicrotask(() => { for (const r of this.reply(address, args)) this.cb(r); });
  }

  onFrame(cb: (p: Uint8Array) => void): void { this.cb = cb; }
  onClose(cb: (r: string) => void): void { this.closeCb = cb; }
  async close(): Promise<void> { this.closed = true; }
  info(): string { return 'fake'; }
  drop(why: string): void { this.closeCb(why); }
}

/** A device that acks every write with an empty reply and answers reads from a table. */
function scripted(values: Record<string, OscArg[]> = {}): FakeDevice {
  return new FakeDevice((address) => [encode(`/sk/reply${address.slice(3)}`,
    ...(values[address] ?? []))]);
}

/** A device whose describe answers with the real firmware bundle. */
function describing(extra: Record<string, OscArg[]> = {}): FakeDevice {
  const bundle = bundleBytes();
  return new FakeDevice((address) => (address === '/sk/dev/describe'
    ? [bundle]
    : [encode(`/sk/reply${address.slice(3)}`, ...(extra[address] ?? []))]));
}

// --- framing --------------------------------------------------------------------------------------

test('acks are enabled at connect, so a write is assertable', async () => {
  // Without this a successful write is silent by design, and a UI cannot tell an applied address
  // from a rejected one.
  const dev = scripted();
  const client = new OscDevice(dev);
  await client.send('/sk/x');
  eq(dev.sent[0].address, '/sk/dev/mode/ack');
  eq(dev.sent[0].args, [true]);
});

test('a read is answered and returns the single value unwrapped', async () => {
  const dev = scripted({ '/sk/dev/caps': [oscInt(0x1f)] });
  eq(await new OscDevice(dev, { ack: false }).caps(), 0x1f);
});

test('an /sk/err reply becomes a CommandError carrying the reason', async () => {
  // The request address is echoed alongside the reason; nothing else correlates a rejection back to
  // what caused it.
  const dev = new FakeDevice((address) => [encode('/sk/err', address, 'unknown-address')]);
  const e = await rejects(new OscDevice(dev, { ack: false }).caps(), CommandError);
  eq((e as CommandError).reason, 'unknown-address');
});

test('a silent device times out rather than hanging', async () => {
  const dev = new FakeDevice(() => []);
  await rejects(new OscDevice(dev, { ack: false, timeout: 20 }).caps(), Timeout);
});

test('the three cpu reads are sequential and land in the right fields', async () => {
  // One command in flight, so these are three requests, not one answered three times. The ordering
  // is the assertion: a client that mapped them by arrival rather than by request would still
  // produce three plausible numbers, in the wrong fields, and only under load.
  const values: Record<string, OscArg[]> = {
    '/sk/dev/cpu': [41.5], '/sk/dev/cpumin': [12.0], '/sk/dev/cpumax': [91.3],
  };
  const dev = scripted(values);
  const client = new OscDevice(dev, { ack: false });
  eq(await client.cpu(), { avg: 41.5, min: 12, max: 91.3 });
  eq(dev.sent.map((s) => s.address), ['/sk/dev/cpu', '/sk/dev/cpumin', '/sk/dev/cpumax']);
});

test('frames batched into one delivery are all kept, in order', async () => {
  // The bug the Python client carries a comment about, at the level it actually happens: the
  // transport hands over whatever one read contained. Dropping the extras would mean every later
  // request is answered by its predecessor's reply - wrong values, not an error.
  const dec = new SlipDecoder();
  const wire = new Uint8Array([
    ...slipEncode(encode('/sk/reply/dev/cpu', 41.5)),
    ...slipEncode(encode('/sk/reply/dev/cpumin', 12.0)),
  ]);
  const frames = dec.feed(wire);
  eq(frames.length, 2, 'both frames came out of one delivery');
  eq(frames.map((f) => decode(f).address), ['/sk/reply/dev/cpu', '/sk/reply/dev/cpumin']);
});

test('a stale reply is drained rather than answering the next request', async () => {
  const dev = scripted({ '/sk/dev/caps': [oscInt(7)] });
  const logs: string[] = [];
  const client = new OscDevice(dev, { ack: false, logSink: (l) => logs.push(l) });
  eq(await client.caps(), 7);
  // Push an unsolicited frame, then make a fresh request: it must not be answered by the leftover.
  await client.send('/sk/dev/caps');
  await new Promise((r) => setTimeout(r, 0));
  eq(await client.caps(), 7);
  ok(logs.some((l) => l.startsWith('[stale]')), `expected a stale note, got ${JSON.stringify(logs)}`);
});

test('a /sk/log frame is routed to the log sink, not returned as a reply', async () => {
  // Phase 1 forces INFS_LOG=0 so none can arrive today; this is what lets the deferred /sk/log
  // framing land without a client change.
  const dev = new FakeDevice((address) => [encode('/sk/log', 'tape: slot 2 loaded'),
    encode(`/sk/reply${address.slice(3)}`, oscInt(3))]);
  const logs: string[] = [];
  eq(await new OscDevice(dev, { ack: false, logSink: (l) => logs.push(l) }).caps(), 3);
  eq(logs, ['tape: slot 2 loaded']);
});

// --- scope: the address space's structural difference ---------------------------------------------

test('a deck-scoped param carries its deck segment', async () => {
  const dev = describing();
  const client = new OscDevice(dev, { ack: false });
  await client.setParam('speed', 'a', 0.5);
  const write = dev.sent.find((s) => s.address.includes('/param/speed'));
  eq(write?.address, '/sk/a/param/speed');
  eq(write?.args, [0.5]);
});

test('a global param drops the deck a line-codec caller passes', async () => {
  // The line codec makes a caller pass a deck for a global and then discards it. The OSC space
  // encodes scope structurally, so `/sk/a/param/<global>` is `unknown-address` - and a client that
  // failed to drop it would fail every global against OSC and pass against lines.
  const dev = describing();
  const rows = decodePacket(bundleBytes())
    .filter((p) => p.address === '/sk/reply/dev/describe/param' && p.args[4] === 'global');
  if (!rows.length) skip('the sample descriptor has no global param to exercise');
  const name = String(rows[0].args[0]).split('/').pop()!;
  const client = new OscDevice(dev, { ack: false });
  await client.setParam(name, 'a', 0.25);
  const write = dev.sent.find((s) => s.address.includes(`/param/${name}`));
  eq(write?.address, `/sk/param/${name}`, 'no deck segment on a global');
});

test('route is addressed as the global config it is', async () => {
  const dev = describing();
  await new OscDevice(dev, { ack: false }).setConfig('route', 'a', 1);
  eq(dev.sent.find((s) => s.address.includes('/cfg/route'))?.address, '/sk/cfg/route');
});

// --- values: typed on the wire, text at the surface -----------------------------------------------

test('query renders typed replies back to the text the line client returns', async () => {
  // A client handing back `true` or `0.5` would fail every assertion written against the line codec
  // and pass against OSC - a client difference masquerading as a firmware one.
  const dev = describing({
    '/sk/dev/cpu': [41.5],
    '/sk/dev/usb': ['boot=1 sof=1'],
  });
  const client = new OscDevice(dev, { ack: false });
  eq(await client.query('cpu'), '41.5000', 'a float gets the line codec\'s 4 decimals');
  eq(await client.query('usb'), 'boot=1 sof=1', 'text passes through');
});

test('a config is written as an int and a param as a float', async () => {
  const dev = describing();
  const client = new OscDevice(dev, { ack: false });
  await client.setParam('speed', 'a', 1);
  await client.setConfig('route', 'a', 2);
  const param = dev.sent.find((s) => s.address === '/sk/a/param/speed')!;
  const cfg = dev.sent.find((s) => s.address === '/sk/cfg/route')!;
  // Re-encode and read the tag back: this is the distinction JavaScript's number type erases.
  ok(String.fromCharCode(...encode(param.address, 0.5)).includes(',f'));
  eq(param.args, [1]);
  eq(cfg.args, [2]);
});

test('pad play strips the ok framing exactly as the line client does', async () => {
  // That handler composes free-form text the typed sink cannot describe, so the OSC reply carries the
  // whole line. The line client strips `ok `; so must this one, or the two report different values
  // for a press the two codecs agree about.
  const dev = new FakeDevice((address) => [encode(`/sk/reply${address.slice(3)}`, 'ok empty=0')]);
  eq(await new OscDevice(dev, { ack: false }).pad('play', 'a'), 'empty=0');
});

test('a bare pad press reports an empty payload, as the line client does', async () => {
  const dev = scripted();
  eq(await new OscDevice(dev, { ack: false }).pad('stop', 'a'), '');
});

// --- describe: the same model the line codec produces ---------------------------------------------

test('describe reduces the bundle to the line codec\'s Descriptor', async () => {
  const d = await new OscDevice(describing(), { ack: false }).describe();
  ok(d.engine, 'an engine name');
  ok(d.params.size, 'params');
  ok(d.queries.size, 'queries');
  for (const p of d.params.values()) {
    eq([p.lo, p.hi], [0, 1], `${p.name} is normalized`);
    ok(p.scope === 'deck' || p.scope === 'global', `${p.name} has a scope`);
  }
});

test('deck-scoped rows arriving once per deck collapse to one entry', () => {
  // The device expands them so a host need not know decks exist; the line codec carries bare names.
  const rows = decodePacket(bundleBytes());
  const paramRows = rows.filter((r) => r.address === '/sk/reply/dev/describe/param');
  const d = describeFromRows(rows);
  const distinct = new Set(paramRows.map((r) => String(r.args[0]).split('/').pop()));
  eq(d.params.size, distinct.size, 'one entry per distinct name, not per deck row');
  ok(paramRows.length > distinct.size, 'the bundle really did carry per-deck rows');
});

test('an unknown describe row is ignored rather than rejected', () => {
  const d = describeFromRows([
    { address: '/sk/reply/dev/describe', args: ['tape', '0.6.1', 'masked=1'] },
    { address: '/sk/reply/dev/describe/something-new', args: ['whatever'] },
    { address: '/sk/reply/dev/describe/caps', args: [31] },
  ]);
  eq(d.engine, 'tape');
  eq(d.masked, true);
  eq(d.caps, 31);
});

// --- the WebSerial frame transport ----------------------------------------------------------------

test('the frame transport reassembles a bundle split across reads', async () => {
  // The whole descriptor in 64-byte chunks, which is how the device drains its FIFO. Note there is no
  // TextDecoder in this path: UTF-8 decoding a bundle would mangle the very bytes SLIP delimits with.
  const raw = bundleBytes();
  const wire = slipEncode(raw);
  const port = {
    readable: new ReadableStream({
      start(c: ReadableStreamDefaultController<Uint8Array>) {
        for (let i = 0; i < wire.length; i += 64) c.enqueue(wire.subarray(i, i + 64));
        c.close();
      },
    }),
    writable: new WritableStream<Uint8Array>({ write() {} }),
    async open() {}, async close() {}, async setSignals() {},
    getInfo: () => ({ usbVendorId: DAISY_VID, usbProductId: 0x5740 }),
  };
  const t = new OscSerialTransport(port as never);
  const got = await new Promise<Uint8Array>((resolve, reject) => {
    t.onFrame(resolve);
    setTimeout(() => reject(new Error('no frame arrived')), 1000);
  });
  eq(got, raw);
  eq(t.info(), `USB 0x0483:0x5740`);
});

test('the frame transport SLIP-encodes what it sends', async () => {
  const written: Uint8Array[] = [];
  const port = {
    readable: new ReadableStream({ start(c: ReadableStreamDefaultController<Uint8Array>) { c.close(); } }),
    writable: new WritableStream<Uint8Array>({ write(c) { written.push(c); } }),
    async open() {}, async close() {}, async setSignals() {},
    getInfo: () => ({}),
  };
  const t = new OscSerialTransport(port as never);
  const packet = encode('/sk/a/param/speed', 0.5);
  await t.send(packet);
  eq(written.length, 1);
  eq(new SlipDecoder().feed(written[0]), [packet], 'what arrived unwraps to what was sent');
});
