// ui.test.js - the views must mount.
//
// Every view builds its UI imperatively in one mount call, so a typo in an el() property name, a
// missing import or an exception mid-construction produces a blank tab - and a blank tab is exactly
// the failure a user cannot report usefully. These tests mount all four against a minimal DOM shim and
// assert each one built the controls it promises, plus the few behaviours that are easy to regress
// silently: the graceful degradation when a browser API is missing, and the confirmation on
// destructive verbs.

import { readFileSync } from 'node:fs';

import { suite, test, ok, eq, readWeb } from './harness.js';
import { installDom, tags, textOf } from './dom_shim.js';
import { makeLayout } from '../js/layout.js';

suite('ui');

const layout = makeLayout(readWeb('card_layout.json'));
const patches = readWeb('patches.json');

/**
 * Mount a view in a fresh shimmed document, then restore the globals. The shim defaults to the least
 * capable browser (no File System Access, no WebSerial), so a view that forgets to degrade fails here.
 */
async function mount(name, capabilities = {}) {
  const dom = installDom(capabilities);
  try {
    // Imported inside the shim's lifetime: dom.js is import-safe, but the views must not touch the
    // document at module scope either, and this is what proves it.
    const mod = await import(`../js/ui/${name}.js?t=${Math.random()}`);
    // By name, not "the first exported function" - a view may export helpers alongside its mount.
    const fn = Object.entries(mod).find(([k, v]) => typeof v === 'function' && k.startsWith('mount'))?.[1];
    ok(fn, `${name}.js exports no mount function`);
    const root = dom.document.createElement('section');
    const ctx = { layout, patches, card: null, setCard() {} };
    const handle = fn(root, ctx);
    return { root, ctx, handle, dom };
  } finally {
    dom.restore();
  }
}

// --- tab order ------------------------------------------------------------------------------------
//
// Pinned because it is a considered decision that reads like an arbitrary one, and so is exactly the
// kind of thing a later edit undoes by accident. Verify is the most VALUABLE screen but the wrong
// FIRST screen: the entry state for someone who just bought a device is "I have no card yet", and all
// Verify can say to that is "this is not a card".

const TAB_ORDER = ['build', 'convert', 'verify', 'terminal'];

test('the landing tab is Build, and the tabs run in the order a person needs them', () => {
  const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
  const tabs = [...html.matchAll(/data-view="(\w+)"/g)].map((m) => m[1]);
  eq(tabs, TAB_ORDER);
  const panels = [...html.matchAll(/id="panel-(\w+)"/g)].map((m) => m[1]);
  eq(panels, TAB_ORDER, 'panels must follow the tabs, or the wrong one is visible on load');
  ok(/data-view="build"[^>]*aria-selected="true"/.test(html), 'Build is selected on load');
  ok(html.includes('<section id="panel-build" role="tabpanel"></section>'),
    'and its panel is the one not hidden');
});

test('main.js defaults to the same tab the markup pre-selects', () => {
  // Two sources of truth for "which tab is first" - the markup and the VIEWS map - so assert they
  // agree rather than trusting that whoever reorders one remembers the other.
  const js = readFileSync(new URL('../js/ui/main.js', import.meta.url), 'utf8');
  const order = [...js.matchAll(/^ {2}(\w+): mount/gm)].map((m) => m[1]);
  eq(order, TAB_ORDER);
  ok(js.includes('Object.keys(VIEWS)[0]'), 'the default is derived from the map, not restated');
});

// --- verify -------------------------------------------------------------------------------------

test('the verify view mounts and offers both ways in', async () => {
  const { root } = await mount('verify_view');
  const labels = tags(root, 'button').map((b) => b.textContent);
  ok(labels.includes('Choose card folder'), labels.join(','));
  ok(labels.includes('Browse for folder'), 'the fallback for browsers without File System Access');
  ok(root.querySelector('.dropzone'), 'a drop target');
  ok(root.querySelector('.results'), 'somewhere to put findings');
});

test('the verify view disables in-place access when the browser lacks the API', async () => {
  // Degrading rather than breaking is the whole reason the app is designed around the read-only path.
  const { root } = await mount('verify_view');
  const pick = tags(root, 'button').find((b) => b.textContent === 'Choose card folder');
  ok(pick.disabled, 'the picker is disabled without showDirectoryPicker');
  ok(textOf(root).includes('dropping a folder works here'), 'and says what still works');
});

