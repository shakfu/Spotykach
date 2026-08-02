// model.test.ts - the view-models, with no DOM and no browser.
//
// This file is what the src/core + src/app split bought. Every question in it used to need the DOM
// shim (and two of them needed a device): does Convert refuse to run with nothing queued, does a
// decode failure leave the buttons recoverable, does an unplugged port tear the session down, does the
// CPU poll stop when the device stops answering. They are now ordinary unit tests over plain objects,
// because everything the browser owns enters through a port in src/core/ports.ts and every fake below
// is a few lines long.

import { suite, test, ok, eq, layoutData, readWeb } from './harness.ts';
import { makeLayout } from '../src/core/layout.ts';
import type { AudioDecoder, CardAccess, Clock, SerialPorts, Transport } from '../src/core/ports.ts';
import type { Card, CardFile } from '../src/core/types.ts';
import { BuildModel } from '../src/app/build_model.ts';
import { ConvertModel, type InputFile } from '../src/app/convert_model.ts';
import { VerifyModel } from '../src/app/verify_model.ts';
import { ReferenceModel } from '../src/app/reference_model.ts';
import { TerminalModel } from '../src/app/terminal_model.ts';
import { Store } from '../src/app/store.ts';

suite('model');

const layout = makeLayout(layoutData());
const patches = readWeb<Record<string, string>>('patches.json');

// --- fakes ----------------------------------------------------------------------------------------

/** A decoder that returns silence of a known length, and records what it was asked for. */
function fakeDecoder(seconds = 1): AudioDecoder & { calls: Array<{ rate: number; channels: number }> } {
  const calls: Array<{ rate: number; channels: number }> = [];
  return {
    calls,
    async decode(_data, rate, channels) {
      calls.push({ rate, channels });
      return {
        samples: new Float32Array(Math.floor(seconds * rate) * channels),
        rate,
        channels,
        sourceRate: 44100,
        sourceChannels: 2,
      };
    },
  };
}

function fakeAccess(direct: boolean): CardAccess & { written: CardFile[] } {
  const written: CardFile[] = [];
  return {
    written,
    hasDirectAccess: () => direct,
    async pickDirectory() {
      return { files: [], dirs: new Set<string>(), handle: {} } as Card;
    },
    async writeInto(_handle, files) {
      written.push(...files);
      return { written: files.map((f) => f.path), failed: [] };
    },
  };
}

const fakeDownloader = () => {
  const saved: Array<{ name: string; bytes: number }> = [];
  return { saved, save: (b: Uint8Array, name: string) => saved.push({ name, bytes: b.length }) };
};

const inputFile = (name: string, bytes = 1024): InputFile => ({
  name, size: bytes, bytes: async () => new ArrayBuffer(bytes),
});

/** A clock whose ticks the test fires by hand. */
function fakeClock(): Clock & { tick(): Promise<void>; running: boolean } {
  let fn: (() => void) | null = null;
  return {
    get running() {
      return fn != null;
    },
    every(_ms, cb) {
      fn = cb;
      return () => { fn = null; };
    },
    async tick() {
      fn?.();
      await Promise.resolve();
    },
  };
}

/** A transport that answers from a script, and can be made to vanish. */
function fakeTransport(reply: (cmd: string) => string | string[]): Transport & {
  vanish(reason: string): void;
  sent: string[];
} {
  let onLine: (l: string) => void = () => {};
  let onClose: (r: string) => void = () => {};
  const sent: string[] = [];
  return {
    sent,
    async write(text) {
      const cmd = text.replace(/\r?\n$/, '');
      sent.push(cmd);
      for (const l of ([] as string[]).concat(reply(cmd))) onLine(l);
    },
    onLine(cb) { onLine = cb; },
    onClose(cb) { onClose = cb; },
    async close() {},
    info: () => 'USB 0x0483:0x5740',
    vanish: (reason) => onClose(reason),
  };
}

const fakeSerial = (transport: Transport | (() => Promise<Transport>)): SerialPorts => ({
  supported: () => true,
  request: async () => (typeof transport === 'function' ? transport() : transport),
});

// --- store ----------------------------------------------------------------------------------------

test('the store notifies on change and replays current state to a new subscriber', () => {
  const s = new Store({ n: 0 });
  const seen: number[] = [];
  const off = s.subscribe((st) => seen.push(st.n));
  s.set({ n: 1 });
  off();
  s.set({ n: 2 });
  eq(seen, [0, 1], 'the immediate call, then the change - and nothing after unsubscribing');
});

