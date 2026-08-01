// wav.test.js - the JS encoders must agree with the Python ones BYTE FOR BYTE.
//
// This is the test the whole cross-language arrangement rests on. `web/js/wav.js` is one of only two
// things the web app reimplements rather than reading out of card_layout.json, and it is the one whose
// output lands on a card the firmware reads. The fixtures come from card_audio.py via
// scripts/web_export.py, so a change to either side fails here rather than on hardware.

import {
  suite, test, ok, eq, throws, readFixture, readJson,
} from './harness.js';
import {
  writeWav, writeRaw, parseWav, readSamples, padToBytes,
  WavError, WavNeedMore, F32, INT16,
} from '../js/wav.js';

suite('wav');

const manifest = readJson('manifest.json');

for (const spec of manifest.formats) {
  test(`writes ${spec.name} byte-identically to card_audio.py`, () => {
    const expected = readFixture(spec.name);
    const actual = spec.kind === 'raw'
      ? writeRaw(spec.samples)
      : writeWav(spec.samples, spec.rate, spec.channels, spec.encoding);
    eq(actual.length, expected.length, `${spec.name}: length`);
    eq(actual, expected, `${spec.name}: bytes`);
  });
}

test('int16 conversion truncates toward zero, as Python int() does', () => {
  // The failure this pins: Math.round instead of Math.trunc differs on roughly half of all samples.
  // 0.9999 * 32767 = 32763.7233 -> 32763 truncated, 32764 rounded.
  const bytes = writeWav([0.9999, -0.9999], 48000, 1, INT16);
  const view = new DataView(bytes.buffer, 44);
  eq(view.getInt16(0, true), 32763);
  eq(view.getInt16(2, true), -32763);
});

test('int16 clips rather than wrapping', () => {
  const bytes = writeWav([1.5, -1.5, 2000], 48000, 1, INT16);
  const view = new DataView(bytes.buffer, 44);
  eq([view.getInt16(0, true), view.getInt16(2, true), view.getInt16(4, true)], [32767, -32767, 32767]);
});

test('the header is the 44 bytes the device itself writes', () => {
  // src/memory/wav.h static_asserts sizeof(header) == 44 with a fixed BlocSize of 16. Matching it
  // keeps cards readable by firmware predating the chunk-walk fix.
  const bytes = writeWav([0, 0, 0, 0], 48000, 1, F32);
  eq(parseWav(bytes).dataOffset, 44);
  eq(bytes.length, 44 + 16);
});

test('byte rate and block align are consistent with the declared format', () => {
  const bytes = writeWav([0, 0, 0, 0], 44100, 2, INT16);
  const v = new DataView(bytes.buffer);
  eq(v.getUint16(32, true), 4, 'block align = channels * bits/8');
  eq(v.getUint32(28, true), 44100 * 4, 'byte rate = rate * block align');
});

// --- parsing --------------------------------------------------------------------------------

for (const spec of manifest.parses) {
  test(`parses ${spec.name} the way the firmware does`, () => {
    const info = parseWav(readFixture(spec.name));
    eq(info.encoding, spec.encoding);
    eq(info.channels, spec.channels);
    eq(info.rate, spec.rate);
    eq(info.frames, spec.frames);
    eq(info.dataOffset, spec.data_offset);
    eq(info.dataOffset > 44, spec.past44, 'the body is past the canonical offset 44');
  });
}

for (const spec of manifest.rejects) {
  test(`rejects ${spec.name} (${spec.why}), as the firmware does`, () => {
    throws(() => parseWav(readFixture(spec.name)), WavError);
  });
}

test('round-trips float samples', () => {
  const bytes = writeWav([0, 0.5, -0.5, 1], 48000, 1, F32);
  const { samples, info } = readSamples(bytes);
  eq(info.encoding, F32);
  eq(info.rate, 48000);
  eq([...samples], [0, 0.5, -0.5, 1]);
});

test('round-trips int16 samples within one LSB', () => {
  const { samples, info } = readSamples(writeWav([0, 0.5, -0.5], 24000, 1, INT16));
  eq(info.rate, 24000);
  ok(Math.abs(samples[1] - 0.5) < 1e-4, `0.5 came back as ${samples[1]}`);
  ok(Math.abs(samples[2] + 0.5) < 1e-4, `-0.5 came back as ${samples[2]}`);
});

test('a truncated data chunk reports the bytes that are actually there', () => {
  // The firmware clamps to the file length rather than trusting the header; a checker that trusted the
  // header would report a plausible duration for a file that is half missing.
  const full = writeWav(new Array(64).fill(0), 48000, 1, F32);
  const cut = full.subarray(0, 44 + 40);
  eq(parseWav(cut, cut.length).dataSize, 40);
});

test('a prefix too short for the header asks for more rather than reporting a broken file', () => {
  // The distinction matters: verify reads a 64 KB prefix off the card, and "I need more bytes" must
  // not be shown to the user as "this file is corrupt".
  const full = writeWav([0, 0, 0, 0], 48000, 1, F32);
  const e = throws(() => parseWav(full.subarray(0, 20), full.length), WavNeedMore);
  ok(e.needed > 20, 'reports how many bytes it needs');
});

test('a header split across a prefix boundary parses once the whole file is available', () => {
  const full = readFixture('extensible.wav');
  throws(() => parseWav(full.subarray(0, 30), full.length), WavNeedMore);
  eq(parseWav(full).encoding, INT16);
});

// --- padding --------------------------------------------------------------------------------

test('padToBytes loops a short clip up over the scan floor', () => {
  const short = new Float64Array(100).fill(0.25);
  const padded = padToBytes(short, 32 * 1024, 2);
  eq(padded.length, Math.ceil((32 * 1024) / 2));
  eq(padded[0], 0.25);
  eq(padded[padded.length - 1], 0.25, 'the loop, not silence, fills the tail');
});

test('padToBytes leaves a file that already clears the floor alone', () => {
  const long = new Float64Array(40000).fill(0.1);
  eq(padToBytes(long, 32 * 1024, 2).length, 40000);
});
