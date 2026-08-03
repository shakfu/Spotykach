// ui.test.ts - the views must mount, and must say what they promise to say.
//
// Deliberately smaller than it was. Every tab's state and behaviour now lives in a view-model
// (src/app/), and model.test.ts checks that with no DOM at all - so what is left here is the part that
// genuinely needs a document: a view builds its whole UI imperatively in one mount call, so a typo in
// an el() property, a missing import or an exception mid-construction produces a blank tab, and a
// blank tab is exactly the failure a user cannot report usefully.

import { readFileSync, statSync } from 'node:fs';

import { suite, test, ok, eq, readWeb, layoutData, engineData } from './harness.ts';
import { installDom, tags, textOf, type ShimNode } from './dom_shim.ts';
import { makeLayout } from '../src/core/layout.ts';
import { makeCatalogue } from '../src/core/engines.ts';
import { Store } from '../src/app/store.ts';
import { nextTabIndex } from '../src/ui/tabs.ts';
import { parseHash } from '../src/ui/route.ts';
import { THEMES, DEFAULT_THEME, currentTheme } from '../src/ui/theme.ts';

suite('ui');

const layout = makeLayout(layoutData());
const patches = readWeb<Record<string, string>>('patches.json');
const engines = makeCatalogue(engineData(), layout);

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
    fn(root, { layout, engines, patches, engineFocus: new Store({ engine: null }) });
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

const TAB_ORDER = ['build', 'convert', 'verify', 'reference', 'terminal', 'flash'];

test('the landing tab is Build, and the tabs run in the order a person needs them', () => {
  const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
  const tabs = [...html.matchAll(/data-view="(\w+)"/g)].map((m) => m[1]);
  eq(tabs, TAB_ORDER);
  // Only the TAB panels: `panel-engine` is a route, not a tab, and deliberately has no role.
  const panels = [...html.matchAll(/id="panel-(\w+)" role="tabpanel"/g)].map((m) => m[1]);
  eq(panels, TAB_ORDER, 'panels must follow the tabs, or the wrong one is visible on load');
  ok(/id="panel-engine"(?![^>]*role="tabpanel")/.test(html),
    'the engine page is not a tabpanel - it would put a sixth tab in the tablist');
  ok(/data-view="build"[^>]*aria-selected="true"/.test(html), 'Build is selected on load');
  // By meaning rather than by literal markup: the build panel is the one WITHOUT `hidden`, and every
  // other panel has it. Matching the exact tag made this fail the moment the panel gained an
  // aria-labelledby, which is a test breaking on something it was not there to check.
  const panelTag = (view: string) =>
    html.match(new RegExp(`<section id="panel-${view}"[^>]*>`))![0];
  ok(!panelTag('build').includes('hidden'), 'the build panel is visible on load');
  for (const view of TAB_ORDER.slice(1)) {
    ok(panelTag(view).includes('hidden'), `${view}: every other panel starts hidden`);
  }
});

test('main.ts defaults to the same tab the markup pre-selects', () => {
  // Two sources of truth for "which tab is first" - the markup and the VIEWS map - so assert they
  // agree rather than trusting that whoever reorders one remembers the other.
  const js = readFileSync(new URL('../src/ui/main.ts', import.meta.url), 'utf8');
  const order = [...js.matchAll(/^ {2}(\w+): mount/gm)].map((m) => m[1]);
  eq(order, TAB_ORDER);
  ok(js.includes('Object.keys(VIEWS)[0]'), 'the default is derived from the map, not restated');
});

test('the tablist honours the contract its roles promise', () => {
  // Declaring role=tablist/role=tab tells assistive tech two things: the group is ONE stop in the tab
  // order, and the arrows move within it. Declaring them without implementing them is worse than
  // using plain buttons - the widget is then broken rather than merely plain.
  const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
  const tabs = [...html.matchAll(/<button id="tab-(\w+)"[^>]*>/g)];
  eq(tabs.map((m) => m[1]), TAB_ORDER, 'every tab carries an id, which the panel refers back to');

  for (const [tag, view] of tabs) {
    ok(tag.includes(`aria-controls="panel-${view}"`), `${view}: tab must name its panel`);
    // Exactly one tab is reachable by Tab; the rest are reached with the arrows.
    const wants = view === TAB_ORDER[0] ? '0' : '-1';
    ok(tag.includes(`tabindex="${wants}"`), `${view}: roving tabindex should start at ${wants}`);
  }
  for (const view of TAB_ORDER) {
    ok(html.includes(`id="panel-${view}" role="tabpanel" aria-labelledby="tab-${view}"`),
      `${view}: panel must point back at its tab`);
  }
  ok(/id="tabs"[^>]*aria-label=/.test(html), 'the tablist is named');

  const js = readFileSync(new URL('../src/ui/main.ts', import.meta.url), 'utf8');
  ok(js.includes('tab.tabIndex = selected ? 0 : -1'), 'and tabindex is kept in step with selection');
});

