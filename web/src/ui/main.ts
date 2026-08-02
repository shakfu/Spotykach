// main.ts - bootstrap and tab switching.
//
// Views are mounted lazily, once, on first visit to their tab: the terminal opens a serial port and
// the convert view builds an AudioContext, and neither should happen because the page loaded.

import { makeLayout } from '../core/layout.ts';
import { makeCatalogue } from '../core/engines.ts';
import { Store } from '../app/store.ts';
import type { EngineData, LayoutData } from '../core/types.ts';
import { $, $$, el, showError } from './dom.ts';
import { mountBuild } from './build_view.ts';
import { mountConvert } from './convert_view.ts';
import { mountVerify } from './verify_view.ts';
import { mountReference } from './reference_view.ts';
import { mountTerminal } from './terminal_view.ts';
import { mountEngine } from './engine_view.ts';
import { nextTabIndex } from './tabs.ts';
import { parseHash } from './route.ts';
import { THEMES, applyTheme, currentTheme } from './theme.ts';
import type { EngineFocus, MountFn, ViewContext } from './context.ts';

// Declaration order is tab order; the first entry is what a fresh visit lands on. Build comes first
// because the entry state for someone who just bought a device is "I have no card yet", and Verify has
// nothing useful to say to that. Reference is a lookup rather than a step, so it follows the three task
// tabs; Terminal is last because it needs a firmware build almost nobody has.
const VIEWS: Record<string, MountFn> = {
  build: mountBuild,
  convert: mountConvert,
  verify: mountVerify,
  reference: mountReference,
  terminal: mountTerminal,
};

const DEFAULT_VIEW = Object.keys(VIEWS)[0];

/** The engine page's panel id suffix. Not in VIEWS: it is a route, not one of the five tabs. */
const ENGINE_PANEL = 'engine';

async function main(): Promise<void> {
  let ctx: ViewContext;
  try {
    const [layoutData, engineData, patches] = await Promise.all([
      fetch('./card_layout.json').then((r) => {
        if (!r.ok) throw new Error(`cannot load ./card_layout.json: HTTP ${r.status}`);
        return r.json() as Promise<LayoutData>;
      }),
      fetch('./engines.json').then((r) => {
        if (!r.ok) throw new Error(`cannot load ./engines.json: HTTP ${r.status}`);
        return r.json() as Promise<EngineData>;
      }),
      fetch('./patches.json').then((r) => (r.ok ? r.json() as Promise<Record<string, string>> : {})),
    ]);
    const layout = makeLayout(layoutData);
    ctx = {
      layout,
      engines: makeCatalogue(engineData, layout),
      patches,
      engineFocus: new Store<EngineFocus>({ engine: null }),
    };
  } catch (e) {
    showError($('#panels')!, new Error(
      `${(e as Error).message}\n\nThis page is generated: run \`make web-data\` and serve web/ over http `
      + '(file:// will not work - the browser blocks the fetch).'));
    return;
  }

  const mounted = new Set<string>();

  function show(name: string): void {
    if (!VIEWS[name] && name !== ENGINE_PANEL) name = DEFAULT_VIEW;
    for (const tab of $$<HTMLButtonElement>('#tabs button')) {
      const selected = tab.dataset.view === name;
      tab.classList.toggle('active', selected);
      tab.setAttribute('aria-selected', String(selected));
      // Roving tabindex: the selected tab is the group's only stop in the page tab order, so Tab
      // moves past the whole row and the arrows move within it.
      tab.tabIndex = selected ? 0 : -1;
    }
    // On the engine page nothing is selected, so the row would have no tab stop at all. Park it on
    // Reference, which is where "all engines" goes.
    if (name === ENGINE_PANEL) {
      const ref = $<HTMLButtonElement>('#tab-reference');
      if (ref) ref.tabIndex = 0;
    }
    for (const panel of $$<HTMLElement>('#panels > section')) panel.hidden = panel.id !== `panel-${name}`;
    if (!mounted.has(name)) {
      mounted.add(name);
      const root = $<HTMLElement>(`#panel-${name}`)!;
      try {
        (name === ENGINE_PANEL ? mountEngine : VIEWS[name])(root, ctx);
      } catch (e) {
        showError(root, e);
      }
    }
    if (parseHash(location.hash).view !== name) history.replaceState(null, '', `#${name}`);
  }

  /** Show one engine's own page, and record it in the URL so the link is shareable. */
  function showEngine(name: string): void {
    show(ENGINE_PANEL);
    // Set after showing, so the view is mounted and subscribed before the name arrives.
    ctx.engineFocus.set({ engine: name });
    if (location.hash !== `#engine/${name}`) history.replaceState(null, '', `#engine/${name}`);
  }

  const tabs = $$<HTMLButtonElement>('#tabs button');
  for (const tab of tabs) {
    tab.addEventListener('click', () => show(tab.dataset.view ?? DEFAULT_VIEW));
  }

  // Selection follows focus, which the authoring practices allow where showing a panel is cheap - and
  // here it is, since a view mounts once and is only shown thereafter.
  $('#tabs')!.addEventListener('keydown', (e) => {
    const ev = e as KeyboardEvent;
    const next = nextTabIndex(ev.key, tabs.indexOf(document.activeElement as HTMLButtonElement),
      tabs.length);
    if (next == null) return;
    ev.preventDefault();
    tabs[next].focus();
    show(tabs[next].dataset.view ?? DEFAULT_VIEW);
  });

  window.addEventListener('hashchange', () => {
    const route = parseHash(location.hash);
    if (route.engine) showEngine(route.engine);
    else show(route.view);
  });

  // The card layout is versioned with the firmware so the rules on this page match the binaries it
  // sits beside; say which, rather than leaving the user to guess whether the page is current.
  const provenance = `${ctx.layout.banks.length} banks, `
    + `scan floor ${ctx.layout.scan.min_bytes / 1024} KB, name limit ${ctx.layout.scan.max_name}`;
  $('#banner')!.append(el('span', { class: 'muted' }, provenance));

  wireAboutMenu(provenance);
  // Home: back to the landing tab, and drop the fragment so the URL is the bare page again.
  $('#home-link')?.addEventListener('click', () => {
    (document.activeElement as HTMLElement | null)?.blur?.();
    history.replaceState(null, '', location.pathname + location.search);
    show(DEFAULT_VIEW);
  });

  buildEngineMenu(ctx, showEngine);
  buildThemeMenu();

  const route = parseHash(location.hash);
  if (route.engine) showEngine(route.engine);
  else show(route.view || DEFAULT_VIEW);
}

