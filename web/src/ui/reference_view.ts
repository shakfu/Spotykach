// reference_view.ts - "what is this engine, and what does it want on the card?"
//
// Both halves of that question, on one screen, because it is one person asking. This started as the
// web counterpart of `python3 scripts/sk_card.py layout` - the ten card banks and their formats - and
// the other twelve engines were a footnote saying they existed. They are now first-class entries with
// their documentation, joined to the card spec where there is one.
//
// Every fact here is generated: the card rules from `scripts/card_layout.py`, the engine descriptions
// from `docs/engines/*.md`, both via `scripts/web_export.py`. Nothing is written down twice, and a
// number typed into this file would be a figure that silently outlives its source.

import { ReferenceModel } from '../app/reference_model.ts';
import { folderLabel } from '../core/layout.ts';
import type { Layout } from '../core/layout.ts';
import type { EngineEntry } from '../core/engines.ts';
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

/** One `label / value` row of an engine's spec table. */
function specRow(label: string, value: string, mono = false, cls?: string): HTMLTableRowElement {
  return el('tr', { class: cls ?? null },
    el('th', {}, label),
    el('td', { class: mono ? 'mono' : null }, value));
}

/**
 * The rules that hold for every scanned folder, stated once at the top instead of repeated per engine.
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

/** One engine's entry: what it is, and - if it reads a card - what it expects to find there. */
function engineSection(layout: Layout, entry: EngineEntry): HTMLElement {
  const { doc, bank } = entry;
  const rows: HTMLTableRowElement[] = [];

  if (bank) {
    rows.push(specRow('Format', bank.fmt.describe));
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
      rows.push(specRow('Length', `about ${seconds(bank.max_seconds)} s at most - this engine loads `
        + 'the whole file into RAM, so anything longer is trimmed'));
    }
    for (const name of bank.sidecars) {
      // "Also needs" is right for a file beside the audio, and wrong for the platform entry, where
      // the config file is not an extra - it is the entire contents of the folder.
      const label = bank.kind === 'config' ? 'File' : 'Also needs';
      const dflt = bank.extras[name];
      // Trimmed: these defaults are single-line and end in a newline, and the escape on screen reads
      // as though it were part of the value.
      rows.push(specRow(label, dflt ? `${name} - defaults to ${JSON.stringify(dflt.trim())}` : name, true));
    }
    if (bank.target) rows.push(specRow('Convert writes', bank.target, true));
    // Only worth a row where it is not the obvious answer: the shared tape store is read by more than
    // one engine, so a reader list of one says nothing a heading has not already said.
    if (bank.readers.length > 1) rows.push(specRow('Read by', bank.readers.join(', ')));
    // Off by default: a citation is the answer to "prove it", which is a developer's question on a
    // screen aimed at somebody filling a card.
    rows.push(specRow('Firmware', bank.source, true, 'src'));
  }

  return el('section', { class: 'ref-bank', dataset: { engine: doc.name } },
    el('h3', {}, doc.name, ' ',
      el('span', { class: 'mono muted' }, bank ? folderLabel(bank.dirs) : 'needs no card')),
    doc.summary && el('p', { class: 'summary' }, doc.summary),
    doc.body && el('p', {}, doc.body),
    el('p', { class: 'muted note' },
      // Saying so matters: `granular` and `passthrough` are documented and deliberately unpublished,
      // so an owner looking for them in a release needs to know that is intentional.
      !doc.released && doc.doc && '(not in the released set) ',
      // In-app, not out to GitHub: the manual is rendered on this site now, so leaving it would be a
      // round trip for a file we already ship. `platform` has no page - it is the shared SK/ folder,
      // not an engine - so it gets no link rather than one pointing at nothing.
      doc.page ? el('a', { href: `#engine/${doc.name}` }, 'Open the manual') : null),
    rows.length ? el('table', { class: 'layout spec' }, el('tbody', {}, rows)) : null,
    bank && bank.kind === 'config' ? configTable(layout) : null);
}

export function mountReference(root: HTMLElement, ctx: ViewContext): void {
  const model = new ReferenceModel(ctx.layout, ctx.engines);
  const status = el('div', { class: 'status' });

  const sections = new Map<string, HTMLElement>(
    ctx.engines.entries.map((e) => [e.doc.name, engineSection(ctx.layout, e)]));

  const filter = el('input', {
    type: 'text',
    class: 'filter',
    placeholder: 'Filter by engine, folder or format',
    autocomplete: 'off',
    oninput: () => model.setQuery(filter.value),
  });

  // Chips filter rather than scrolling to an anchor: a `#granular` href would be caught by main.ts's
  // hashchange handler, which does not know that name and would fall back to the Build tab.
  const chips = el('div', { class: 'chips' }, ctx.engines.entries.map((e) =>
    el('button', { class: 'link', onclick: () => model.toggleChip(e.doc.name) }, e.doc.name)));

  const srcToggle = el('input', {
    type: 'checkbox',
    onchange: () => model.setShowSources(srcToggle.checked),
  });

  const banksEl = el('div', { class: 'ref-banks' }, [...sections.values()]);

  model.store.subscribe((s) => {
    const visible = new Set(model.visible().map((e) => e.doc.name));
    for (const [engine, node] of sections) node.hidden = !visible.has(engine);
    for (const chip of [...chips.children]) {
      chip.classList.toggle('on', chip.textContent === s.pinned);
    }
    if (filter.value !== s.query) filter.value = s.query;
    banksEl.classList.toggle('show-src', s.showSources);
    status.textContent = model.status();
  });

  // The Engines menu and `#engine/<name>` links both land here. Scrolling is deliberate: selecting an
  // engine from a menu at the top of the page should not leave the reader looking at the filter box.
  ctx.engineFocus.subscribe(({ engine }) => {
    if (!engine) return;
    model.select(engine);
    sections.get(engine)?.scrollIntoView?.({ block: 'start' });
  });

  root.append(
    el('p', { class: 'lead' }, 'Every engine, what it does, and what it expects on the card.'),
    el('div', { class: 'controls' }, filter,
      el('label', { class: 'field inline' }, srcToggle, el('span', {}, 'firmware sources'))),
    chips,
    status,
    everywhere(model.scan()),
    banksEl,
    aside('Where these facts come from',
      el('p', {},
        'The card rules are generated from the same table the firmware and the command-line tools '
        + 'read, so this page cannot disagree with ',
        el('code', {}, 'python3 scripts/sk_card.py layout'),
        '. The engine descriptions are the opening paragraph of each ',
        el('code', {}, 'docs/engines/<name>.md'),
        ', so they cannot drift from the documentation either.')),
  );
}
