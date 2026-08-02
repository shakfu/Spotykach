// reference_view.js - "what does this engine expect on the card?"
//
// The web counterpart of `python3 scripts/sk_card.py layout`, which was the one subcommand with no
// screen. The other three tabs each need something from the user - a card, some audio, a device - and
// answer a question about *their* card. This one answers a question about the *firmware*, so it needs
// nothing, works in every browser, and is the tab you leave open while using the others.
//
// Every fact rendered here is a lookup into card_layout.json. Nothing is written down: the moment a
// number in this file disagrees with scripts/card_layout.py it is wrong, and prose does not have a test.

import { el } from './dom.js';
import { folderLabel } from '../layout.js';

/** `[a, b]` -> `a-b`, for the config property ranges. */
const range = ([lo, hi]) => (lo === hi ? String(lo) : `${lo}-${hi}`);

/** `['raw','wav']` -> `.raw/.wav`, the way the CLI and the folder READMEs write it. */
const extList = (exts) => exts.map((e) => `.${e}`).join('/');

/**
 * The seconds cap, kept exact rather than rounded.
 *
 * `sk_card.py layout` prints `~11 s` for softcut because a terminal line wants to be short. Here there
 * is room, and 10.9 is the number in the engine's own documentation - rounding it just invites someone
 * to wonder which of the two is the real limit.
 */
const seconds = (s) => (Number.isInteger(s) ? String(s) : s.toFixed(1));

/** One `label / value` row of a bank's spec table. */
const specRow = (label, value, mono = false) =>
  el('tr', {}, el('th', {}, label), el('td', { class: mono ? 'mono' : null }, value));

/**
 * The rules that hold for every scanned folder, stated once at the top instead of repeated per bank.
 * These are the ones that fail *silently* on the hardware - a file over the name limit is not rejected,
 * it is simply never indexed - so they are the reason this screen exists.
 */
function everywhere(layout) {
  const { scan } = layout;
  return el('div', { class: 'callout' },
    el('strong', {}, 'True of every scanned folder: '),
    `filenames of at most ${scan.max_name} characters, ending ${extList(scan.extensions)}, `
    + `at least ${scan.min_bytes / 1024} KB of audio. `,
    el('span', { class: 'muted' },
      'A file that breaks any of these is skipped by the directory scan with nothing shown on the '
      + 'device, and a file in the wrong format is not rejected either - it is read as raw bytes and '
      + 'plays as noise. The Verify tab finds both.'));
}

/** The platform config file's accepted keys, which exist nowhere else in the UI. */
function configTable(layout) {
  return el('table', { class: 'layout' },
    el('thead', {}, el('tr', {}, el('th', {}, 'Property'), el('th', {}, 'Range'))),
    el('tbody', {}, Object.entries(layout.configProperties).map(([k, v]) =>
      el('tr', {}, el('td', { class: 'mono' }, k), el('td', { class: 'mono' }, range(v))))));
}