test('the verify view enables in-place access where the browser has it', async () => {
  const { root } = await mount('verify_view', { fileSystemAccess: true });
  const pick = tags(root, 'button').find((b) => b.textContent === 'Choose card folder');
  eq(pick.disabled, false);
  ok(!textOf(root).includes('needs Chrome or Edge'), 'no upgrade notice where it is not needed');
});

test('the verify view states the rules that fail silently', async () => {
  const { root } = await mount('verify_view');
  const text = textOf(root);
  ok(text.includes(String(layout.scan.max_name)), 'the filename limit, taken from the layout');
  ok(text.includes('plays as noise'), 'the reason a wrong format is not simply rejected');
});

test('the verify view counts layouts and formats from the data, not from prose', () => {
  // The "eight folder layouts" figure in the docs silently became nine the moment softcut was added.
  // Prose does not have a test; this does.
  const banks = layout.banks.filter((b) => b.kind !== 'config').length;
  const formats = new Set(
    layout.banks.filter((b) => b.fmt.container !== 'text').map((b) => b.fmt.describe),
  ).size;
  eq(banks, 9);
  eq(formats, 4);
  // Comments stripped first: the file explains WHY the counts are derived, and that explanation names
  // the numbers. It is the rendered copy that must not.
  const src = readFileSync(new URL('../js/ui/verify_view.js', import.meta.url), 'utf8')
    .split('\n').map((l) => l.replace(/(^|\s)\/\/.*$/, '')).join('\n');
  ok(!/\b(eight|nine|ten|four)\b/.test(src),
    'the view must not hardcode a count that the layout can change');
});

// --- build --------------------------------------------------------------------------------------

test('the build view mounts, listing every folder it creates', async () => {
  const { root } = await mount('build_view');
  const text = textOf(root);
  for (const bank of layout.banks) ok(text.includes(bank.engine), `${bank.engine} missing from the table`);
  ok(tags(root, 'button').some((b) => b.textContent === 'Download a starter card (.zip)'));
});

test('the build view disables in-place writing without File System Access, but not the zip', async () => {
  const { root } = await mount('build_view');
  const buttons = tags(root, 'button');
  eq(buttons.find((b) => b.textContent === 'Download a starter card (.zip)').disabled, false, 'zip always works');
  eq(buttons.find((b) => b.textContent === 'Write onto a card').disabled, true);
});

test('folder labels collapse to a shared parent instead of reading as a range', async () => {
  const { folderLabel } = await import('../js/ui/build_view.js');
  eq(folderLabel(['tapes']), 'tapes');
  eq(folderLabel(['SK/B', 'SK/G', 'SK/P', 'SK/R', 'SK/T', 'SK/Y']), 'SK/{B,G,P,R,T,Y}');
  eq(folderLabel(['radio/0', 'radio/1', 'radio/2']), 'radio/{0..2}');
  eq(folderLabel(['a/1', 'a/3']), 'a/{1,3}', 'non-contiguous numbers are listed, not ranged');
  eq(folderLabel(['x/1', 'y/2']), 'x/1, y/2', 'no shared parent - a clumsy label beats a wrong one');
});

test('the build view labels granular as a set under SK, not a two-folder range', async () => {
  const { root } = await mount('build_view');
  const text = textOf(root);
  ok(text.includes('SK/{B,G,P,R,T,Y}'), 'granular reads as six folders under SK');
  ok(text.includes('radio/{0..15}'), 'and radio as a numeric run');
  ok(!text.includes('SK/B .. SK/Y'), 'the old range form is gone');
});

test('the build view names every engine that reads a folder, not just the owning one', async () => {
  // SK/{B,G,P,R,T,Y} is the platform's shared tape store. Listing only "granular" makes graincloud's
  // loops look like they belong nowhere - and granular is not even a published engine.
  const { root } = await mount('build_view');
  ok(textOf(root).includes('granular, graincloud'), 'both readers of the shared tape store are named');
  eq(layout.bank('granular').readers, ['granular', 'graincloud']);
  eq(layout.bank('tape').readers, ['tape'], 'a bank with one reader stays unadorned');
});

test('the build view enables in-place writing where the browser has the API', async () => {
  const { root } = await mount('build_view', { fileSystemAccess: true });
  eq(tags(root, 'button').find((b) => b.textContent === 'Write onto a card').disabled, false);
});

