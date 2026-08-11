// client.test.ts - one device surface over either codec, and the session that sits on it.
//
// The point of these tests is the substitution itself: the same view-model, driven the same way,
// against both codecs, producing the same observable state. That is what the TODO item wanted from
// the browser end - not "the page can speak OSC" but "the page does not care" - and it is the only
// claim that stops the two codecs drifting apart in the UI the way the two Python clients drifted
// apart in the sweep.

import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { suite, test, ok, eq, rejects, skip } from './harness.ts';
import { decode, encode } from '../src/core/osc.ts';
import { consoleLineToPacket, isDestructiveAddress, oscClient } from '../src/core/client.ts';
import { OscDevice } from '../src/core/oscdevice.ts';
import { TerminalModel } from '../src/app/terminal_model.ts';
import type { Clock, FrameTransport, SerialPorts, Transport } from '../src/core/ports.ts';

suite('client');

const HERE = dirname(fileURLToPath(import.meta.url));
const SAMPLE = join(HERE, '..', '..', 'host', 'build', 'describe_osc_sample.bin');

function bundleBytes(): Uint8Array {
  if (!existsSync(SAMPLE)) {
    skip('run `make -C host test-terminal-osc` to produce host/build/describe_osc_sample.bin');
  }
  return new Uint8Array(readFileSync(SAMPLE));
}

// --- the OSC console ------------------------------------------------------------------------------

test('console arguments are typed from how they were spelled', () => {
  // The one place the int/float ambiguity has a good answer. Everywhere else a plain number must
  // default to ,f, because JavaScript cannot tell 1 from 1.0 and a tag that flickered as a fader
  // crossed an integer would be worse than a wrong-but-stable one. Here the user's text survives.
  const line = (s: string): { args: unknown[]; tags: string } => {
    const { args, tags } = consoleLineToPacket(s);
    return { args, tags };
  };
  eq(line('/sk/cfg/route 2'), { args: [2], tags: ',i' }, 'an integer spelling is an int');
  eq(line('/sk/cfg/route 2.0'), { args: [2], tags: ',f' }, 'a decimal spelling is a float');
  eq(line('/sk/a/param/speed 0.5'), { args: [0.5], tags: ',f' });
  eq(line('/sk/a/param/speed -0.5'), { args: [-0.5], tags: ',f' });
  // Exponent notation is accepted, and the value comes back as the nearest float32 rather than the
  // decimal typed - OSC's `f` is 32-bit, so 0.001 is not representable. Worth pinning: it is the
  // reason a read-back never exactly equals what a console write sent.
  eq(line('/sk/a/param/speed 1e-3').tags, ',f');
  ok(Math.abs((line('/sk/a/param/speed 1e-3').args[0] as number) - 0.001) < 1e-9);
  eq(line('/sk/a/pad/play true'), { args: [true], tags: ',T' });
  eq(line('/sk/a/pad/play false'), { args: [false], tags: ',F' });
  eq(line('/sk/dev/reset A'), { args: ['A'], tags: ',s' }, 'anything else is a string');
  eq(line('/sk/a/param/speed'), { args: [], tags: '' }, 'no arguments at all is a read');
  eq(line('/sk/midi/note 144 60'), { args: [144, 60], tags: ',ii' });
});

test('the OSC console refuses a line-codec command with a usable message', async () => {
  // Translating `set param speed a 0.5` into an address would be a second, hand-written copy of the
  // composition rules sk_osc.py derives from the firmware tables - free to drift, and drifting into
  // `unknown-address` rather than into an obvious failure.
  const dev = new FakeFrames(() => []);
  const client = oscClient(new OscDevice(dev, { ack: false }));
  const e = await rejects(client.exec('set param speed a 0.5'), 'not an OSC address');
  ok(String(e).includes('/sk/dev/describe'), 'the message suggests what to type instead');
});

test('destructive addresses are recognised, and reset/cpu is not one', () => {
  ok(isDestructiveAddress('/sk/a/pad/clear'));
  ok(isDestructiveAddress('/sk/dev/preset/save 0'));
  ok(isDestructiveAddress('/sk/dev/reset'));
  ok(!isDestructiveAddress('/sk/dev/reset/cpu'), 'clearing the meter extremes is harmless');
  ok(!isDestructiveAddress('/sk/a/param/speed 0.5'));
  ok(!isDestructiveAddress('/sk/a/pad/play'));
});

// --- scripted devices for both codecs -------------------------------------------------------------

class FakeFrames implements FrameTransport {
  readonly sent: Array<{ address: string; args: unknown[] }> = [];
  private cb: (p: Uint8Array) => void = () => {};
  constructor(private readonly reply: (a: string, args: unknown[]) => Uint8Array[]) {}
  async send(p: Uint8Array): Promise<void> {
    const { address, args } = decode(p);
    this.sent.push({ address, args });
    queueMicrotask(() => { for (const r of this.reply(address, args)) this.cb(r); });
  }
  onFrame(cb: (p: Uint8Array) => void): void { this.cb = cb; }
  onClose(): void {}
  async close(): Promise<void> {}
  info(): string { return 'osc-fake'; }
}

class FakeLines implements Transport {
  readonly sent: string[] = [];
  private cb: (l: string) => void = () => {};
  constructor(private readonly reply: (line: string) => string[]) {}
  async write(text: string): Promise<void> {
    const line = text.replace(/\r?\n$/, '');
    this.sent.push(line);
    queueMicrotask(() => { for (const r of this.reply(line)) this.cb(r); });
  }
  onLine(cb: (l: string) => void): void { this.cb = cb; }
  onClose(): void {}
  async close(): Promise<void> {}
  info(): string { return 'line-fake'; }
}