test('the store replaces rather than mutates, so a snapshot stays valid', () => {
  const s = new Store({ n: 0 });
  const before = s.get();
  s.set({ n: 1 });
  eq(before.n, 0, 'the old object is untouched');
  eq(s.get().n, 1);
});

// --- build ----------------------------------------------------------------------------------------

test('build offers in-place writing only where the browser can do it', () => {
  eq(new BuildModel(layout, patches, {
    access: fakeAccess(false), downloader: fakeDownloader(),
  }).canWriteInPlace(), false);
  eq(new BuildModel(layout, patches, {
    access: fakeAccess(true), downloader: fakeDownloader(),
  }).canWriteInPlace(), true);
});

test('build hands the whole card to the downloader as one zip', async () => {
  const downloader = fakeDownloader();
  const model = new BuildModel(layout, patches, { access: fakeAccess(false), downloader });
  await model.downloadZip();
  eq(downloader.saved.length, 1);
  eq(downloader.saved[0].name, 'sk-card-starter.zip');
  ok(downloader.saved[0].bytes > 1000, 'a real archive, not an empty one');
  eq(model.store.get().verdict!.kind, 'good');
});

test('build tops up an existing card instead of overwriting what is on it', async () => {
  // The failure this prevents is silent and destructive: pointing the builder at a card whose
  // SK/config.txt the user tuned must not reset it.
  const access = fakeAccess(true);
  access.pickDirectory = async () => ({
    files: [{ path: 'SK/config.txt', size: 4, read: async () => new Uint8Array(4) }],
    dirs: new Set<string>(),
    handle: {},
  });
  const model = new BuildModel(layout, patches, { access, downloader: fakeDownloader() });
  await model.writeInPlace();
  ok(!access.written.some((f) => f.path === 'SK/config.txt'), 'the existing config was left alone');
  ok(access.written.length > 0, 'but everything missing was written');
  ok(model.store.get().status.includes('1 already present'));
});

test('a dismissed picker is not reported as a failure', async () => {
  const access = fakeAccess(true);
  access.pickDirectory = async () => {
    throw Object.assign(new Error('cancelled'), { name: 'AbortError' });
  };
  const model = new BuildModel(layout, patches, { access, downloader: fakeDownloader() });
  eq(await model.writeInPlace(), false);
  eq(model.store.get().error, null, 'cancelling is not an error and must not look like one');
});

// --- convert --------------------------------------------------------------------------------------

const convertModel = (decoder: AudioDecoder = fakeDecoder(), direct = false) => new ConvertModel(layout, {
  decoder, access: fakeAccess(direct), downloader: fakeDownloader(),
});

test('convert starts with nothing to do and knows it', () => {
  const m = convertModel();
  eq(m.canConvert(), false);
  eq(m.canSaveToCard(), false);
});

test('convert shows only the fields its target template uses', () => {
  const m = convertModel();
  m.setEngine('granular');
  eq(m.fields(), { deck: false, bank: false, tape: true, rate: false });
  m.setEngine('tape');
  eq(m.fields(), { deck: true, bank: false, tape: false, rate: false });
  m.setEngine('radio');
  eq(m.fields(), { deck: false, bank: true, tape: false, rate: false });
});

test('bard is the one bank with a rate control, and it starts at 24 kHz', () => {
  // Half the bytes per hour, which for an audiobook shelf is the difference that matters.
  const m = convertModel();
  m.setEngine('bard');
  eq(m.fields().rate, true);
  eq(m.store.get().rate, 24000);
  m.setEngine('tape');
  eq(m.store.get().rate, 48000, 'and a fixed-rate bank goes back to its own rate');
});

test('convert decodes at the rate and channel count the bank demands', async () => {
  const decoder = fakeDecoder();
  const m = convertModel(decoder);
  m.setEngine('granular'); // stereo, 48 kHz
  m.addFiles([inputFile('a.mp3')]);
  await m.convert();
  eq(decoder.calls, [{ rate: 48000, channels: 2 }]);
  eq(m.store.get().results.map((r) => r.path), ['SK/B/1.WAV']);
});

test('convert numbers slots consecutively from the chosen start', async () => {
  const m = convertModel();
  m.setEngine('tape');
  m.setField('slot', 3);
  m.addFiles([inputFile('a.mp3'), inputFile('b.mp3')]);
  await m.convert();
  eq(m.store.get().results.map((r) => r.path), ['tapes/tape_a_3.wav', 'tapes/tape_a_4.wav']);
});

