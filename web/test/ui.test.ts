// ui.test.ts - the views must mount, and must say what they promise to say.
//
// Deliberately smaller than it was. Every tab's state and behaviour now lives in a view-model
// (src/app/), and model.test.ts checks that with no DOM at all - so what is left here is the part that
// genuinely needs a document: a view builds its whole UI imperatively in one mount call, so a typo in
// an el() property, a missing import or an exception mid-construction produces a blank tab, and a
// blank tab is exactly the failure a user cannot report usefully.

import { readFileSync } from 'node:fs';

import { suite, test, ok, eq, readWeb, layoutData } from './harness.ts';
import { installDom, tags, textOf, type ShimNode } from './dom_shim.ts';
import { makeLayout } from '../src/core/layout.ts';

suite('ui');

const layout = makeLayout(layoutData());
const patches = readWeb<Record<string, string>>('patches.json');

/**
 * A view's source with its comments removed.
 *
 * Several tests below assert that a view does not write down a fact the layout owns. The comments are
 * where those facts are legitimately discussed - a comment explaining WHY a count is derived has to
 * name the count - so it is the executable half that must be checked. Both comment forms are stripped:
 * missing block comments would let `10.9` in a doc comment answer for a hardcoded 10.
 */
const code = (view: string): string =>
  readFileSync(new URL(`../src/ui/${view}.ts`, import.meta.url), 'utf8')
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .split('\n').map((l) => l.replace(/(^|\s)\/\/.*$/, '')).join('\n');

interface Mounted {
  root: ShimNode;
  dom: ReturnType<typeof installDom>;
}

/**
 * Mount a view in a fresh shimmed document, then restore the globals. The shim defaults to the least
 * capable browser (no File System Access, no WebSerial), so a view that forgets to degrade fails here.
 */
async function mount(
  name: string,
  capabilities: Parameters<typeof installDom>[0] = {},
  during: ((m: Mounted) => Promise<void> | void) | null = null,
): Promise<Mounted> {
  const dom = installDom(capabilities);
  try {
    // Imported inside the shim's lifetime: dom.ts is import-safe, but the views must not touch the
    // document at module scope either, and this is what proves it.
    const mod = await import(`../src/ui/${name}.ts?t=${Math.random()}`);
    // By name, not "the first exported function" - a view may export helpers alongside its mount.
    const fn = Object.entries(mod)
      .find(([k, v]) => typeof v === 'function' && k.startsWith('mount'))?.[1] as
        ((root: unknown, ctx: unknown) => void) | undefined;
    ok(fn, `${name}.ts exports no mount function`);
    const root = dom.document.createElement('section') as ShimNode;
    fn(root, { layout, patches });
    const mounted = { root, dom };
    // Interactions that CREATE elements have to run while the shim is still installed. Inspecting the
    // tree afterwards is fine, which is why most tests below do not need this.
    if (during) await during(mounted);
    return mounted;
  } finally {
    dom.restore();
  }
}

// --- tab order ------------------------------------------------------------------------------------
//
// Pinned because it is a considered decision that reads like an arbitrary one, and so is exactly the
// kind of thing a later edit undoes by accident. Verify is the most VALUABLE screen but the wrong
// FIRST screen: the entry state for someone who just bought a device is "I have no card yet", and all
// Verify can say to that is "this is not a card". Reference follows the three task tabs rather than
// joining them - it is the lookup you consult while doing the job, not a step in it - and Terminal
// stays last because it needs a firmware build almost nobody has.

const TAB_ORDER = ['build', 'convert', 'verify', 'reference', 'terminal'];

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

test('main.ts defaults to the same tab the markup pre-selects', () => {
  // Two sources of truth for "which tab is first" - the markup and the VIEWS map - so assert they
  // agree rather than trusting that whoever reorders one remembers the other.
  const js = readFileSync(new URL('../src/ui/main.ts', import.meta.url), 'utf8');
  const order = [...js.matchAll(/^ {2}(\w+): mount/gm)].map((m) => m[1]);
  eq(order, TAB_ORDER);
  ok(js.includes('Object.keys(VIEWS)[0]'), 'the default is derived from the map, not restated');
});

