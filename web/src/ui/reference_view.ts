// reference_view.ts - "what does this engine expect on the card?"
//
// The web counterpart of `python3 scripts/sk_card.py layout`, which was the one subcommand with no
// screen. The other three tabs each need something from the user - a card, some audio, a device - and
// answer a question about *their* card. This one answers a question about the *firmware*, so it needs
// nothing, works in every browser, and is the tab you leave open while using the others.
//
// Every fact rendered here is a lookup into card_layout.json. Nothing is written down: the moment a
// number in this file disagrees with scripts/card_layout.py it is wrong, and prose does not have a test.

import { ReferenceModel } from '../app/reference_model.ts';
import { folderLabel } from '../core/layout.ts';
import type { Layout } from '../core/layout.ts';
import type { Bank } from '../core/types.ts';
import { aside, el } from './dom.ts';
import type { ViewContext } from './context.ts';

/** `[a, b]` -> `a-b`, for the config property ranges. */
const range = ([lo, hi]: [number, number]): string => (lo === hi ? String(lo) : `${lo}-${hi}`);

/** `['raw','wav']` -> `.raw/.wav`, the way the CLI and the folder READMEs write it. */
const extList = (exts: string[]): string => exts.map((e) => `.${e}`).join('/');

/**
 * The seconds cap, kept exact rather than rounded.
 *
 * `sk_card.py layout` prints `~11 s` for softcut because a terminal line wants to be short. Here there
 * is room, and 10.9 is the number in the engine's own documentation - rounding it just invites someone
 * to wonder which of the two is the real limit.
 */
const seconds = (s: number): string => (Number.isInteger(s) ? String(s) : s.toFixed(1));

/**
 * Slot names, abbreviated the way `sk_card.py layout` abbreviates them.
 *
 * Three banks name sixteen files each, and printing all forty-eight was a third of everything on this
 * screen - for a list whose interesting part is the pattern, not the sixteenth entry. The full set
 * stays in the title attribute, and the pattern is legible from five.
 */
const slotList = (slots: string[], shown = 5): string =>
  (slots.length <= shown ? slots.join(', ')
    : `${slots.slice(0, shown).join(', ')} ... +${slots.length - shown} more`);

/** One `label / value` row of a bank's spec table. */
function specRow(label: string, value: string, mono = false, cls?: string): HTMLTableRowElement {
  return el('tr', { class: cls ?? null },
    el('th', {}, label),
    el('td', { class: mono ? 'mono' : null }, value));
}

/**
 * The rules that hold for every scanned folder, stated once at the top instead of repeated per bank.
 * These are the ones that fail *silently* on the hardware - a file over the name limit is not rejected,
 * it is simply never indexed - so they are the reason this screen exists.
 */
function everywhere(scan: Layout['scan']): HTMLElement {
  return el('div', { class: 'callout' },
    el('strong', {}, 'Every scanned folder: '),
    `at most ${scan.max_name} characters in the name, ending ${extList(scan.extensions)}, `
    + `at least ${scan.min_bytes / 1024} KB. `,
    el('span', { class: 'muted' },
      'Break any of these and the file is skipped silently; get the format wrong and it plays as '
      + 'noise. Verify finds both.'));
}

/** The platform config file's accepted keys, which exist nowhere else in the UI. */
function configTable(layout: Layout): HTMLElement {
  return el('table', { class: 'layout' },
    el('thead', {}, el('tr', {}, el('th', {}, 'Property'), el('th', {}, 'Range'))),
    el('tbody', {}, Object.entries(layout.configProperties).map(([k, v]) =>
      el('tr', {}, el('td', { class: 'mono' }, k), el('td', { class: 'mono' }, range(v))))));
}

/** One engine's full entry: the folders, the format, and every constraint that applies to it. */
function bankSection(layout: Layout, bank: Bank): HTMLElement {
  const rows: HTMLTableRowElement[] = [specRow('Format', bank.fmt.describe)];

  if (bank.slots.length) {
    const row = specRow(`Names (${bank.slots.length})`, slotList(bank.slots), true);
    row.querySelector('td')!.title = bank.slots.join(', ');
    rows.push(row);
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
  // Off by default: a citation like `src/engine/tape/tape_engine.cpp:397` is the answer to "prove it",
  // which is a developer's question on a screen aimed at somebody filling a card.
  rows.push(specRow('Firmware', bank.source, true, 'src'));

  return el('section', { class: 'ref-bank', dataset: { engine: bank.engine } },
    el('h3', {}, bank.engine, ' ', el('span', { class: 'mono muted' }, folderLabel(bank.dirs))),
    bank.blurb && el('p', { class: 'muted' }, bank.blurb),
    el('table', { class: 'layout spec' }, el('tbody', {}, rows)),
    bank.kind === 'config' && configTable(layout));
}

export function mountReference(root: HTMLElement, ctx: ViewContext): void {
  const model = new ReferenceModel(ctx.layout);
  const status = el('div', { class: 'status' });

  const sections = new Map<string, HTMLElement>(
    ctx.layout.banks.map((b) => [b.engine, bankSection(ctx.layout, b)]));

  const filter = el('input', {
    type: 'text',
    class: 'filter',
    placeholder: 'Filter by engine, folder or format',
    autocomplete: 'off',
    oninput: () => model.setQuery(filter.value),
  });

  // Chips filter rather than scrolling to an anchor: a `#granular` href would be caught by main.ts's
  // hashchange handler, which does not know that name and would fall back to the Build tab.
  const chips = el('div', { class: 'chips' }, ctx.layout.banks.map((b) =>
    el('button', { class: 'link', onclick: () => model.toggleChip(b.engine) }, b.engine)));

  const srcToggle = el('input', {
    type: 'checkbox',
    onchange: () => model.setShowSources(srcToggle.checked),
  });

  const banksEl = el('div', { class: 'ref-banks' }, [...sections.values()]);

  model.store.subscribe((s) => {
    const visible = new Set(model.visible().map((b) => b.engine));
    for (const [engine, node] of sections) node.hidden = !visible.has(engine);
    for (const chip of [...chips.children]) {
      chip.classList.toggle('on', chip.textContent === s.pinned);
    }
    if (filter.value !== s.query) filter.value = s.query;
    banksEl.classList.toggle('show-src', s.showSources);
    status.textContent = model.status();
  });

  root.append(
    el('p', { class: 'lead' }, 'What each engine expects on the card.'),
    el('div', { class: 'controls' }, filter,
      el('label', { class: 'field inline' }, srcToggle, el('span', {}, 'firmware sources'))),
    chips,
    status,
    everywhere(model.scan()),
    banksEl,
    aside('Where are the other engines?',
      el('p', {},
        'An engine not listed here reads nothing from the card and needs no folder at all - most of the '
        + 'effects are in that group. Everything above is generated from the same table the firmware '
        + 'and the command-line tools read, so it is the same content as ',
        el('code', {}, 'python3 scripts/sk_card.py layout'),
        '.')),
  );
}
