// home_view.ts - the front page.
//
// What a first visit should answer, in order: what is this, what is in it, and what can I do from
// here. Before this existed the page opened straight onto the card builder, which answered the third
// question for one specific task and none of the first two - fine when the page was only card tools,
// wrong now that it is the front door to a firmware project.
//
// The prose here is a precis of the project README, and it has to stay one. An earlier version called
// this "a family of audio engines for the Electrosmith Daisy", which was wrong in the way that
// matters: the Daisy is the module inside, the instrument is Synthux Academy's Spotykach, and the
// point of the project is not that there are many engines but that the PLATFORM is fixed and the
// engine is swappable. Describing the output instead of the idea missed what the fork is for.
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

/** What the platform gives every engine, unchanged. Straight from the README's opening. */
const PLATFORM = [
  'Encoders with pickup behaviour and LED ring feedback',
  'Pad gestures and transport controls',
  'CV and gate I/O, and MIDI',
  'SD-card storage, and a clock every engine can sync to',
];

export function mountHome(root: HTMLElement, ctx: ViewContext): void {
  // Only entries with a rendered page are engines. The catalogue also carries a synthetic entry per
  // card bank that no documented engine reads - the shared `SK/` folder is one - and counting those
  // as engines is how the front page came to claim one more than exists.
  const engines = ctx.engines.entries.filter((e) => e.doc.page);
  const released = engines.filter((e) => e.doc.released).length;
  const cardReaders = engines.filter((e) => e.bank).length;

  clear(root).append(
    // No <h2> naming the project: the menu bar names it and the window header says which page this
    // is. Three "sk-engines" on one screen told the reader nothing twice.
    el('p', { class: 'lead text-base' },
      'A fork of the Synthux Academy ',
      el('a', { href: 'https://synthux.academy/store/spotykach', target: '_blank', rel: 'noreferrer' },
        'Spotykach'),
      ' firmware, restructured so the instrument is a fixed hardware and UI ',
      el('strong', {}, 'platform'),
      ' with a swappable DSP ',
      el('strong', {}, 'engine'),
      '. Each firmware build replaces only the engine and its parameters.'),

    el('p', { class: 'max-w-measure' },
      'The panel does not change when you flash a different engine. The same controls mean the same '
      + 'things, so what you learn once keeps working - and an engine is free to be a looper, an '
      + 'effect, a sampler or a whole scripting language behind it.'),

    // Derived, never typed: a count in prose is exactly the thing that goes stale the first time an
    // engine is added and nobody thinks to grep for the number.
    el('p', { class: 'muted note max-w-measure' },
      `${engines.length} engines in the tree, ${released} of them in the released set. `
      + `${cardReaders} read the SD card; the rest need no card at all. `
      + `${ctx.layout.banks.length} card layouts, names up to ${ctx.layout.scan.max_name} `
      + `characters, files from ${ctx.layout.scan.min_bytes / 1024} KB.`),

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
        `All ${engines.length} engines`)),

    el('details', { class: 'aside' },
      el('summary', {}, 'What stays the same across every engine'),
      el('ul', { class: 'ml-4 list-disc pl-4' }, PLATFORM.map((p) => el('li', {}, p))),
      el('p', {},
        'The platform is decoupled from any engine by construction - the hardware, UI, memory and '
        + 'transport code carries no engine-specific dependency, and a build-time check fails the '
        + 'build if one is introduced.')),

    el('details', { class: 'aside' },
      el('summary', {}, 'Engines are written three ways'),
      el('p', {},
        'In C++ against the engine interface; in Faust, from a .dsp source and a small manifest with '
        + 'no hand-written C++ at all; or in Max/MSP gen~, translated to C++. The generated paths are '
        + 'not toys - the reverb and several others ship from them.')),

    el('details', { class: 'aside' },
      el('summary', {}, 'Nothing here is uploaded'),
      el('p', {},
        'Every tool on this page runs in this tab. Cards are read and written through the browser\'s '
        + 'own file APIs, and the device is reached over WebSerial and WebUSB - there is no server, '
        + 'no account and no install. The card rules are generated from the same source the '
        + 'command-line tools use, so the two cannot disagree about them.')),
  );
}