test('the page loads the built bundle, not the sources', () => {
  // The browser cannot run TypeScript. If this ever points back at src/, the page is blank in every
  // browser and green in every test here, because the tests import the sources directly.
  const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
  ok(html.includes('src="./dist/app.js"'), 'index.html must load dist/app.js');
  ok(!html.includes('src="./src/'), 'and must not try to load a .ts entry point');
});

test('the stylesheet defines no class the app never uses', () => {
  // The visual vocabulary is the thing that made this page feel complicated - six boxed styles
  // competing at the same weight. Rules outliving their markup is how that grows back: `.steps`
  // survived the tab it was written for and nothing said so.
  //
  // Deliberately loose in the safe direction: "used" means the name appears anywhere in the views'
  // source, because classes are assembled from template literals (`finding ${cls}`) and variables, so
  // anything stricter would report false alarms on real code.
  const css = readFileSync(new URL('../app.css', import.meta.url), 'utf8');
  const selectors = css.replace(/\{[^}]*\}/g, ' ').replace(/\/\*[\s\S]*?\*\//g, ' ');
  const defined = new Set([...selectors.matchAll(/\.([a-z][\w-]*)/g)].map((m) => m[1]));

  const sources = ['index.html', ...['main', 'dom', 'build_view', 'convert_view', 'verify_view',
    'reference_view', 'terminal_view', 'cpu_plot'].map((f) => `src/ui/${f}.ts`)]
    .map((f) => readFileSync(new URL(`../${f}`, import.meta.url), 'utf8')).join('\n');

  const unused = [...defined].filter((c) => !sources.includes(c));
  eq(unused, [], 'these are styled but nothing wears them');
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
  const pick = tags(root, 'button').find((b) => b.textContent === 'Choose card folder')!;
  ok(pick.disabled, 'the picker is disabled without showDirectoryPicker');
  ok(textOf(root).includes('dropping a folder works here'), 'and says what still works');
});

