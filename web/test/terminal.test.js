// terminal.test.js - framing, the descriptor, and the command client, all without hardware.
//
// The client is driven by a scripted transport rather than a serial port, mirroring how
// tools/conftest.py skips cleanly when no device is attached. That matters more here than it looks:
// the bugs in a serial client are chunk-boundary bugs and reply-offset bugs, neither of which a
// hardware session finds reliably and both of which a fake device reproduces on demand.

import { suite, test, ok, eq, throws, rejects } from './harness.js';
import { LineAssembler, isLog, parseReply, isDestructive, CommandError, Timeout } from '../js/terminal/framing.js';
import { parseDescribe, vocabulary, parseUsbDiag } from '../js/terminal/descriptor.js';
import { Device } from '../js/terminal/device.js';
import { SerialTransport, requestPort, DAISY_VID } from '../js/terminal/serial.js';

suite('terminal');

// --- the WebSerial transport --------------------------------------------------------------------
//
// SerialTransport is the one part of the client that touches a browser API, so it is the part the
// scripted-transport tests below cannot reach. A ReadableStream is a ReadableStream in node, though,
// which is enough to drive the two lifecycle paths that matter and that no host test covered before:
// a port that ends by itself, and one closed deliberately.

/** A serial port whose read stream can be ended or failed on demand. */
function fakePort({ chunks = [], failWith = null, keepOpen = false } = {}) {
  return {
    readable: new ReadableStream({
      start(c) {
        for (const s of chunks) c.enqueue(new TextEncoder().encode(s));
        if (failWith) c.error(new Error(failWith));
        else if (!keepOpen) c.close();
      },
    }),
    writable: new WritableStream({ write() {} }),
    async open() {},
    async close() {},
    async setSignals() {},
    getInfo: () => ({ usbVendorId: DAISY_VID, usbProductId: 0x5740 }),
  };
}

/**
 * Wait for `onClose`, or give up.
 *
 * The bound is the point: a transport that never reports the loss is exactly the bug these tests
 * exist to catch, and a bare `new Promise(r => t.onClose(r))` does not fail on it - it hangs the whole
 * suite forever, which reads as a broken runner rather than a broken transport.
 */
const closeReason = (t, ms = 1000) => Promise.race([
  new Promise((resolve) => t.onClose(resolve)),
  new Promise((_, reject) => setTimeout(() => reject(new Error('onClose never fired')), ms)),
]);

const withNavigator = async (serial, fn) => {
  const saved = globalThis.navigator;
  globalThis.navigator = { serial };
  try {
    return await fn();
  } finally {
    globalThis.navigator = saved;
  }
};

test('the chooser is filtered to the Daisy by default and unfiltered on request', async () => {
  // An empty chooser and a cancelled one are the same NotFoundError, so a filter that matches nothing
  // is a dead end. The unfiltered retry is the way out for a board that reports another vendor id.
  const seen = [];
  const serial = {
    requestPort: async (opts) => {
      seen.push(opts);
      return fakePort({ keepOpen: true });
    },
  };
  await withNavigator(serial, async () => {
    await (await requestPort()).close();
    await (await requestPort({ filtered: false })).close();
  });
  eq(seen[0], { filters: [{ usbVendorId: DAISY_VID }] });
  eq(seen[1], {}, 'no filters at all, not an empty filter list - those differ to the chooser');
});

test('a port that ends by itself reports the loss', async () => {
  const t = new SerialTransport(fakePort({ chunks: ['ok 1\r\n'] }));
  const lines = [];
  t.onLine((l) => lines.push(l));
  const why = await closeReason(t);
  eq(lines, ['ok 1'], 'and delivers what it had already received first');
  ok(why, 'with a reason to show the user');
});

test('a read failure reports the loss too, with the error', async () => {
  const t = new SerialTransport(fakePort({ failWith: 'device disconnected' }));
  t.onLine(() => {});
  eq(await closeReason(t), 'device disconnected');
});

test('a deliberate close is not reported as a loss', async () => {
  // Otherwise the teardown re-enters itself: close() fires onClose, which the view answers by tearing
  // down, which calls close().
  const t = new SerialTransport(fakePort({ keepOpen: true }));
  let fired = false;
  t.onClose(() => { fired = true; });
  await t.close();
  await new Promise((r) => setTimeout(r, 0));
  ok(!fired);
});

test('the transport names the port it opened', () => {
  eq(new SerialTransport(fakePort({ keepOpen: true })).info(), 'USB 0x0483:0x5740');
});

// --- framing ----------------------------------------------------------------------------------

test('assembles a line delivered in one chunk', () => {
  eq(new LineAssembler().push('ok 0.5\r\n'), ['ok 0.5']);
});