const immediateClock: Clock = { every: () => () => {} };

/** The describe block the line codec would send for the same device the OSC sample describes. */
const LINE_DESCRIBE = [
  'descr engine=mock version=0.6.1 masked=1',
  'param speed deck 0..1',
  'config route global 0:stereo 1:mono',
  'query cpu global float',
  'caps 0x1f',
];

// --- the same session over either codec -----------------------------------------------------------

test('the model connects over the line codec and reports it', async () => {
  const t = new FakeLines((line) => (line === 'describe' ? [...LINE_DESCRIBE, 'end'] : ['ok']));
  const serial: SerialPorts = { supported: () => true, request: async () => t };
  const model = new TerminalModel({ serial, clock: immediateClock, confirm: () => true });
  ok(await model.connect());
  const s = model.store.get();
  eq(s.codec, 'line');
  ok(s.status.includes('line codec'), s.status);
  eq(s.descriptor?.engine, 'mock');
});

test('the model connects over the OSC codec and reports it', async () => {
  const bundle = bundleBytes();
  const t = new FakeFrames((address) => [address === '/sk/dev/describe'
    ? bundle : encode(`/sk/reply${address.slice(3)}`)]);
  const serial: SerialPorts = {
    supported: () => true,
    request: async () => { throw new Error('the line path must not be used'); },
    requestFrames: async () => t,
  };
  const model = new TerminalModel({ serial, clock: immediateClock, confirm: () => true });
  model.setCodec('osc');
  ok(await model.connect());
  const s = model.store.get();
  eq(s.codec, 'osc');
  ok(s.status.includes('OSC codec'), s.status);
  ok(s.descriptor?.params.size, 'the bundle produced a descriptor');
  eq(s.example, '/sk/a/param/speed 0.5');
  // The first thing sent must be the ack enable, or every write is silent and unassertable.
  eq(t.sent[0].address, '/sk/dev/mode/ack');
});

test('setParam reaches the right address in each codec', async () => {
  const lines = new FakeLines((line) => (line === 'describe' ? [...LINE_DESCRIBE, 'end'] : ['ok']));
  const lineModel = new TerminalModel({
    serial: { supported: () => true, request: async () => lines },
    clock: immediateClock, confirm: () => true,
  });
  await lineModel.connect();
  await lineModel.setParam('speed', 'A', 0.5);
  ok(lines.sent.includes('set param speed A 0.5'), lines.sent.join(' | '));

  const bundle = bundleBytes();
  const frames = new FakeFrames((address) => [address === '/sk/dev/describe'
    ? bundle : encode(`/sk/reply${address.slice(3)}`)]);
  const oscModel = new TerminalModel({
    serial: {
      supported: () => true,
      request: async () => { throw new Error('unused'); },
      requestFrames: async () => frames,
    },
    clock: immediateClock, confirm: () => true,
  });
  oscModel.setCodec('osc');
  await oscModel.connect();
  await oscModel.setParam('speed', 'A', 0.5);
  const write = frames.sent.find((s) => s.address.includes('/param/speed'));
  eq(write?.address, '/sk/a/param/speed');
  eq(write?.args, [0.5]);
});

test('a destructive pad press is confirmed in either codec', async () => {
  const asked: string[] = [];
  const bundle = bundleBytes();
  const frames = new FakeFrames((address) => [address === '/sk/dev/describe'
    ? bundle : encode(`/sk/reply${address.slice(3)}`)]);
  const model = new TerminalModel({
    serial: {
      supported: () => true,
      request: async () => { throw new Error('unused'); },
      requestFrames: async () => frames,
    },
    clock: immediateClock,
    confirm: (what) => { asked.push(what); return false; },
  });
  model.setCodec('osc');
  await model.connect();
  await model.pad('clear', 'A');
  eq(asked, ['Send "pad clear A"?']);
  // Assert on the addresses, not on a count: connect() kicks off a CPU poll whose replies land
  // asynchronously, so the length of `sent` moves on its own and a count would flake.
  ok(!frames.sent.some((s) => s.address.includes('/pad/clear')), 'a refused clear sends nothing');
  // A safe press is not confirmed, and does reach the device.
  await model.pad('play', 'A');
  eq(asked.length, 1, 'play was not confirmed');
  ok(frames.sent.some((s) => s.address === '/sk/a/pad/play'), frames.sent.map((s) => s.address).join(' '));
});

test('the codec cannot be switched mid-session', async () => {
  // It is a property of the firmware, not the connection: switching live would leave the client
  // speaking a language the device does not.
  const t = new FakeLines((line) => (line === 'describe' ? [...LINE_DESCRIBE, 'end'] : ['ok']));
  const model = new TerminalModel({
    serial: { supported: () => true, request: async () => t },
    clock: immediateClock, confirm: () => true,
  });
  await model.connect();
  model.setCodec('osc');
  eq(model.store.get().codec, 'line', 'ignored while connected');
  await model.disconnect();
  model.setCodec('osc');
  eq(model.store.get().codec, 'osc', 'accepted once disconnected');
});

test('a page whose serial port cannot do frames says so instead of throwing', async () => {
  const model = new TerminalModel({
    serial: { supported: () => true, request: async () => new FakeLines(() => ['ok']) },
    clock: immediateClock, confirm: () => true,
  });
  eq(model.oscSupported(), false);
  model.setCodec('osc');
  eq(await model.connect(), false);
  ok(model.store.get().error?.includes('OSC'), model.store.get().error ?? '(no error)');
});
