// build.test.js - the browser must build the same card `sk_card.py init` does.
//
// Checked by SHA-256 per file against build_manifest.json, so this is byte equality, not "looks
// similar". Most of what it proves is that shipping the generated text (READMEs, root README, config)
// as data in card_layout.json actually works end to end - if any of it had been paraphrased into JS
// instead, every hash here would differ.

import { suite, test, ok, eq, readJson, readWeb, sha256hex, layoutData, type Fixture } from './harness.ts';
import { makeLayout } from '../src/core/layout.ts';
import { buildCard, missingFrom } from '../src/core/build.ts';

suite('build');

const layout = makeLayout(layoutData());
const patches = readWeb<Record<string, string>>('patches.json');
const manifest = readJson<Fixture>('build_manifest.json');

test('writes exactly the files sk_card.py init writes', () => {
  const built = buildCard(layout, patches);
  eq(built.files.map((f) => f.path).sort(), Object.keys(manifest.files).sort());
});

test('every file is byte-identical to the Python original', async () => {
  const built = buildCard(layout, patches);
  const mismatched = [];
  for (const f of built.files) {
    const got = await sha256hex(f.bytes);
    if (got !== manifest.files[f.path]) mismatched.push(f.path);
  }
  eq(mismatched, [], 'these files differ from sk_card.py init');
});

test('creates every folder the layout declares, parents first', () => {
  const built = buildCard(layout, patches);
  eq(built.dirs, manifest.dirs);
  // init creates them in order, so a parent must precede its children.
  ok(built.dirs.indexOf('radio') < built.dirs.indexOf('radio/0'));
});

test('the per-folder READMEs state the scan rules only where they apply', () => {
  const built = buildCard(layout, patches);
  const text = (p) => new TextDecoder().decode(built.files.find((f) => f.path === p).bytes);
  const scanned = text('radio/0/README.TXT');
  ok(scanned.includes('12 characters') && scanned.includes('32 KB'), 'a scanned folder states both limits');
  const slots = text('tapes/README.TXT');
  ok(!slots.includes('INVISIBLE'), 'a slot folder must not claim rules that do not apply to it');
  ok(slots.includes('tape_a_1.wav'), 'a slot folder names its slots');
});

test('carries the chuck and csound example patches', () => {
  const built = buildCard(layout, patches);
  const paths = new Set(built.files.map((f) => f.path));
  ok(paths.has('chuck/0.ck'), 'chuck/0.ck');
  ok(paths.has('csound/0.csd'), 'csound/0.csd');
});

test('only bundles patches that are actually slot filenames', () => {
  // examples/ also holds README.md and midi_in.ck, which the engine never opens. Shipping them would
  // produce "not a slot the engine loads" warnings on a card the tool itself built.
  for (const path of Object.keys(patches)) {
    const [engine, name] = path.split('/');
    ok(layout.bank(engine).slots.includes(name), `${path} is not a slot of ${engine}`);
  }
});

test('topping up an existing card leaves files that are already there alone', () => {
  const built = buildCard(layout, patches);
  const existing = ['SK/config.txt', 'tapes/README.TXT'];
  const missing = missingFrom(built, existing);
  const paths = missing.files.map((f) => f.path);
  ok(!paths.includes('SK/config.txt'), 'must not overwrite a config the user tuned');
  ok(!paths.includes('tapes/README.TXT'), 'must not overwrite an edited README');
  eq(missing.files.length, built.files.length - 2);
});

test('the top-up comparison is case-insensitive, as FAT is', () => {
  const built = buildCard(layout, patches);
  const missing = missingFrom(built, ['sk/config.TXT'.toUpperCase()]);
  ok(!missing.files.some((f) => f.path === 'SK/config.txt'));
});