test('arrow keys move around the tab row, and wrap', () => {
  const n = TAB_ORDER.length;
  eq(nextTabIndex('ArrowRight', 0, n), 1);
  eq(nextTabIndex('ArrowLeft', 1, n), 0);
  eq(nextTabIndex('ArrowRight', n - 1, n), 0, 'the last tab wraps forward to the first');
  eq(nextTabIndex('ArrowLeft', 0, n), n - 1, 'and the first wraps back to the last');
  eq(nextTabIndex('Home', 3, n), 0);
  eq(nextTabIndex('End', 0, n), n - 1);
});

test('the tab row ignores keys that are not its own', () => {
  // Returning null rather than a number is what lets main.ts leave preventDefault alone - otherwise
  // Tab, Enter and every character key would be swallowed by the tablist.
  for (const key of ['Tab', 'Enter', ' ', 'a', 'ArrowUp', 'Escape']) {
    eq(nextTabIndex(key, 0, 5), null, key);
  }
  eq(nextTabIndex('ArrowRight', -1, 5), null, 'and it does nothing when focus is outside the row');
});

test('nothing in the chrome is a dead affordance', () => {
  // The close and resize widgets were dropped for this reason and then a four-item menu bar was added
  // that did nothing, which is the same mistake with more pixels. The rule: every menu item opens a
  // menu, and every menu contains a real control.
  const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
  const items = [...html.matchAll(/(<li role="menu-item"[^>]*>)([\s\S]*?)<\/li>/g)]
    .map((m) => [m[1], m[2]] as const);
  ok(items.length > 0, 'there is a menu bar at all');
  const mainSrc = readFileSync(new URL('../src/ui/main.ts', import.meta.url), 'utf8');
  for (const [tag, body] of items) {
    const id = tag.match(/id="([\w-]+)"/)?.[1];
    // Either the menu is in the markup, or main.ts fills it from data. What is not allowed is neither.
    const filled = /<ul role="menu">|<button|<a /.test(body) || (id && mainSrc.includes(`#${id}`));
    ok(filled, `a menu item with nothing behind it: ${body.trim().slice(0, 40)}`);
  }
  ok(!/>\s*(File|Edit)\s*</.test(html), 'the painted File/Edit menus are gone');

  // And the one item that exists is wired up.
  const js = readFileSync(new URL('../src/ui/main.ts', import.meta.url), 'utf8');
  ok(js.includes('#about-open') && js.includes('showModal'), 'About is actually opened by main.ts');
});

test('the page loads the built bundle, not the sources', () => {
  // The browser cannot run TypeScript. If this ever points back at src/, the page is blank in every
  // browser and green in every test here, because the tests import the sources directly.
  const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
  ok(html.includes('src="./dist/app.js"'), 'index.html must load dist/app.js');
  ok(!html.includes('src="./src/'), 'and must not try to load a .ts entry point');
});

for (const sheet of ['app.css', 'themes/system6.css', 'themes/plain.css', 'themes/dark.css']) {
  test(`${sheet} defines no class the app never uses`, () => {
  // The visual vocabulary is the thing that made this page feel complicated - six boxed styles
  // competing at the same weight. Rules outliving their markup is how that grows back: `.steps`
  // survived the tab it was written for and nothing said so.
  //
  // Deliberately loose in the safe direction: "used" means the name appears anywhere in the views'
  // source, because classes are assembled from template literals (`finding ${cls}`) and variables, so
  // anything stricter would report false alarms on real code.
  const css = readFileSync(new URL(`../${sheet}`, import.meta.url), 'utf8');
  const selectors = css.replace(/\{[^}]*\}/g, ' ').replace(/\/\*[\s\S]*?\*\//g, ' ');
  const defined = new Set([...selectors.matchAll(/\.([a-z][\w-]*)/g)].map((m) => m[1]));

  const sources = ['index.html', ...['main', 'dom', 'build_view', 'convert_view', 'verify_view',
    'reference_view', 'terminal_view', 'cpu_plot', 'engine_view', 'lightbox'].map((f) => `src/ui/${f}.ts`)]
    .map((f) => readFileSync(new URL(`../${f}`, import.meta.url), 'utf8')).join('\n');

    const unused = [...defined].filter((c) => !sources.includes(c));
    eq(unused, [], 'these are styled but nothing wears them');
  });
}

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