test('the build view points at the released card for demo audio rather than synthesizing it', async () => {
  const { root } = await mount('build_view');
  ok(textOf(root).includes('sk-card-<version>.zip'));
  ok(tags(root, 'a').some((a) => a.href.includes('/releases/')));
});

// --- convert ------------------------------------------------------------------------------------

test('the convert view offers exactly the banks that take audio', async () => {
  const { root } = await mount('convert_view');
  const options = tags(root, 'select')[0].children.map((o) => o.textContent);
  eq(options.sort(), layout.audioBanks().map((b) => b.engine).sort());
});

test('the convert view hides the controls a bank does not use', async () => {
  const { root } = await mount('convert_view');
  const field = (label) => root.querySelectorAll('.field').find((f) => f.textContent.startsWith(label));
  // Default bank is granular: it takes a tape folder, not a deck or a shelf, and its rate is fixed.
  eq(field('Deck').hidden, true);
  eq(field('Bank / shelf').hidden, true);
  eq(field('Tape').hidden, false);
  eq(field('Sample rate').hidden, true, 'granular is 48 kHz or nothing');
});

test('switching to bard exposes the rate control and defaults it to 24 kHz', async () => {
  // bard is the one bank with no fixed rate; 24 kHz is the right rate for speech (half the bytes per
  // hour), so the control appears and starts there rather than at the 48 kHz everything else uses.
  const { root } = await mount('convert_view');
  const engine = tags(root, 'select')[0];
  engine.value = 'bard';
  await engine.fire('change');
  const field = (label) => root.querySelectorAll('.field').find((f) => f.textContent.startsWith(label));
  eq(field('Sample rate').hidden, false);
  eq(field('Bank / shelf').hidden, false);
  eq(tags(root, 'input').find((i) => i.className === 'slot' && i.min === '3000').value, '24000');
});

test('the convert view is honest about resampling not matching the CLI', async () => {
  const { root } = await mount('convert_view');
  ok(textOf(root).includes('not bit-identical'), 'the caveat must be on the page, not only in the docs');
});

test('convert starts with nothing to do', async () => {
  const { root } = await mount('convert_view');
  for (const label of ['Convert', 'Download as .zip', 'Save onto the card']) {
    eq(tags(root, 'button').find((b) => b.textContent === label).disabled, true, label);
  }
});

// --- terminal -----------------------------------------------------------------------------------

test('the terminal view mounts and leads with the firmware caveat', async () => {
  const { root } = await mount('terminal_view');
  const text = textOf(root);
  ok(text.includes('Released firmware has no terminal'),
    'a user must not spend an afternoon wondering why nothing answers');
  ok(text.includes('TERMINAL=1'), 'and must be told what build does work');
});

test('the terminal view says WebSerial is missing rather than silently doing nothing', async () => {
  const { root } = await mount('terminal_view');
  ok(textOf(root).includes('no WebSerial'));
  ok(textOf(root).includes('no zip-shaped substitute'), 'and that there is no fallback for this tab');
});

test('the terminal view drops the WebSerial notice where the browser has it', async () => {
  const { root } = await mount('terminal_view', { serial: true });
  ok(!textOf(root).includes('no WebSerial'));
  // The firmware caveat is not browser-dependent and must stay in both cases.
  ok(textOf(root).includes('Released firmware has no terminal'));
});

test('the terminal command line starts disabled until a device is connected', async () => {
  const { root } = await mount('terminal_view');
  eq(root.querySelector('.cmdline').disabled, true);
});

test('the terminal view builds the console, CPU plot and USB panels', async () => {
  const { root } = await mount('terminal_view');
  ok(root.querySelector('.console'), 'console');
  ok(root.querySelector('canvas'), 'the CPU plot');
  ok(root.querySelector('.cpu-readout'), 'the numeric readout beside it');
  ok(root.querySelector('.usb'), 'the usb table');
  ok(textOf(root).includes('reset cpu'), 'the button a measurement needs');
});

test('destructive actions are marked, and reset cpu is not one of them', async () => {
  const { root } = await mount('terminal_view');
  // The surface is only generated once a device answers `describe`, but the static CPU control is
  // present from mount - and `reset cpu` shares a verb with the destructive `reset`, so it is the one
  // most likely to be misclassified.
  const resetCpu = tags(root, 'button').find((b) => b.textContent === 'reset cpu');
  ok(resetCpu && resetCpu.className !== 'danger', '`reset cpu` only clears meter extremes');
});
