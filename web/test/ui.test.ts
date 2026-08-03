// ui.test.ts - the views must mount, and must say what they promise to say.
//
// Deliberately smaller than it was. Every tab's state and behaviour now lives in a view-model
// (src/app/), and model.test.ts checks that with no DOM at all - so what is left here is the part that
// genuinely needs a document: a view builds its whole UI imperatively in one mount call, so a typo in
// an el() property, a missing import or an exception mid-construction produces a blank tab, and a
// blank tab is exactly the failure a user cannot report usefully.

import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';

import { suite, test, ok, eq, readWeb, layoutData, engineData } from './harness.ts';
import { installDom, tags, textOf, type ShimNode } from './dom_shim.ts';
import { makeLayout } from '../src/core/layout.ts';
import { makeCatalogue } from '../src/core/engines.ts';
import { Store } from '../src/app/store.ts';
import { parseHash } from '../src/ui/route.ts';
import { THEMES, DEFAULT_THEME, THEME_ATTR, STORAGE_KEY, currentTheme } from '../src/ui/theme.ts';

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
  /** Navigation the view REQUESTED, as [kind, target] pairs. See the note in mount(). */
  navigated: Array<[string, string]>;
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
    // Navigation is recorded rather than performed: a view's job is to ASK to go somewhere, and the
    // router's job is to do it. Capturing the request is what lets a test assert a card leads to the
    // right engine without booting the whole application.
    const navigated: Array<[string, string]> = [];
    fn(root, {
      layout,
      engines,
      patches,
      engineFocus: new Store({ engine: null }),
      go: (v: string) => navigated.push(['view', v]),
      goEngine: (e: string) => navigated.push(['engine', e]),
    });
    const mounted = { root, dom, navigated };
    // Interactions that CREATE elements have to run while the shim is still installed. Inspecting the
    // tree afterwards is fine, which is why most tests below do not need this.
    if (during) await during(mounted);
    return mounted;
  } finally {
    dom.restore();
  }
}

// --- navigation ----------------------------------------------------------------------------------
//
// The tab row is gone; the menu bar carries the global actions and the front page repeats the common
// ones. What is pinned here is the property that replaced "the tabs are in the right order": every
// view is reachable, and the menus cannot name a view that does not exist.

/** Views that must exist as panels. Order is menu order, which is still a considered decision. */
const VIEW_ORDER = ['home', 'engines', 'build', 'convert', 'verify', 'reference', 'flash', 'terminal',
  'engine'];

test('every view has a panel, and only the front page starts visible', () => {
  const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
  const panels = [...html.matchAll(/id="panel-([\w-]+)"/g)].map((m) => m[1]);
  eq(panels.slice().sort(), VIEW_ORDER.slice().sort(),
    'a view without a panel is a menu item that opens nothing');

  // By meaning rather than by literal markup: the home panel is the one WITHOUT `hidden`.
  const panelTag = (view: string) =>
    html.match(new RegExp(`<section id="panel-${view}"[^>]*>`))![0];
  ok(!panelTag('home').includes('hidden'), 'the front page is what a fresh visit shows');
  for (const view of VIEW_ORDER.filter((v) => v !== 'home')) {
    ok(panelTag(view).includes('hidden'), `${view}: every other panel starts hidden`);
  }
  ok(!html.includes('role="tablist"'), 'the tab row is gone, not merely hidden');
});

