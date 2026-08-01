// misc.test.js - the layout wrapper, the target-name templates, and the ZIP writer.

import { suite, test, ok, eq, throws, readWeb } from './harness.js';
import { makeLayout, formatTarget } from '../js/layout.js';
import { makeZip, crc32 } from '../js/zip.js';
import { encodeForBank, targetSummary } from '../js/convert.js';

suite('misc');

const layout = makeLayout(readWeb('card_layout.json'));

// --- layout -----------------------------------------------------------------------------------

test('rejects a card_layout.json from a newer schema instead of half-working', () => {
  throws(() => makeLayout({ schema: 99, banks: [] }), 'unsupported schema');
});

test('the longest directory match wins', () => {
  // SK/ belongs to the platform entry, but SK/B is a granular tape folder. Otherwise granular audio
  // would be validated as platform config.
  eq(layout.bankForPath('SK').engine, 'platform');
  eq(layout.bankForPath('SK/B').engine, 'granular');
  eq(layout.bankForPath('radio/3').engine, 'radio');
  eq(layout.bankForPath('nonsense'), null);
});

for (const [name, want] of [
  ['01.raw', true],
  ['BOOK01.WAV', true],
  ['track.wav', true],
  ['aaaaaaaaa.wav', false], // 13 chars - one over the limit
  ['aaaaaaaa.wav', true], //  12 chars - exactly at the limit
  ['._01.raw', false], // AppleDouble
  ['.DS_Store', false],
  ['song.mp3', false],
  ['noextension', false],
]) {
  test(`scanNameOk(${JSON.stringify(name)}) is ${want}`, () => eq(layout.scanNameOk(name), want));
}

test('every bank cites the firmware it mirrors', () => {
  for (const b of layout.banks) {
    ok(b.dirs.length, `${b.engine} declares no directories`);
    ok(b.source, `${b.engine} has no firmware source citation`);
  }
});

test('the raw format description does not claim a WAV format tag', () => {
  // A headerless file has no AudioFormat field; saying "(WAV AudioFormat 1)" for one would be
  // actively misleading, since the absence of any self-description is that format's whole hazard.
  ok(!layout.bank('radio').fmt.describe.includes('AudioFormat'));
  ok(layout.bank('tape').fmt.describe.includes('AudioFormat'));
});

// --- target templates -------------------------------------------------------------------------

test('expands each bank template the way card_layout.format_target does', () => {
  const o = { deck: 'b', bank: 3, tape: 'R' };
  eq(formatTarget(layout.bank('granular').target, 1, o), 'SK/R/1.WAV');
  eq(formatTarget(layout.bank('tape').target, 2, o), 'tapes/tape_b_2.wav');
  eq(formatTarget(layout.bank('shuttle').target, 2, o), 'shuttle/tape_b_2.wav');
  eq(formatTarget(layout.bank('radio').target, 1, o), 'radio/3/01.raw');
  eq(formatTarget(layout.bank('bard').target, 1, o), 'bard/3/BOOK01.WAV');
  eq(formatTarget(layout.bank('pstretch').target, 12, o), 'pstretch/CLIP12.WAV');
});

test('only the audio banks advertise a target', () => {
  eq(layout.audioBanks().map((b) => b.engine).sort(),
    ['bard', 'granular', 'pstretch', 'radio', 'shuttle', 'softcut', 'tape']);
  eq(layout.bank('chuck').target, '');
  eq(layout.bank('platform').target, '');
});

// --- convert ----------------------------------------------------------------------------------

test('a scanned bank loops a short clip up over the 32 KB floor', () => {
  const { bytes, notes } = encodeForBank(layout, layout.bank('pstretch'), new Float32Array(100), 48000);
  ok(bytes.length - 44 >= layout.scan.min_bytes, 'the body clears the floor');
  ok(notes.some((n) => n.includes('looped up')), 'and says so');
});

test('a RAM-loaded bank trims a long clip and says so', () => {
  const bank = layout.bank('shuttle');
  const tooLong = new Float32Array(Math.floor(bank.max_seconds * 48000) + 48000);
  const { bytes, notes } = encodeForBank(layout, bank, tooLong, 48000);
  eq((bytes.length - 44) / 4, Math.floor(bank.max_seconds * 48000));
  ok(notes.some((n) => n.includes('trimmed')));
});

test('the radio bank is written headerless', () => {
  const { bytes } = encodeForBank(layout, layout.bank('radio'), new Float32Array(100), 48000);
  eq(bytes.length, layout.scan.min_bytes, 'no 44-byte header, padded to the floor');
});

test('a streaming bank is left at its natural length', () => {
  const { bytes, notes } = encodeForBank(layout, layout.bank('tape'), new Float32Array(1000), 48000);
  eq(bytes.length, 44 + 4000);
  eq(notes, []);
});

test('the target summary names the container, depth, channels and rate', () => {
  eq(targetSummary(layout.bank('tape'), 48000), '.wav, 32-bit float, mono, 48000 Hz');
  eq(targetSummary(layout.bank('radio'), 48000), 'headerless .raw, 16-bit PCM, mono, 48000 Hz');
  eq(targetSummary(layout.bank('granular'), 48000), '.wav, 32-bit float, stereo, 48000 Hz');
  eq(targetSummary(layout.bank('bard'), 24000), '.wav, 16-bit PCM, mono, 24000 Hz', 'any-rate bank');
});

// --- zip --------------------------------------------------------------------------------------

test('crc32 matches the reference value for "123456789"', () => {
  eq(crc32(new TextEncoder().encode('123456789')), 0xcbf43926);
});

test('produces an archive with the entries and folders it was given', async () => {
  const files = [{ path: 'SK/config.txt', bytes: new TextEncoder().encode('pre_load\n1\n') }];
  const blob = await makeZip(files, ['SK', 'radio/0']);
  const bytes = new Uint8Array(await blob.arrayBuffer());
  const view = new DataView(bytes.buffer);
  eq(view.getUint32(0, true), 0x04034b50, 'starts with a local file header');

  // Walk back from the end-of-central-directory record, which is where any unzip starts.
  const eocd = bytes.length - 22;
  eq(view.getUint32(eocd, true), 0x06054b50);
  eq(view.getUint16(eocd + 8, true), 3, 'two folders and one file');

  const text = new TextDecoder().decode(bytes);
  ok(text.includes('SK/'), 'records the SK folder');
  ok(text.includes('radio/0/'), 'records an empty radio bank folder');
  ok(text.includes('SK/config.txt'));
});

test('entries carry the DOS epoch so the archive is reproducible', async () => {
  // A card zip whose bytes change on every build cannot be checksummed meaningfully; sk_card.py dist
  // fixes its timestamps for the same reason.
  const a = new Uint8Array(await (await makeZip([{ path: 'a', bytes: new Uint8Array([1]) }])).arrayBuffer());
  const b = new Uint8Array(await (await makeZip([{ path: 'a', bytes: new Uint8Array([1]) }])).arrayBuffer());
  eq(a, b);
  const v = new DataView(a.buffer);
  eq(v.getUint16(10, true), 0, 'time field');
  eq(v.getUint16(12, true), (1 << 5) | 1, '1980-01-01');
});

test('stores rather than deflates when compression would not help', async () => {
  const blob = await makeZip([{ path: 'x', bytes: new Uint8Array([7]) }]);
  const v = new DataView(await blob.arrayBuffer());
  eq(v.getUint16(8, true), 0, 'method 0 = stored');
});