/** One engine's full entry: the folders, the format, and every constraint that applies to it. */
function bankSection(layout, bank) {
  const rows = [specRow('Format', bank.fmt.describe)];

  if (bank.slots.length) {
    rows.push(specRow(`Names (${bank.slots.length})`, bank.slots.join(', '), true));
  }
  if (bank.scanned) {
    rows.push(specRow('Scanned', `any name of at most ${layout.scan.max_name} characters ending `
      + `${extList(layout.scan.extensions)}, at least ${layout.scan.min_bytes / 1024} KB`
      + `${bank.max_files ? `, at most ${bank.max_files} per folder` : ''}`));
  }
  if (bank.max_seconds) {
    rows.push(specRow('Length', `about ${seconds(bank.max_seconds)} s at most - this engine loads the `
      + 'whole file into RAM, so anything longer is trimmed'));
  }
  for (const name of bank.sidecars) {
    // "Also needs" is right for a file beside the audio, and wrong for the platform entry, where the
    // config file is not an extra - it is the entire contents of the folder.
    const label = bank.kind === 'config' ? 'File' : 'Also needs';
    const dflt = bank.extras[name];
    // Trimmed: these defaults are single-line and end in a newline, and `"48000\n"` on screen reads
    // as though the escape were part of the value.
    rows.push(specRow(label, dflt ? `${name} - defaults to ${JSON.stringify(dflt.trim())}` : name, true));
  }
  if (bank.target) rows.push(specRow('Convert writes', bank.target, true));
  // Only worth a row where it is not the obvious answer: SK/{B,G,P,R,T,Y} is the platform's shared tape
  // store, so a reader list of one engine says nothing a heading has not already said.
  if (bank.readers.length > 1) rows.push(specRow('Read by', bank.readers.join(', ')));
  rows.push(specRow('Firmware', bank.source, true));

  const section = el('section', {
    class: 'ref-bank',
    dataset: { engine: bank.engine },
  },
  el('h3', {}, bank.engine, ' ', el('span', { class: 'mono muted' }, folderLabel(bank.dirs))),
  bank.blurb && el('p', { class: 'muted' }, bank.blurb),
  el('table', { class: 'layout spec' }, el('tbody', {}, rows)),
  bank.kind === 'config' && configTable(layout));

  // The haystack the filter matches against, built once: an engine is findable by its name, its
  // folders, its format or anything its blurb mentions, because "the one that wants raw files" is as
  // likely a starting point as "radio".
  section.dataset.search = [
    bank.engine, bank.dirs.join(' '), bank.readers.join(' '), bank.fmt.describe, bank.blurb,
    bank.slots.join(' '), bank.target,
  ].join(' ').toLowerCase();
  return section;
}

export function mountReference(root, ctx) {
  const { layout } = ctx;
  const sections = layout.banks.map((b) => bankSection(layout, b));
  const status = el('div', { class: 'status' });

  // A chip means "this engine", the text box means "anything mentioning this". Keeping them as two
  // states rather than having the chip type into the box matters: half the engine names appear in each
  // other's text - `tape` is in granular's blurb and in shuttle's own filenames - so a chip that
  // filtered by substring would answer a request for one engine with four.
  let pinned = null;

  const filter = el('input', {
    type: 'text',
    class: 'filter',
    placeholder: 'Filter by engine, folder or format',
    autocomplete: 'off',
    oninput: () => {
      pinned = null;
      apply();
    },
  });

  function apply() {
    const q = filter.value.trim().toLowerCase();
    let shown = 0;
    for (const s of sections) {
      const hit = pinned ? s.dataset.engine === pinned : !q || s.dataset.search.includes(q);
      s.hidden = !hit;
      if (hit) shown++;
    }
    for (const chip of chips.children) chip.classList.toggle('on', chip.textContent === pinned);
    status.textContent = pinned || q
      ? `${shown} of ${sections.length} shown`
      : `${sections.length} folder layouts`;
  }

  // Chips filter rather than scrolling to an anchor: a `#granular` href would be caught by main.js's
  // hashchange handler, which does not know that name and would fall back to the Build tab.
  const chips = el('div', { class: 'chips' }, layout.banks.map((b) =>
    el('button', {
      class: 'link',
      onclick: () => {
        pinned = pinned === b.engine ? null : b.engine;
        filter.value = '';
        apply();
      },
    }, b.engine)));

  root.append(
    el('p', { class: 'lead' },
      'What every engine expects on the card, generated from the same table the firmware and the '
      + 'command-line tools read. Nothing here needs a card, a browser permission or a device - it is '
      + 'the reference to keep open while using the other tabs. Same content as ',
      el('code', {}, 'python3 scripts/sk_card.py layout'),
      '. An engine that is not listed here reads nothing from the card and needs no folder at all - '
      + 'most of the effects are in that group.'),
    everywhere(layout),
    el('div', { class: 'controls' }, filter),
    chips,
    status,
    el('div', { class: 'ref-banks' }, sections),
  );
  apply();
}
