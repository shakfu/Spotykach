// verify.test.js - the two checkers must give a user the same answer.
//
// The important test here is not a list of hand-written expectations: it is `verify_cases.json`, a
// deliberately-broken card built by scripts/web_export.py and run through the REAL scripts/sk_card.py
// `verify_card`, whose findings are shipped as the expected result. So this asserts against the
// Python's actual behaviour rather than against a second reading of the same spec - which is the only
// version of this test that would have caught the drift docs/dev/web-frontend.md warns about.
//
// The hand-written cases below cover the two branches the fixture card cannot reach on a
// case-insensitive filesystem (a slot name differing only in case) or without 33 large files (the
// per-folder index cap).

import { suite, test, ok, eq, readJson, readWeb, entriesFromCase, layoutData, type Fixture } from './harness.ts';
import { makeLayout } from '../src/core/layout.ts';
import { verifyCard, summarize } from '../src/core/verify.ts';
import { writeWav, INT16, F32 } from '../src/core/wav.ts';

suite('verify');

const layout = makeLayout(layoutData());

const key = (f) => `${f.level}|${f.path}|${f.problem}`;
const sortKeys = (fs) => fs.map(key).sort();

test('reaches the same verdicts as sk_card.py on a deliberately-broken card', async () => {
  const c = readJson<Fixture>('verify_cases.json');
  const found = await verifyCard(layout, { files: entriesFromCase(c), dirs: c.dirs });
  eq(sortKeys(found), sortKeys(c.findings), 'findings differ from the Python checker');
});

test('reproduces the fix text too, not just the verdict', async () => {
  // The fix line is what the user acts on, so it is worth pinning: a right diagnosis with a wrong
  // remedy is its own failure. Only the CLI-invocation wording differs, because the web app can offer
  // its own Convert tab; everything else must match.
  const c = readJson<Fixture>('verify_cases.json');
  const found = await verifyCard(layout, { files: entriesFromCase(c), dirs: c.dirs });
  const mine = new Map(found.map((f) => [key(f), f.fix]));
  for (const expected of c.findings) {
    const fix = mine.get(key(expected));
    ok(fix !== undefined, `missing finding: ${key(expected)}`);
    const strip = (s) => s.replace(/Convert it on the Convert tab, or: /, 'Convert it: ')
      .replace(/Fix it on the Convert tab, or: /, 'Fix with: ')
      .replace(/Build a fresh one on the Build tab, or: /, 'Build a fresh one with: ');
    eq(strip(fix), expected.fix, `fix text for ${key(expected)}`);
  }
});

test('a card sk_card.py init just built has no errors', async () => {
  // A base card that its own verifier rejects would be worse than shipping nothing, and that has to
  // hold in the browser too - this is the screen a new user sees first.
  const c = readJson<Fixture>('clean_card.json');
  const found = await verifyCard(layout, { files: entriesFromCase(c), dirs: c.dirs });
  const { errors } = summarize(found);
  eq(errors.map((f) => `${f.path}: ${f.problem}`), [], 'a freshly built card must be clean');
  eq(sortKeys(found), sortKeys(c.findings), 'and must match the Python exactly, warnings included');
});

// --- branches the fixture card cannot reach ---------------------------------------------------

const entry = (path, bytes) => ({
  path,
  size: bytes.length,
  read: async (max) => (max != null && max < bytes.length ? bytes.subarray(0, max) : bytes),
});

const filler = (n) => new Uint8Array(n);

test('flags a slot filename that differs only in case', async () => {
  // FAT is case-insensitive so it generally still opens - a warning, not an error. Unreachable in a
  // filesystem fixture on macOS, where the two names are the same file.
  const good = writeWav(new Array(1024).fill(0), 48000, 2, F32);
  const found = await verifyCard(layout, {
    files: [entry('SK/B/1.wav', good)],
    dirs: ['SK', 'SK/B'],
  });
  const warn = found.find((f) => f.path === 'SK/B/1.wav');
  ok(warn && warn.level === 'warn' && warn.problem.includes('documented as 1.WAV'),
    `expected a case warning, got ${JSON.stringify(found)}`);
});

test('does not warn about case for a slot that is documented lowercase', async () => {
  const good = writeWav(new Array(1024).fill(0), 48000, 1, F32);
  const found = await verifyCard(layout, {
    files: [entry('tapes/tape_a_1.wav', good)],
    dirs: ['tapes'],
  });
  eq(found.filter((f) => f.path === 'tapes/tape_a_1.wav'), []);
});

test('warns when a scanned folder holds more files than the engine indexes', async () => {
  const bank = layout.bank('pstretch');
  const good = writeWav(new Array(20000).fill(0), 48000, 1, INT16);
  const files = [];
  for (let i = 0; i < bank.max_files + 2; i++) {
    files.push(entry(`pstretch/CLIP${String(i).padStart(2, '0')}.WAV`, good));
  }
  const found = await verifyCard(layout, { files, dirs: ['pstretch'] });
  const warn = found.find((f) => f.path === 'pstretch');
  ok(warn && warn.problem.includes(`only the first ${bank.max_files}`),
    `expected an index-cap warning, got ${JSON.stringify(found.map((f) => f.problem))}`);
});

test('reports a folder that is not a card at all', async () => {
  const found = await verifyCard(layout, { files: [entry('holiday.jpg', filler(10))], dirs: [] });
  eq(found.length, 1);
  eq(found[0].level, 'error');
  ok(found[0].problem.includes('no recognised engine folders'));
});

test('walks past filesystem bookkeeping directories', async () => {
  const found = await verifyCard(layout, {
    files: [entry('tapes/README.TXT', filler(10)),
      entry('System Volume Information/IndexerVolumeGuid', filler(76))],
    dirs: ['tapes'],
  });
  eq(found.filter((f) => f.path.includes('System Volume')), []);
});

test('the rename suggestion it offers is itself a legal name', async () => {
  // A hint that would fail the same check is worse than no hint. Mirrors the assertion in
  // scripts/test_sk_card.py.
  const c = readJson<Fixture>('verify_cases.json');
  const found = await verifyCard(layout, { files: entriesFromCase(c), dirs: c.dirs });
  const tooLong = found.find((f) => f.problem.includes('characters; the scan skips'));
  ok(tooLong, 'expected a name-too-long finding in the fixture card');
  const suggestion = tooLong.fix.split('(e.g. ')[1].split(')')[0];
  ok(layout.scanNameOk(suggestion), `suggested ${suggestion}, which the scan would still skip`);
});