test('assembles a line split across chunks', () => {
  const a = new LineAssembler();
  eq(a.push('ok 0.'), []);
  eq(a.push('5\r\n'), ['ok 0.5']);
});

test('handles a CRLF straddling a chunk boundary', () => {
  const a = new LineAssembler();
  eq(a.push('ok\r'), []);
  eq(a.push('\nerr bad-arg\n'), ['ok', 'err bad-arg']);
});

test('yields several lines from one chunk', () => {
  eq(new LineAssembler().push('a\nb\nc\n'), ['a', 'b', 'c']);
});

test('holds an unterminated tail rather than emitting it', () => {
  const a = new LineAssembler();
  eq(a.push('partial'), []);
  eq(a.pending, 'partial');
});

test('log lines are the ones starting with a bracket', () => {
  ok(isLog('[usb] host connected'));
  ok(!isLog('ok 1'));
  ok(!isLog('err unknown-verb'));
});

test('classifies replies', () => {
  eq(parseReply('ok'), { kind: 'ok', value: '' });
  eq(parseReply('ok 0.25'), { kind: 'ok', value: '0.25' });
  eq(parseReply('ok empty=1'), { kind: 'ok', value: 'empty=1' });
  eq(throws(() => parseReply('err bad-deck'), CommandError).reason, 'bad-deck');
  throws(() => parseReply('nonsense'), CommandError);
});

test('flags the verbs that must not fire from a single click', () => {
  // docs/dev/terminal-target-b.md: sweeping a control surface can clear a recorded buffer or write
  // the card, so the UI confirms these first.
  ok(isDestructive('pad clear A'));
  ok(isDestructive('seq clear B'));
  ok(isDestructive('preset save 0'));
  ok(isDestructive('reset'));
  ok(isDestructive('reset A'));
  ok(!isDestructive('reset cpu'), 'reset cpu only clears meter extremes');
  ok(!isDestructive('query empty A'));
  ok(!isDestructive('set param size A 0.5'));
});

// --- descriptor -------------------------------------------------------------------------------

const BLOCK = [
  'descr engine=reso version=0.6.1 masked=1',
  'param size deck 0..1',
  'param tempo global 20..300',
  'config mode deck 0:slice 1:reel 2:drift',
  'config route global 0:L 1:C 2:R',
  'query empty deck bool',
  'query cpu global float',
  'query layout deck enum 0:single 1:slice 2:chord 3:none',
  'caps 0x0000000d',
];

test('parses a describe block', () => {
  const d = parseDescribe(BLOCK);
  eq(d.engine, 'reso');
  eq(d.version, '0.6.1');
  ok(d.masked);
  eq(d.params.get('size'), { name: 'size', scope: 'deck', lo: 0, hi: 1 });
  eq(d.params.get('tempo'), { name: 'tempo', scope: 'global', lo: 20, hi: 300 });
  eq(d.configs.get('mode').values.get(2), 'drift');
  eq(d.queries.get('empty').kind, 'bool');
  eq(d.queries.get('layout').values.get(3), 'none');
  eq(d.caps, 0x0d);
});

test('a config scope token is not mistaken for a value', () => {
  // The dispatch doc shows a scope, the tools sketch omits it; only int:label tokens are values.
  eq(parseDescribe(['config mode deck 0:a 1:b']).configs.get('mode').values, new Map([[0, 'a'], [1, 'b']]));
  eq(parseDescribe(['config mode 0:a 1:b']).configs.get('mode').values, new Map([[0, 'a'], [1, 'b']]));
});

test('older firmware that omits kind still parses', () => {
  const d = parseDescribe(['query empty deck', 'query mix']);
  eq(d.queries.get('empty'), { name: 'empty', scope: 'deck', kind: 'text', values: new Map() });
  eq(d.queries.get('mix').scope, 'global');
});

test('an unknown tag is ignored rather than breaking the page', () => {
  const d = parseDescribe(['descr engine=x version=1 masked=0', 'newthing whatever 1 2 3', 'param a deck 0..1']);
  eq(d.engine, 'x');
  eq(d.params.size, 1);
});

test('masked=0 is reported, so the UI can say the surface is over-reported', () => {
  ok(!parseDescribe(['descr engine=x version=1 masked=0']).masked);
});

test('the completion vocabulary includes every advertised name', () => {
  const words = vocabulary(parseDescribe(BLOCK));
  for (const w of ['size', 'tempo', 'mode', 'route', 'empty', 'cpu', 'layout', 'describe', 'query']) {
    ok(words.includes(w), `missing ${w}`);
  }
});