test('the reference view lists every engine, not only the ones that read a card', async () => {
  // This screen used to be the ten card banks; the other twelve engines were a footnote saying they
  // existed. "What is the delay engine" and "what does tape want on the card" are the same person's
  // question, so both are answered here.
  const { root } = await mount('reference_view');
  const sections = root.querySelectorAll('.ref-bank');
  eq(sections.length, engines.entries.length, 'one section per engine');

  for (const entry of engines.entries) {
    const s = sections.find((n) => n.dataset.engine === entry.doc.name)!;
    ok(s, `${entry.doc.name} has no entry`);
    ok(s.textContent.includes(entry.doc.body.slice(0, 40)), `${entry.doc.name} states no description`);
    if (entry.bank) {
      ok(s.textContent.includes(entry.bank.fmt.describe), `${entry.doc.name} omits its format`);
      ok(s.textContent.includes(entry.bank.source), `${entry.doc.name} omits its firmware citation`);
    } else {
      ok(s.textContent.includes('needs no card'), `${entry.doc.name} should say it needs no card`);
    }
  }
});

test('the engines that need no card are the ones with no bank', async () => {
  const { root } = await mount('reference_view');
  const cardless = engines.entries.filter((e) => !e.bank).map((e) => e.doc.name);
  ok(cardless.includes('delay') && cardless.includes('reverb'), 'the effects need no card');
  ok(!cardless.includes('tape'), 'and the streaming engines do');
  ok(textOf(root).includes('delay'), 'delay is listed even though it reads nothing');
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
  // `platform` is a bank with no docs/engines page - it is the shared SK/ folder, not an engine. It
  // still has to appear, because it carries the only listing of config.txt's accepted properties.
  const { root } = await mount('reference_view');
  const platform = root.querySelectorAll('.ref-bank').find((s) => s.dataset.engine === 'platform')!;
  ok(platform, 'the platform entry must survive being absent from docs/engines/');
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

// --- routing ---------------------------------------------------------------------------------------

test('an engine link is shareable', () => {
  // `#engine/bard` has to survive being pasted into a message. Somebody answering "what format does
  // bard want?" should be able to send a URL that lands on the answer rather than on the Build tab.
  eq(parseHash('#engine/bard'), { view: 'reference', engine: 'bard' });
  eq(parseHash('#verify'), { view: 'verify', engine: null });
  eq(parseHash('#'), { view: '', engine: null });
  eq(parseHash(''), { view: '', engine: null });
});

test('a malformed engine link lands on the Reference tab rather than nowhere', () => {
  // The name is not validated here - the model rejects one it does not know - but the VIEW is still
  // the right one, so a mistyped link shows the engine list instead of falling back to Build.
  // Both forms land on the engine list rather than falling back to Build, which is what a bare
  // `#engine` should do anyway - it is a request for engines, just an underspecified one.
  eq(parseHash('#engine/'), { view: 'reference', engine: null });
  eq(parseHash('#engine'), { view: 'reference', engine: null });
});

test('selecting an unknown engine changes nothing', async () => {
  const { ReferenceModel } = await import('../src/app/reference_model.ts');
  const m = new ReferenceModel(layout, engines);
  m.select('not-an-engine');
  eq(m.store.get().pinned, null, 'a bad link must not empty the list');
  eq(m.visible().length, engines.entries.length);
});

test('the engines menu is built from the catalogue, not from the markup', () => {
  const js = readFileSync(new URL('../src/ui/main.ts', import.meta.url), 'utf8');
  ok(js.includes('ctx.engines.entries.map'), 'the menu iterates the catalogue');
  const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
  for (const entry of engines.entries) {
    ok(!html.includes(`>${entry.doc.name}<`), `${entry.doc.name} is hardcoded into the menu markup`);
  }
});

// --- themes ----------------------------------------------------------------------------------------

test('the head script and theme.ts offer the same themes', () => {
  // The list is duplicated on purpose - the theme has to be settled before the first paint, which
  // means an inline script, which means a second copy. A silent disagreement between them is a page
  // that flashes the wrong theme on every load, so it is asserted rather than trusted.
  const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
  const head = html.slice(0, html.indexOf('</head>'));
  // The SCRIPT, not the whole head: `plain` appears in `./themes/plain.css` either way, so scanning
  // the head let a broken comparison (`t === 'plane'`) pass. It is the literal being compared that
  // has to match the id.
  const script = head.slice(head.indexOf('<script>'), head.indexOf('</script>'));
  ok(script.length > 50, 'there is a pre-paint theme script at all');
  for (const theme of THEMES) {
    if (theme.id === DEFAULT_THEME) {
      // The default is what the markup ships with; the script only has to handle the others.
      ok(head.includes(`href="${theme.framework}"`), `${theme.id}: not the default framework in markup`);
      ok(head.includes(`href="${theme.skin}"`), `${theme.id}: not the default skin in markup`);
      continue;
    }
    ok(script.includes(`'${theme.id}'`), `${theme.id}: the head script compares against no such id`);
    ok(script.includes(theme.framework), `${theme.id}: its framework is never applied pre-paint`);
    ok(script.includes(theme.skin), `${theme.id}: its skin is never applied pre-paint`);
  }
  ok(script.includes('sk-card-theme'), 'the head script reads the same storage key');
});

test('every theme names two stylesheets that exist', () => {
  for (const theme of THEMES) {
    for (const href of [theme.framework, theme.skin]) {
      const path = href.replace(/^\.\//, '');
      ok(statSync(new URL(`../${path}`, import.meta.url).pathname).size > 0, `${theme.id}: ${href}`);
    }
    ok(theme.note && theme.label, `${theme.id}: a theme needs a label and a reason`);
  }
});

test('an unknown saved theme falls back rather than leaving the page unstyled', async () => {
  const dom = installDom({});
  try {
    (globalThis as { localStorage?: unknown }).localStorage = {
      getItem: () => 'a-theme-that-was-removed',
      setItem: () => {},
    };
    eq(currentTheme(), DEFAULT_THEME);
  } finally {
    delete (globalThis as { localStorage?: unknown }).localStorage;
    dom.restore();
  }
});

test('the theme menu is built from the list, not from the markup', () => {
  const js = readFileSync(new URL('../src/ui/main.ts', import.meta.url), 'utf8');
  ok(js.includes('THEMES.map'), 'the View menu iterates the theme list');
  const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
  for (const theme of THEMES) {
    ok(!html.includes(`>${theme.label}<`), `${theme.label} is hardcoded into the menu markup`);
  }
});

test('there is a way home from anywhere', () => {
  // It matters most on an engine page: that route selects no tab, so without this the way back to
  // the start is a guess. A button rather than a menu - "go to the start" has one meaning, and a
  // menu holding one entry is ceremony.
  const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
  ok(/<li role="menu-item" id="home-item">\s*<button id="home-link"/.test(html),
    'the menu bar has a Home control');

  const js = readFileSync(new URL('../src/ui/main.ts', import.meta.url), 'utf8');
  ok(js.includes("$('#home-link')?.addEventListener"), 'and main.ts wires it');
  ok(js.includes('show(DEFAULT_VIEW)'), 'to the landing tab, derived rather than named');
  ok(js.includes('location.pathname + location.search'),
    'dropping the fragment, so the URL is the bare page again rather than #engine/<name>');
});

test('a theme cannot collapse a field the app sized', () => {
  // The bug this pins, which shipped: both themes reset `input[type=text] { width: auto }` to undo
  // system.css forcing inputs to 100%. That reset and `input.cmdline` have the SAME specificity, and
  // themes load last - so the command line, the reference filter and the slot box all collapsed to
  // their default width. An id in the app's selector puts it out of reach of any element-level rule.
  const app = readFileSync(new URL('../app.css', import.meta.url), 'utf8')
    .replace(/\/\*[\s\S]*?\*\//g, '');
  const themeResets = ['themes/system6.css', 'themes/plain.css', 'themes/dark.css']
    .map((f) => readFileSync(new URL(`../${f}`, import.meta.url), 'utf8'))
    .some((css) => /input\[type=text\][\s\S]{0,120}width:/.test(css));
  ok(themeResets, 'a theme still resets input widths - this test is guarding a live hazard');

  for (const m of app.matchAll(/^([^{}\n]*input\.[\w-]+[^{}\n]*)\{([^}]*)\}/gm)) {
    if (!/\bwidth\s*:/.test(m[2])) continue;
    ok(m[1].includes('#'),
      `"${m[1].trim()}" sets a width but a theme's element rule will outrank it on load order`);
  }
});

test('the shared stylesheet holds no colour a theme should own', () => {
  // The rule that keeps two themes from being two applications: app.css describes structure through
  // tokens, and a literal colour in it is a bug waiting for the OTHER theme. This was not theoretical
  // - `a { color: #000 }` made every link black in the Plain theme, and `.verdict.bad strong` rendered
  // a failing verdict inverted there, because both sat in the shared layer and Plain lost the
  // specificity race to override them.
  //
  // Two literals are allowed, each because it is genuinely the same in both themes.
  const ALLOWED = new Map([
    ['#fff', 'the control diagrams are black line art on transparency and need a backing in dark mode'],
    ['rgb(', 'the modal scrim is a translucent black in a light theme and a dark one alike'],
  ]);

  const css = readFileSync(new URL('../app.css', import.meta.url), 'utf8')
    .replace(/\/\*[\s\S]*?\*\//g, '');
  const found = [...css.matchAll(/#[0-9a-f]{3,8}\b|\b(?:rgb|hsl)a?\(/gi)].map((m) => m[0].toLowerCase());
  const offending = [...new Set(found)].filter((c) => !ALLOWED.has(c));
  eq(offending, [], 'these belong in a theme, not in the shared layer');
});