test('the verify view enables in-place access where the browser has it', async () => {
  const { root } = await mount('verify_view', { fileSystemAccess: true });
  const pick = tags(root, 'button').find((b) => b.textContent === 'Choose card folder')!;
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
  ok(!/\b(eight|nine|ten|four)\b/.test(code('verify_view')),
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
  eq(buttons.find((b) => b.textContent === 'Download a starter card (.zip)')!.disabled, false,
    'zip always works');
  eq(buttons.find((b) => b.textContent === 'Write onto a card')!.disabled, true);
});

test('the build view enables in-place writing where the browser has the API', async () => {
  const { root } = await mount('build_view', { fileSystemAccess: true });
  eq(tags(root, 'button').find((b) => b.textContent === 'Write onto a card')!.disabled, false);
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
  eq(layout.bank('granular')!.readers, ['granular', 'graincloud']);
  eq(layout.bank('tape')!.readers, ['tape'], 'a bank with one reader stays unadorned');
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
  const field = (label: string) =>
    root.querySelectorAll('.field').find((f) => f.textContent.startsWith(label))!;
  // Default bank is granular: it takes a tape folder, not a deck or a shelf, and its rate is fixed.
  eq(field('Deck').hidden, true);
  eq(field('Bank / shelf').hidden, true);
  eq(field('Tape').hidden, false);
  eq(field('Sample rate').hidden, true, 'granular is 48 kHz or nothing');
});

test('switching to bard exposes the rate control and defaults it to 24 kHz', async () => {
  // bard is the one bank with no fixed rate; 24 kHz is the right rate for speech (half the bytes per
  // hour), so the control appears and starts there rather than at the 48 kHz everything else uses.
  await mount('convert_view', {}, async ({ root }) => {
    const engine = tags(root, 'select')[0];
    engine.value = 'bard';
    await engine.fire('change');
    const field = (label: string) =>
      root.querySelectorAll('.field').find((f) => f.textContent.startsWith(label))!;
    eq(field('Sample rate').hidden, false);
    eq(field('Bank / shelf').hidden, false);
    eq(tags(root, 'input').find((i) => i.className === 'slot' && i.min === '3000')!.value, '24000');
  });
});

test('the convert view is honest about resampling not matching the CLI', async () => {
  const { root } = await mount('convert_view');
  ok(textOf(root).includes('not bit-identical'), 'the caveat must be on the page, not only in the docs');
});

test('convert starts with nothing to do', async () => {
  const { root } = await mount('convert_view');
  for (const label of ['Convert', 'Download as .zip', 'Save onto the card']) {
    eq(tags(root, 'button').find((b) => b.textContent === label)!.disabled, true, label);
  }
});

// --- reference ----------------------------------------------------------------------------------

test('the reference view gives every bank an entry, naming its folders and its format', async () => {
  const { root } = await mount('reference_view');
  const sections = root.querySelectorAll('.ref-bank');
  eq(sections.length, layout.banks.length, 'one section per bank');
  for (const bank of layout.banks) {
    const s = sections.find((n) => n.dataset.engine === bank.engine)!;
    ok(s, `${bank.engine} has no entry`);
    ok(s.textContent.includes(bank.fmt.describe), `${bank.engine} does not state its format`);
    ok(s.textContent.includes(bank.source), `${bank.engine} does not cite the firmware it mirrors`);
  }
});

test('the reference view needs nothing from the browser', async () => {
  // The other three tabs each degrade or disable something under the least-capable shim. This one is
  // pure data, so anything disabled here is a mistake - and it is the tab a Safari user is left with.
  const { root } = await mount('reference_view');
  ok(!tags(root, 'button').some((b) => b.disabled), 'no control is unavailable');
  const text = textOf(root);
  ok(!text.includes('Chrome or Edge'), 'and no upgrade notice');
  ok(!text.includes('WebSerial'));
});

test('the reference view states the constraints that fail silently', async () => {
  const { root } = await mount('reference_view');
  const text = textOf(root);
  ok(text.includes(String(layout.scan.max_name)), 'the filename limit');
  ok(text.includes(`${layout.scan.min_bytes / 1024} KB`), 'the size floor');
  ok(text.includes('.raw/.wav'), 'the extensions the scan indexes');
  ok(text.includes('plays as noise'), 'and why a wrong format is not simply rejected');
});

test('the reference view exposes the sidecar defaults, which appear on no other tab', async () => {
  // radio/rate.txt decides the playback rate of every file in the bank; getting it wrong detunes the
  // lot. It is written by Build and checked by Verify, but neither ever says what it should contain.
  const { root } = await mount('reference_view');
  const radio = root.querySelectorAll('.ref-bank').find((s) => s.dataset.engine === 'radio')!;
  ok(radio.textContent.includes('radio/rate.txt'), 'names the sidecar');
  ok(radio.textContent.includes('48000'), 'and its default contents');
});

test('the reference view lists the config properties and their ranges', async () => {
  const { root } = await mount('reference_view');
  const platform = root.querySelectorAll('.ref-bank').find((s) => s.dataset.engine === 'platform')!;
  for (const [k, [lo, hi]] of Object.entries(layout.configProperties)) {
    ok(platform.textContent.includes(k), `${k} is missing`);
    ok(platform.textContent.includes(`${lo}-${hi}`), `${k} does not state its range`);
  }
});

test('the reference view hardcodes no fact the layout owns', () => {
  // The same rule the verify view is held to, and it matters more here: this screen is nothing BUT
  // facts the layout owns, so one typed literal is a figure that silently outlives card_layout.py.
  const src = code('reference_view');
  for (const n of [layout.scan.max_name, layout.scan.min_bytes / 1024, layout.banks.length]) {
    ok(!new RegExp(`\\b${n}\\b`).test(src), `${n} is written down rather than derived`);
  }
  for (const bank of layout.banks) {
    ok(!src.includes(`'${bank.engine}'`), `${bank.engine} is named in the view`);
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
  eq(root.querySelector('.cmdline')!.disabled, true);
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

test('an empty port chooser reveals the unfiltered retry', async () => {
  // The model owns the explanation (model.test.ts checks the wording); what the VIEW owes is the
  // button, which must be absent until it is useful and present the moment it is.
  const notFound = () =>
    Promise.reject(Object.assign(new Error('No port selected'), { name: 'NotFoundError' }));
  await mount('terminal_view', { serial: { requestPort: notFound } }, async ({ root }) => {
    const button = (label: string) => tags(root, 'button').find((b) => b.textContent === label)!;
    eq(button('List every serial port').hidden, true, 'the escape hatch stays out of the normal path');
    await button('Connect').fire('click');
    eq(button('List every serial port').hidden, false, 'and appears once the filter finds nothing');
    ok(textOf(root).includes('vendor id'), 'with the console saying why');
  });
});
