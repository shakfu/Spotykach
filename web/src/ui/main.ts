// main.ts - bootstrap and navigation.
//
// Views are mounted lazily, once, on first visit: the terminal opens a serial port and the convert
// view builds an AudioContext, and neither should happen because the page loaded.
//
// Navigation is a MENU BAR, not a tab row. The tabs were right when the page was one tool with six
// screens; they cannot express the split this page now has - global actions that operate on a card or
// a device, and per-engine actions that only make sense once an engine is known. So the global ones
// are grouped in menus by what they act on, the per-engine ones live on the engine's own page, and
// the front page repeats the common global ones as buttons so nothing important is a click away.
//
// VIEWS below is the single source for both the routes and the menus: a menu is generated from the
// same table that resolves the route, so a menu item cannot name a view that does not exist and a
// view cannot quietly become unreachable.

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
import { mountFlash } from './flash_view.ts';
import { mountEngine } from './engine_view.ts';
import { mountHome } from './home_view.ts';
import { mountEngines } from './engines_view.ts';
import { parseHash } from './route.ts';
import { THEMES, applyTheme, currentTheme } from './theme.ts';
import type { EngineFocus, MountFn, ViewContext } from './context.ts';

interface ViewDef {
  mount: MountFn;
  /**
   * The menu label, and the page heading in the window header - deliberately one field, not two.
   *
   * There was a separate `title` when the front page's menu entry said "Home" and its heading said
   * "Overview". Naming the same destination two things is a small lie about the page, and the second
   * field existed only to carry it; renaming the menu entry to Overview retired both.
   */
  label: string;
  /** Which menu it belongs to. Absent = reachable by route only, never listed. */
  menu?: 'card' | 'device';
}

// Declaration order is menu order within each group: get a card, put audio on it, then check it.
// Reference is a lookup rather than a step, so it follows the three card tasks. Terminal is last
// because it needs a firmware build almost nobody has.
const VIEWS: Record<string, ViewDef> = {
  home: { mount: mountHome, label: 'Overview' },
  engines: { mount: mountEngines, label: 'Engines' },
  build: { mount: mountBuild, label: 'Build a card', menu: 'card' },
  convert: { mount: mountConvert, label: 'Convert audio', menu: 'card' },
  verify: { mount: mountVerify, label: 'Verify a card', menu: 'card' },
  reference: { mount: mountReference, label: 'Card reference', menu: 'card' },
  flash: { mount: mountFlash, label: 'Flash firmware', menu: 'device' },
  terminal: { mount: mountTerminal, label: 'Terminal', menu: 'device' },
  // A route, never a menu item: it needs an engine name, which a menu entry has no way to carry.
  engine: { mount: mountEngine, label: 'Engine' },
};

const DEFAULT_VIEW = 'home';
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
      // Replaced with the real router below, once `show` exists. They are not optional in the type
      // because a view may call either at any time after mount, and "sometimes navigation works" is a
      // worse contract than a no-op during the few statements it takes to wire them up.
      go: () => {},
      goEngine: () => {},
    };
  } catch (e) {
    showError($('#panels')!, new Error(
      `${(e as Error).message}\n\nThis page is generated: run \`make web-data\` and serve web/ over http `
      + '(file:// will not work - the browser blocks the fetch).'));
    return;
  }

  const mounted = new Set<string>();

  const pageTitle = $('#page-title');
  const setTitle = (text: string): void => {
    if (pageTitle) pageTitle.textContent = text;
    // The document title moves too, so a browser tab, a bookmark and the history list all say which
    // page they are - the same information, in the three places the browser shows it.
    document.title = `${text} - sk-engines`;
  };

  function show(name: string): void {
    if (!VIEWS[name]) name = DEFAULT_VIEW;
    setTitle(VIEWS[name].label);
    for (const panel of $$<HTMLElement>('#panels > section')) panel.hidden = panel.id !== `panel-${name}`;
    if (!mounted.has(name)) {
      mounted.add(name);
      const root = $<HTMLElement>(`#panel-${name}`)!;
      try {
        VIEWS[name].mount(root, ctx);
      } catch (e) {
        showError(root, e);
      }
    }
    // The front page is the bare URL, not `#home`: it is where an unadorned link should land, and a
    // fragment naming the default would make every shared link carry noise.
    if (name === DEFAULT_VIEW) {
      if (location.hash) history.replaceState(null, '', location.pathname + location.search);
    } else if (parseHash(location.hash).view !== name) {
      history.replaceState(null, '', `#${name}`);
    }
    // A view switch is a page change to a reader who cannot see the layout shift, so move focus to
    // the panel rather than leaving it on a menu item that is now closed.
    $<HTMLElement>(`#panel-${name}`)?.focus?.();
  }

  /** Show one engine's own page, and record it in the URL so the link is shareable. */
  function showEngine(name: string): void {
    show(ENGINE_PANEL);
    // The engine IS the page title here, so it overrides the generic one show() just set. Done even
    // for a name that turns out not to exist: the heading then says what was asked for, and the body
    // says it was not found, which together are more use than a bare "Engine".
    setTitle(name);
    // Set after showing, so the view is mounted and subscribed before the name arrives.
    ctx.engineFocus.set({ engine: name });
    if (location.hash !== `#engine/${name}`) history.replaceState(null, '', `#engine/${name}`);
  }

  ctx.go = show;
  ctx.goEngine = showEngine;

  window.addEventListener('hashchange', () => {
    const route = parseHash(location.hash);
    if (route.engine) showEngine(route.engine);
    else show(route.view);
  });

  // The card layout is versioned with the firmware so the rules on this page match the binaries it
  // sits beside; say which, rather than leaving the user to guess whether the page is current.
  const provenance = `${ctx.layout.banks.length} banks, `
    + `scan floor ${ctx.layout.scan.min_bytes / 1024} KB, name limit ${ctx.layout.scan.max_name}`;

  wireAboutMenu(provenance);
  // Home: back to the front page, and drop the fragment so the URL is the bare page again.
  $('#home-link')?.addEventListener('click', () => {
    (document.activeElement as HTMLElement | null)?.blur?.();
    show(DEFAULT_VIEW);
  });
  // The Engines label is a destination as well as a menu: clicking it opens the catalogue. Without
  // this the only way to reach the grid was a button on the front page, so anyone already deeper in
  // the app had to go home first to browse.
  $('#engines-link')?.addEventListener('click', () => {
    // The menu is held open by focus; blur so it closes behind the page it just opened.
    (document.activeElement as HTMLElement | null)?.blur?.();
    show('engines');
  });

  buildEngineMenu(ctx, showEngine);
  buildActionMenu('#card-menu', 'card', show);
  buildActionMenu('#device-menu', 'device', show);
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
 * One of the global-action menus, generated from the VIEWS table.
 *
 * Generated rather than written into the markup for the reason nothing else here is typed twice: a
 * hand-written menu can name a view that was renamed, or silently omit one that was added, and both
 * failures are invisible until somebody looks for the missing item.
 */
function buildActionMenu(sel: string, group: 'card' | 'device', go: (view: string) => void): void {
  const host = $(sel);
  if (!host) return;
  const items = Object.entries(VIEWS).filter(([, v]) => v.menu === group);
  host.append(el('ul', { role: 'menu' }, items.map(([id, v]) =>
    el('li', { role: 'menu-item' },
      el('button', {
        type: 'button',
        onclick: () => {
          // The menu is held open by focus; blur so it closes behind the view it just opened.
          (document.activeElement as HTMLElement | null)?.blur?.();
          go(id);
        },
      }, v.label)))));
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