test('renders the usb bring-up snapshot as ordered pairs, not a flag soup', () => {
  const pairs = parseUsbDiag('boot=1 region=2 clkcfg=1 hsi48=1 usbsel=0 usb33den=1 usb33rdy=1 phy=1 pullup=1');
  eq(pairs.length, 9);
  eq(pairs[0], { key: 'boot', value: '1' });
  eq(pairs[8], { key: 'pullup', value: '1' });
});

// --- the client -------------------------------------------------------------------------------

/** A scripted device: answers each written command from a queue, or from a handler. */
function fakeDevice(script) {
  let onLine = () => {};
  const sent = [];
  return {
    sent,
    transport: {
      async write(text) {
        sent.push(text.replace(/\r?\n$/, ''));
        const reply = typeof script === 'function' ? script(sent[sent.length - 1]) : script.shift();
        for (const line of [].concat(reply ?? [])) onLine(line);
      },
      onLine(cb) {
        onLine = cb;
      },
      close: async () => {},
      emit: (line) => onLine(line),
    },
    get emit() {
      return onLine;
    },
  };
}

test('sends a command and returns its payload', async () => {
  const f = fakeDevice(['ok 0.25']);
  const d = new Device(f.transport);
  eq(await d.getParam('size', 'A'), 0.25);
  eq(f.sent, ['get param size A']);
});

test('a bare ok is an empty payload, not an error', async () => {
  const d = new Device(fakeDevice(['ok']).transport);
  eq(await d.cmd('mode test'), '');
});

test('skips interleaved log lines to find the reply', async () => {
  const logs = [];
  const f = fakeDevice([['[usb] host connected', '[sd] scan done', 'ok 1']]);
  const d = new Device(f.transport, { logSink: (l) => logs.push(l) });
  eq(await d.cmd('query empty A'), '1');
  eq(logs, ['[usb] host connected', '[sd] scan done'], 'logs are captured, not discarded silently');
});

test('an err reply raises with the bare reason token', async () => {
  const d = new Device(fakeDevice(['err unknown-param']).transport);
  eq((await rejects(d.cmd('set param nope A 1'), CommandError)).reason, 'unknown-param');
});

test('formats floats as the Python client does', async () => {
  const f = fakeDevice(() => 'ok');
  const d = new Device(f.transport);
  await d.setParam('size', 'A', 0.5);
  await d.setParam('size', 'B', 1);
  await d.setParam('tempo', 'A', 123.456789);
  eq(f.sent, ['set param size A 0.5', 'set param size B 1', 'set param tempo A 123.457']);
});

test('reads a describe block up to the end marker', async () => {
  const f = fakeDevice([[...BLOCK, 'end']]);
  const d = new Device(f.transport);
  eq(await d.describeLines(), BLOCK);
});

test('times out rather than hanging when the device says nothing', async () => {
  const d = new Device(fakeDevice([[]]).transport, { timeout: 30 });
  await rejects(d.cmd('query empty A'), Timeout);
});

test('a late reply after a timeout does not offset every later command', async () => {
  // The failure this prevents: one timeout makes every subsequent command read the PREVIOUS command's
  // answer, so the failures surface far from their cause as nonsense parse errors.
  const f = fakeDevice([[], ['ok 0.75']]);
  const d = new Device(f.transport, { timeout: 30 });
  const dropped = [];
  d.logSink = (l) => dropped.push(l);
  await rejects(d.cmd('query empty A'), Timeout);
  f.transport.emit('ok stale-answer'); // the first command finally replies
  eq(await d.cmd('get param size A'), '0.75', 'the second command got its own answer');
  eq(dropped, ['[stale] ok stale-answer']);
});

test('serializes commands so two in flight cannot cross replies', async () => {
  const f = fakeDevice(['ok first', 'ok second']);
  const d = new Device(f.transport);
  const [a, b] = await Promise.all([d.cmd('one'), d.cmd('two')]);
  eq([a, b], ['first', 'second']);
  eq(f.sent, ['one', 'two']);
});

test('cpu readings are fetched one at a time', async () => {
  const f = fakeDevice((line) => ({
    'query cpu': 'ok 42.5',
    'query cpumin': 'ok 11.0',
    'query cpumax': 'ok 77.25',
  }[line]));
  const d = new Device(f.transport);
  eq(await d.cpu(), { avg: 42.5, min: 11, max: 77.25 });
  eq(f.sent, ['query cpu', 'query cpumin', 'query cpumax']);
});

test('a global query is sent without a trailing deck', async () => {
  const f = fakeDevice(() => 'ok 0.5');
  const d = new Device(f.transport);
  await d.query('mix');
  eq(f.sent, ['query mix']);
});