test('a decode failure is reported and leaves nothing half-converted', async () => {
  const decoder: AudioDecoder = {
    decode: async () => { throw new Error('Unable to decode audio data'); },
  };
  const m = convertModel(decoder);
  m.addFiles([inputFile('broken.mp3')]);
  await m.convert();
  const s = m.store.get();
  eq(s.results, [], 'no partial output');
  eq(s.busy, false, 'and the tab is usable again');
  ok(s.error!.includes('Unable to decode'));
});

test('saving to the card is offered only where the browser can write', () => {
  const withAccess = convertModel(fakeDecoder(), true);
  withAccess.addFiles([inputFile('a.mp3')]);
  eq(withAccess.canSaveToCard(), false, 'not until something has actually been converted');
});

test('removing a queued file invalidates the previous results', async () => {
  const m = convertModel();
  m.addFiles([inputFile('a.mp3'), inputFile('b.mp3')]);
  await m.convert();
  eq(m.store.get().results.length, 2);
  m.removeFile(0);
  eq(m.store.get().results, [], 'stale results must not be downloadable after the queue changes');
  eq(m.store.get().files.map((f) => f.name), ['b.mp3']);
});

// --- verify ---------------------------------------------------------------------------------------

test('verify reports a clean card as clean, and says it can be edited', async () => {
  const m = new VerifyModel(layout);
  await m.run(async () => ({
    files: [{ path: 'tapes/README.TXT', size: 10, read: async () => new Uint8Array(10) }],
    dirs: new Set(['tapes']),
    handle: {},
  }));
  const s = m.store.get();
  eq(s.checked, true);
  eq(s.editable, true, 'a writable handle means in-place editing is available');
  eq(s.fileCount, 1);
});

test('verify reports a folder that is not a card at all', async () => {
  const m = new VerifyModel(layout);
  await m.run(async () => ({ files: [], dirs: new Set(['Documents']), handle: null }));
  const s = m.store.get();
  eq(s.editable, false);
  ok(s.summary!.errors.some((f) => f.problem.includes('no recognised engine folders')));
});

test('a dismissed folder picker leaves verify silent rather than red', async () => {
  const m = new VerifyModel(layout);
  await m.run(async () => {
    throw Object.assign(new Error('cancelled'), { name: 'AbortError' });
  });
  eq(m.store.get().error, null);
  eq(m.store.get().checked, false);
});

// --- reference ------------------------------------------------------------------------------------

test('the reference text box is a search, and a chip is a selection', () => {
  // The distinction is the point: `tape` appears in granular's blurb and in shuttle's filenames, so a
  // chip that filtered by substring would answer a request for one engine with four.
  const m = new ReferenceModel(layout);
  m.setQuery('tape');
  ok(m.visible().length > 1, 'typing tape finds every engine that mentions tape');

  m.toggleChip('tape');
  eq(m.visible().map((b) => b.engine), ['tape'], 'the chip selects exactly one');
  eq(m.store.get().query, '', 'and takes over from the text box rather than fighting it');

  m.toggleChip('tape');
  eq(m.visible().length, layout.banks.length, 'clicking it again clears the selection');
});

test('typing releases a pinned chip', () => {
  const m = new ReferenceModel(layout);
  m.toggleChip('radio');
  m.setQuery('bard');
  eq(m.visible().map((b) => b.engine), ['bard'], 'the search wins, not the stale selection');
  eq(m.store.get().pinned, null);
});

test('the reference search matches folders and formats, not just engine names', () => {
  const m = new ReferenceModel(layout);
  m.setQuery('headerless');
  eq(m.visible().map((b) => b.engine), ['radio'], '"I have raw files, who wants those?"');
  m.setQuery('sk/b');
  eq(m.visible().map((b) => b.engine), ['granular']);
});

test('the reference status counts what is shown against the whole', () => {
  const m = new ReferenceModel(layout);
  eq(m.status(), `${layout.banks.length} folder layouts`);
  m.toggleChip('radio');
  eq(m.status(), `1 of ${layout.banks.length} shown`);
});

// --- terminal -------------------------------------------------------------------------------------

const DESCRIBE = ['descr engine=tape version=0.6.1 masked=1', 'param size deck 0..1', 'end'];

/** Answers describe, usb and cpu; anything else is `ok`. */
const scriptedDevice = () => fakeTransport((cmd) => {
  if (cmd === 'describe') return DESCRIBE;
  if (cmd === 'query usb') return 'ok boot=1 phy=1';
  if (cmd.startsWith('query cpu')) return 'ok 42.5';
  return 'ok';
});

