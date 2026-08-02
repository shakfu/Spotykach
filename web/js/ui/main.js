// main.js - bootstrap and tab switching.
//
// Views are mounted lazily, once, on first visit to their tab: the terminal opens a serial port and
// the convert view builds an AudioContext, and neither should happen because the page loaded.

import { $, $$, el, showError } from './dom.js';
import { loadLayout } from '../layout.js';
import { mountVerify } from './verify_view.js';
import { mountBuild } from './build_view.js';
import { mountConvert } from './convert_view.js';
import { mountReference } from './reference_view.js';
import { mountTerminal } from './terminal_view.js';

// Declaration order is tab order; the first entry is what a fresh visit lands on. Build comes first
// because the entry state for someone who just bought a device is "I have no card yet", and Verify has
// nothing useful to say to that. Reference is a lookup rather than a step, so it follows the three task
// tabs; Terminal is last because it needs a firmware build almost nobody has.
const VIEWS = {
  build: mountBuild,
  convert: mountConvert,
  verify: mountVerify,
  reference: mountReference,
  terminal: mountTerminal,
};

const DEFAULT_VIEW = Object.keys(VIEWS)[0];

async function main() {
  const ctx = {
    layout: null,
    patches: {},
    card: null,
    setCard(card) {
      this.card = card;
    },
  };

  try {
    const [layout, patches] = await Promise.all([
      loadLayout('./card_layout.json'),
      fetch('./patches.json').then((r) => (r.ok ? r.json() : {})),
    ]);
    ctx.layout = layout;
    ctx.patches = patches;
  } catch (e) {
    showError($('#panels'), new Error(
      `${e.message}\n\nThis page is generated: run \`make web-data\` and serve web/ over http `
      + '(file:// will not work - the browser blocks the fetch).'));
    return;
  }

  const mounted = new Set();

  function show(name) {
    if (!VIEWS[name]) name = DEFAULT_VIEW;
    for (const tab of $$('#tabs button')) {
      tab.classList.toggle('active', tab.dataset.view === name);
      tab.setAttribute('aria-selected', String(tab.dataset.view === name));
    }
    for (const panel of $$('#panels > section')) panel.hidden = panel.id !== `panel-${name}`;
    if (!mounted.has(name)) {
      mounted.add(name);
      const root = $(`#panel-${name}`);
      try {
        VIEWS[name](root, ctx);
      } catch (e) {
        showError(root, e);
      }
    }
    if (location.hash.slice(1) !== name) history.replaceState(null, '', `#${name}`);
  }

  for (const tab of $$('#tabs button')) {
    tab.addEventListener('click', () => show(tab.dataset.view));
  }
  window.addEventListener('hashchange', () => show(location.hash.slice(1)));

  // The card layout is versioned with the firmware so the rules on this page match the binaries it
  // sits beside; say which, rather than leaving the user to guess whether the page is current.
  $('#banner').append(el('span', { class: 'muted' },
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

main();