test('the menus name only views that exist, and every view is reachable', () => {
  // The menus are GENERATED from the VIEWS table, so this guards the table rather than the markup:
  // an entry with no menu and no route would be dead code nobody notices.
  const js = readFileSync(new URL('../src/ui/main.ts', import.meta.url), 'utf8');
  const table = js.slice(js.indexOf('const VIEWS'), js.indexOf('const DEFAULT_VIEW'));
  const declared = [...table.matchAll(/^ {2}(\w+): \{/gm)].map((m) => m[1]);
  eq(declared.slice().sort(), VIEW_ORDER.slice().sort(), 'VIEWS and the panels must agree');

  // Exactly the two groups the menu bar declares, and nothing routed into a third by a typo.
  const groups = new Set([...table.matchAll(/menu: '(\w+)'/g)].map((m) => m[1]));
  eq([...groups].sort(), ['card', 'device']);

  const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
  for (const id of ['card-menu', 'device-menu', 'engines-menu', 'theme-menu']) {
    ok(html.includes(`id="${id}"`), `${id}: the menu bar must host it`);
  }
  ok(js.includes("DEFAULT_VIEW = 'home'"), 'a fresh visit lands on the front page');
});

test('the Engines label is a destination, not only a menu', () => {
  // A top-level item that only opens a dropdown means the catalogue is reachable from the front page
  // and nowhere else - so anyone already deeper in the app has to go home to browse. The label is a
  // real button, and it carries the tab stop rather than the <li>, or the item costs two Tab presses.
  const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
  const item = html.match(/<li[^>]*id="engines-menu"[^>]*>[\s\S]*?<\/li>/)![0];
  ok(/<button[^>]*id="engines-link"/.test(item), 'the label must be a button');
  ok(!/<li[^>]*id="engines-menu"[^>]*tabindex/.test(item),
    'the button carries the tab stop, so the item must not also be focusable');
  const js = readFileSync(new URL('../src/ui/main.ts', import.meta.url), 'utf8');
  ok(js.includes("#engines-link") && js.includes("show('engines')"),
    'and main.ts must route it to the catalogue');
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

test('src/app.css defines no component class the app never uses', () => {
  // The visual vocabulary is the thing that made this page feel complicated - six boxed styles
  // competing at the same weight. Rules outliving their markup is how that grows back: `.steps`
  // survived the tab it was written for and nothing said so.
  //
  // Only @layer components is checked, and only the SOURCE stylesheet. Utilities are generated by
  // Tailwind from the markup, so they cannot go stale by construction - it is the hand-written
  // component rules that can. Scanning the built dist/app.css instead would be meaningless: every
  // class in it is there because something already used it.
  //
  // Deliberately loose in the safe direction: "used" means the name appears anywhere in the views'
  // source, because classes are assembled from template literals (`finding ${cls}`) and variables, so
  // anything stricter would report false alarms on real code.
  const css = readFileSync(new URL('../src/app.css', import.meta.url), 'utf8');
  const start = css.indexOf('@layer components');
  ok(start > 0, 'there is a components layer to check');
  const layer = css.slice(start);
  const selectors = layer.replace(/@apply[^;]*;/g, ' ').replace(/\{[^}]*\}/g, ' ')
    .replace(/\/\*[\s\S]*?\*\//g, ' ');
  const defined = new Set([...selectors.matchAll(/\.([a-z][\w-]*)/g)].map((m) => m[1]));

  // The whole directory, not a hand-listed subset. The list used to be written out here, which made
  // this test fail closed in the wrong direction: a NEW view's classes looked unused because nobody
  // remembered to add its filename, and the fix was to edit the test rather than the code. Reading
  // the directory means a view is covered the moment it exists.
  const uiDir = new URL('../src/ui/', import.meta.url).pathname;
  const sources = [
    readFileSync(new URL('../index.html', import.meta.url), 'utf8'),
    ...readdirSync(uiDir).filter((f) => f.endsWith('.ts'))
      .map((f) => readFileSync(join(uiDir, f), 'utf8')),
  ].join('\n');

  // Classes worn only by GENERATED markup (scripts/web_export.py writes the engine pages), so they
  // never appear in src/ and are not dead despite that.
  const generated = ['pdf-link'];

  const unused = [...defined].filter((c) => !sources.includes(c) && !generated.includes(c));
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
  eq(parseHash('#engine/bard'), { view: 'engine', engine: 'bard' });
  eq(parseHash('#verify'), { view: 'verify', engine: null });
  eq(parseHash('#'), { view: '', engine: null });
  eq(parseHash(''), { view: '', engine: null });
});

test('a malformed engine link lands on the catalogue rather than nowhere', () => {
  // The name is not validated here - the model rejects one it does not know - but the VIEW is still
  // the right one, so a mistyped link shows the engine catalogue instead of the front page. A bare
  // `#engine` is a request for engines, just an underspecified one.
  eq(parseHash('#engine/'), { view: 'engines', engine: null });
  eq(parseHash('#engine'), { view: 'engines', engine: null });
  // `#engines` and `#engine/<name>` differ by one letter, so the prefix must match EXACTLY.
  eq(parseHash('#engines'), { view: 'engines', engine: null });
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
  // The SCRIPT, not the whole head: an id can appear in a comment either way, and it is the literal
  // being COMPARED that has to match, so a typo (`t === 'drak'`) cannot pass.
  const script = head.slice(head.indexOf('<script>'), head.indexOf('</script>'));
  ok(script.length > 50, 'there is a pre-paint theme script at all');
  for (const theme of THEMES) {
    if (theme.id === DEFAULT_THEME) {
      // The default ships on the <html> tag, so the script never has to apply it - but the attribute
      // must be PRESENT, or the CSS selector and the menu disagree about what is current.
      ok(
        new RegExp(`<html[^>]*\\b${THEME_ATTR}="${theme.id}"`).test(html),
        `${theme.id}: the default theme is not on the <html> tag`,
      );
      continue;
    }
    ok(script.includes(`'${theme.id}'`), `${theme.id}: the head script compares against no such id`);
    ok(script.includes(THEME_ATTR), `${theme.id}: the head script sets no ${THEME_ATTR}`);
  }
  ok(script.includes(STORAGE_KEY), 'the head script reads the same storage key');
});

test('every theme is styled by the built stylesheet', () => {
  // Themes are custom-property blocks now, not files, so the check that a theme "exists" is that the
  // stylesheet actually carries a rule for it. A theme added to THEMES with no matching block would
  // otherwise be a menu entry that silently does nothing.
  const css = readFileSync(new URL('../dist/app.css', import.meta.url), 'utf8');
  for (const theme of THEMES) {
    ok(theme.note && theme.label, `${theme.id}: a theme needs a label and a reason`);
    if (theme.id === DEFAULT_THEME) continue; // the default is the :root palette
    // Minified CSS drops the quotes, so match both spellings rather than the source form.
    ok(
      css.includes(`[${THEME_ATTR}="${theme.id}"]`) || css.includes(`[${THEME_ATTR}=${theme.id}]`),
      `${theme.id}: the built stylesheet has no palette for it`,
    );
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

// The test that used to sit here - "a theme cannot collapse a field the app sized" - is deliberately
// gone rather than ported. It guarded a cascade race that no longer has the parts to happen: the app
// loaded THREE stylesheets, and a theme's `input[type=text] { width: auto }` tied `input.cmdline` on
// specificity and won on load order, collapsing the command line and the filter box. There is one
// stylesheet now and nothing loads after it, so the hazard is structural rather than watched-for. The
// test asserted the hazard was still live (`ok(themeResets, ...)`) precisely so it would not outlive
// its reason; keeping it would have meant staging a fake hazard for it to find.

test('the source stylesheet holds no colour outside the palette', () => {
  // The rule that keeps two themes from being two applications: the app describes structure through
  // tokens, and a literal colour outside the palette blocks is a bug waiting for the OTHER theme.
  // This was not theoretical - `a { color: #000 }` once made every link black in the light theme, and
  // a failing verdict rendered inverted there, because both sat in the shared layer.
  //
  // The palette blocks themselves are where literals BELONG, so they are cut before scanning: @theme
  // is the light palette and [data-theme="dark"] is the dark one. What is checked is everything else.
  let css = readFileSync(new URL('../src/app.css', import.meta.url), 'utf8')
    .replace(/\/\*[\s\S]*?\*\//g, '');
  css = css.replace(/@theme\s*\{[\s\S]*?\n\}/g, ' ')
           .replace(/\[data-theme="dark"\]\s*\{[\s\S]*?\n\}/g, ' ');

  // Two literals are still allowed, each because it is genuinely the same in both themes.
  const ALLOWED = new Map([
    ['#fff', 'the control diagrams are black line art on transparency and need a backing in dark mode'],
    ['rgb(', 'the modal scrim is a translucent black in a light theme and a dark one alike'],
  ]);
  const found = [...css.matchAll(/#[0-9a-f]{3,8}\b|\b(?:rgb|hsl)a?\(/gi)].map((m) => m[0].toLowerCase());
  const offending = [...new Set(found)].filter((c) => !ALLOWED.has(c));
  eq(offending, [], 'these belong in the palette, not in the component layer');
});

// --- the front page and the catalogue ---------------------------------------------------------------

test('the front page says what this is and offers the global actions', async () => {
  const { root } = await mount('home_view');
  const text = textOf(root);
  ok(text.includes('Spotykach'), 'it names the instrument this is firmware for');
  ok(text.includes('platform') && text.includes('engine'),
    'and states the platform/engine split, which is what the fork is FOR');
  // And does NOT repeat the project name. The menu bar names it and the window header says which
  // page you are on; a third "sk-engines" inside the panel told the reader nothing twice.
  ok(!text.includes('sk-engines'), 'the panel must not restate the project name');
  const labels = tags(root, 'button').map((b) => b.textContent);
  for (const want of ['Build a card', 'Convert audio', 'Verify a card', 'Flash firmware']) {
    ok(labels.some((l) => l?.includes(want)), `${want}: missing from the front page`);
  }
});

test('the front page counts engines rather than stating a number', () => {
  // The figure that goes stale the first time an engine is added and nobody greps for "22".
  const src = code('home_view');
  ok(/ctx\.engines\.entries/.test(src), 'the counts must be derived from the catalogue');
  ok(/\.length/.test(src), 'and counted, not quoted');
  // Any plausible engine or bank count, written down. The page claimed "23 engines" for exactly this
  // reason once - not a stale literal, but a count taken over the wrong set.
  ok(!/\b(1[0-9]|2[0-9])\b/.test(src), 'no engine or bank count may be written into the view');
});

test('a front-page action asks the router to go where it says', async () => {
  const { navigated } = await mount('home_view', {}, async ({ root }) => {
    await tags(root, 'button').find((b) => b.textContent?.includes('Verify a card'))!.fire('click');
  });
  eq(navigated, [['view', 'verify']]);
});

test('the catalogue shows every engine, with a shareable link each', async () => {
  const { root } = await mount('engines_view');
  // Engines, not catalogue entries: the synthetic bank entries (the shared `SK/` folder) have no
  // page and are not instruments. Counting them is how the front page came to claim 23 engines.
  const real = engines.entries.filter((e) => e.doc.page);
  ok(real.length < engines.entries.length, 'the fixture has at least one non-engine entry to exclude');
  const cards = tags(root, 'a').filter((a) => a.className?.includes('engine-card'));
  eq(cards.length, real.length, 'one card per engine');
  for (const e of real) {
    ok(cards.some((c) => c.href === `#engine/${e.doc.name}`),
      `${e.doc.name}: no card links to it`);
  }
});

test('every catalogue card carries a description', async () => {
  // Only 6 of 22 engines have a `summary`; the rest fall back to the opening of their manual. A card
  // with an empty subtitle is the visible symptom of that fallback breaking.
  const { root } = await mount('engines_view');
  const descs = tags(root, 'span').filter((s) => s.className?.includes('engine-card-desc'));
  eq(descs.length, engines.entries.filter((e) => e.doc.page).length);
  for (const d of descs) ok((d.textContent ?? '').trim().length > 10, 'a card with no description');
});

test('a catalogue card routes to that engine', async () => {
  const { navigated } = await mount('engines_view', {}, async ({ root }) => {
    // `href` is a property on the shim, as it is on a real anchor - not an attribute.
    const card = tags(root, 'a').find((a) => a.href === '#engine/bard')!;
    await card.fire('click');
  });
  eq(navigated, [['engine', 'bard']]);
});