const terminalModel = (transport: Transport, clock = fakeClock()) => ({
  model: new TerminalModel({ serial: fakeSerial(transport), clock, confirm: () => true }),
  clock,
});

test('connecting parses the descriptor and reads the usb snapshot', async () => {
  const { model } = terminalModel(scriptedDevice());
  ok(await model.connect());
  const s = model.store.get();
  eq(s.connected, true);
  eq(s.port, 'USB 0x0483:0x5740');
  eq(s.descriptor!.engine, 'tape');
  eq(s.usb.map((r) => r.key), ['boot', 'phy']);
});

test('an empty port chooser explains itself and offers the unfiltered list', async () => {
  // WebSerial reports a cancelled chooser and an EMPTY one as the same NotFoundError, so the app
  // cannot tell them apart - and returning silently is the wrong choice for the empty case.
  const serial: SerialPorts = {
    supported: () => true,
    request: async () => {
      throw Object.assign(new Error('No port selected'), { name: 'NotFoundError' });
    },
  };
  const model = new TerminalModel({ serial, clock: fakeClock(), confirm: () => true });
  eq(await model.connect(), false);
  const s = model.store.get();
  eq(s.offerAllPorts, true, 'the escape hatch is revealed');
  ok(s.lines.at(-1)!.text.includes('vendor id'), 'and the reason is stated');

  await model.connect({ filtered: false });
  eq(model.store.get().lines.at(-1)!.text, 'no port chosen.',
    'cancelling the FULL list must not repeat the "nothing matched" advice');
});

test('a device that goes away tears the session down', async () => {
  // Without this the tab keeps claiming a connection, the command line stays live, and the CPU poll
  // keeps firing commands that time out three seconds at a time.
  const transport = scriptedDevice();
  const { model, clock } = terminalModel(transport);
  await model.connect();
  eq(clock.running, true, 'polling started');

  transport.vanish('device disconnected');
  await Promise.resolve();
  await Promise.resolve();

  const s = model.store.get();
  eq(s.connected, false);
  eq(clock.running, false, 'the poll stopped with it');
  ok(s.lines.some((l) => l.text.includes('device disconnected')));
});

test('the CPU poll gives up when the build cannot answer', async () => {
  // A build without TERMINAL=1 answers `err unknown-verb`; hammering it every 500 ms achieves nothing.
  const transport = fakeTransport((cmd) => (cmd.startsWith('query cpu') ? 'err unknown-verb' : 'ok'));
  const { model, clock } = terminalModel(transport);
  await model.connect();
  await model.pollCpu();
  eq(model.store.get().cpuAvailable, false);
  eq(clock.running, false, 'and it stops polling rather than repeating a known failure');
});

test('cpu readings accumulate a history for the plot', async () => {
  const { model } = terminalModel(scriptedDevice());
  await model.connect();
  await model.pollCpu();
  await model.pollCpu();
  const s = model.store.get();
  eq(s.cpu!.avg, 42.5);
  ok(s.cpuHistory.length >= 2, 'the history is what answers "has max stopped climbing"');
});

test('a destructive verb is refused when the confirmation is declined', async () => {
  const transport = scriptedDevice();
  const model = new TerminalModel({
    serial: fakeSerial(transport), clock: fakeClock(), confirm: () => false,
  });
  await model.connect();
  eq(await model.send('pad clear A'), null);
  // By command, not by count: connect() starts the CPU poll, which keeps writing after it resolves,
  // so a length comparison here races the poll rather than testing the refusal.
  ok(!transport.sent.includes('pad clear A'), 'nothing reached the device');
  ok(model.store.get().lines.at(-1)!.text.includes('cancelled'));
});

test('reset cpu is not treated as destructive', async () => {
  const transport = scriptedDevice();
  const model = new TerminalModel({
    serial: fakeSerial(transport), clock: fakeClock(), confirm: () => false,
  });
  await model.connect();
  await model.resetCpu();
  ok(transport.sent.includes('reset cpu'), 'it only clears meter extremes, so it needs no prompt');
  eq(model.store.get().cpuHistory, []);
});

test('the console is bounded so a chatty device cannot grow the page forever', () => {
  const model = new TerminalModel({
    serial: fakeSerial(scriptedDevice()), clock: fakeClock(), confirm: () => true,
  });
  for (let i = 0; i < 600; i++) model.write(`line ${i}`, 'log');
  const lines = model.store.get().lines;
  eq(lines.length, 500);
  eq(lines.at(-1)!.text, 'line 599', 'and it is the OLDEST that is dropped');
});
