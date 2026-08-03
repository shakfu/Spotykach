// home_view.ts - the front page.
//
// What a first visit should answer, in order: what is this, what is in it, and what can I do from
// here. Before this existed the page opened straight onto the card builder, which answered the third
// question for one specific task and none of the first two - fine when the page was only card tools,
// wrong now that it is the front door to a firmware project with 22 engines behind it.
//
// The global actions are repeated here as buttons even though every one of them is in a menu. That
// duplication is deliberate and it is the only kind worth having: the menu bar is where an action
// LIVES once you know the tool, and the front page is where it is DISCOVERABLE before you do. Both
// routes call the same `ctx.go`, so there is one implementation and no second code path to drift.

import { el, clear } from './dom.ts';
import type { ViewContext } from './context.ts';

/** Global actions, in the order a person meets them. Ids must exist in main.ts's VIEWS table. */
const ACTIONS: Array<{ view: string; label: string; note: string }> = [
  { view: 'build', label: 'Build a card', note: 'A complete, valid, minimal card in one click.' },
  { view: 'convert', label: 'Convert audio', note: 'Re-encode anything to what an engine reads.' },
  { view: 'verify', label: 'Verify a card', note: 'Check a card and get told exactly what is wrong.' },
  { view: 'flash', label: 'Flash firmware', note: 'Write an engine to the device over USB.' },
];

export function mountHome(root: HTMLElement, ctx: ViewContext): void {
  const cardReaders = ctx.engines.entries.filter((e) => e.bank).length;

  clear(root).append(
    // No <h2> naming the project: the menu bar names it and the window header says which page this
    // is. Three "sk-engines" on one screen told the reader nothing twice.
    el('p', { class: 'lead text-base' },
      'A family of audio engines for the Electrosmith Daisy - each one a separate firmware image you '
      + 'flash to the device, sharing one platform, one control surface and one SD card layout.'),

    // The numbers are DERIVED, never typed: a prose figure is exactly the thing that goes stale the
    // first time an engine is added and nobody thinks to grep for "22".
    el('p', { class: 'muted note max-w-measure' },
      `${ctx.engines.entries.length} engines, ${cardReaders} of which read the SD card. `
      + `${ctx.layout.banks.length} card layouts, `
      + `names up to ${ctx.layout.scan.max_name} characters, `
      + `files from ${ctx.layout.scan.min_bytes / 1024} KB.`),

    el('div', { class: 'action-grid' }, ACTIONS.map((a) =>
      el('button', {
        type: 'button',
        class: 'action-card',
        onclick: () => ctx.go(a.view),
      },
      el('span', { class: 'action-label' }, a.label),
      el('span', { class: 'action-note' }, a.note)))),

    el('h3', {}, 'Browse the engines'),
    el('p', { class: 'max-w-measure' },
      'Every engine has a page: what it does, what it expects on the card, and how to get it onto '
      + 'the device.'),
    el('div', { class: 'controls' },
      el('button', { type: 'button', class: 'primary', onclick: () => ctx.go('engines') },
        `All ${ctx.engines.entries.length} engines`)),

    el('details', { class: 'aside' },
      el('summary', {}, 'Nothing here is uploaded'),
      el('p', {},
        'Every tool on this page runs in this tab. Cards are read and written through the browser\'s '
        + 'own file APIs, and the device is reached over WebSerial and WebUSB - there is no server, '
        + 'no account and no install. The card rules are generated from the same source the '
        + 'command-line tools use, so the two cannot disagree about them.')),
  );
}
