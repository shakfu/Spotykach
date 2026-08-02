// main.ts - bootstrap and tab switching.
//
// Views are mounted lazily, once, on first visit to their tab: the terminal opens a serial port and
// the convert view builds an AudioContext, and neither should happen because the page loaded.

import { makeLayout } from '../core/layout.ts';
import type { LayoutData } from '../core/types.ts';
import { $, $$, el, showError } from './dom.ts';
import { mountBuild } from './build_view.ts';
import { mountConvert } from './convert_view.ts';
import { mountVerify } from './verify_view.ts';
import { mountReference } from './reference_view.ts';
import { mountTerminal } from './terminal_view.ts';
import { nextTabIndex } from './tabs.ts';
import type { MountFn, ViewContext } from './context.ts';

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

async function main(): Promise<void> {
  let ctx: ViewContext;
  try {
    const [layoutData, patches] = await Promise.all([
      fetch('./card_layout.json').then((r) => {
        if (!r.ok) throw new Error(`cannot load ./card_layout.json: HTTP ${r.status}`);
        return r.json() as Promise<LayoutData>;
      }),
      fetch('./patches.json').then((r) => (r.ok ? r.json() as Promise<Record<string, string>> : {})),
    ]);
    ctx = { layout: makeLayout(layoutData), patches };
  } catch (e) {
    showError($('#panels')!, new Error(
      `${(e as Error).message}\n\nThis page is generated: run \`make web-data\` and serve web/ over http `
      + '(file:// will not work - the browser blocks the fetch).'));
    return;
  }

  const mounted = new Set<string>();

  function show(name: string): void {
    if (!VIEWS[name]) name = DEFAULT_VIEW;
    for (const tab of $$<HTMLButtonElement>('#tabs button')) {
      const selected = tab.dataset.view === name;
      tab.classList.toggle('active', selected);
      tab.setAttribute('aria-selected', String(selected));
      // Roving tabindex: the selected tab is the group's only stop in the page tab order, so Tab
      // moves past the whole row and the arrows move within it.
      tab.tabIndex = selected ? 0 : -1;
    }
    for (const panel of $$<HTMLElement>('#panels > section')) panel.hidden = panel.id !== `panel-${name}`;
    if (!mounted.has(name)) {
      mounted.add(name);
      const root = $<HTMLElement>(`#panel-${name}`)!;
      try {
        VIEWS[name](root, ctx);
      } catch (e) {
        showError(root, e);
      }
    }
    if (location.hash.slice(1) !== name) history.replaceState(null, '', `#${name}`);
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

  window.addEventListener('hashchange', () => show(location.hash.slice(1)));

  // The card layout is versioned with the firmware so the rules on this page match the binaries it
  // sits beside; say which, rather than leaving the user to guess whether the page is current.
  $('#banner')!.append(el('span', { class: 'muted' },
    `${ctx.layout.banks.length} banks, scan floor ${ctx.layout.scan.min_bytes / 1024} KB, `
    + `name limit ${ctx.layout.scan.max_name}`));

  show(location.hash.slice(1) || DEFAULT_VIEW);
}

// Offline support matters for a tool people use standing next to hardware rather than at a desk.
if ('serviceWorker' in navigator && location.protocol === 'https:') {
  navigator.serviceWorker.register('./sw.js').catch(() => {
    /* offline support is a bonus, never a blocker */
  });
}

void main();