/**
 * The Apple menu's one item.
 *
 * A `<dialog>` rather than a div, so Escape and the focus trap come from the platform instead of from
 * three more event listeners. The menu itself needs no JavaScript at all - system.css opens it on
 * `:focus-within` - so this is only the About box.
 */
function wireAboutMenu(provenance: string): void {
  const dialog = $<HTMLDialogElement>('#about');
  const open = $<HTMLButtonElement>('#about-open');
  const close = $<HTMLButtonElement>('#about-close');
  const facts = $('#about-facts');
  if (!dialog || !open || !close) return;
  if (facts) facts.textContent = provenance;

  open.addEventListener('click', () => {
    // Blur first: the menu is held open by focus, so it would stay painted over the dialog.
    open.blur();
    if (typeof dialog.showModal === 'function') dialog.showModal();
    else dialog.setAttribute('open', '');
  });
  close.addEventListener('click', () => dialog.close());
}

/**
 * The Engines menu: every documented engine, in one System 6 menu.
 *
 * Built from `engines.json` rather than written into the markup, for the same reason nothing else on
 * this page is typed twice - an engine that gains a doc appears here on the next `make web-data`, and
 * one that loses its doc disappears rather than becoming a menu item that opens nothing.
 */
function buildEngineMenu(ctx: ViewContext, onPick: (engine: string) => void): void {
  const host = $('#engines-menu');
  if (!host) return;
  host.append(el('ul', { role: 'menu' }, ctx.engines.entries.map((e) =>
    el('li', { role: 'menu-item' },
      el('button', {
        type: 'button',
        onclick: () => {
          // The menu is held open by focus; blur so it closes behind the tab it just switched to.
          (document.activeElement as HTMLElement | null)?.blur?.();
          onPick(e.doc.name);
        },
      }, e.doc.name, e.bank ? '' : el('span', { class: 'muted' }, '  (no card)'))))));
}

/**
 * The View menu: which theme the page wears.
 *
 * Built from the same list `theme.ts` exports, so the menu cannot offer a theme that does not exist
 * or miss one that does. The check mark is drawn as text rather than a glyph - it has to read in a
 * 1-bit theme as well as a colour one.
 */
function buildThemeMenu(): void {
  const host = $('#theme-menu');
  if (!host) return;
  const render = (): void => {
    const active = currentTheme();
    host.querySelector('[role=menu]')?.remove();
    host.append(el('ul', { role: 'menu' }, THEMES.map((t) =>
      el('li', { role: 'menu-item' },
        el('button', {
          type: 'button',
          title: t.note,
          onclick: () => {
            (document.activeElement as HTMLElement | null)?.blur?.();
            applyTheme(t.id);
            render();
          },
        }, `${t.id === active ? '\u2022 ' : '   '}${t.label}`)))));
  };
  render();
}

// Offline support matters for a tool people use standing next to hardware rather than at a desk.
if ('serviceWorker' in navigator && location.protocol === 'https:') {
  navigator.serviceWorker.register('./sw.js').catch(() => {
    /* offline support is a bonus, never a blocker */
  });
}

void main();
