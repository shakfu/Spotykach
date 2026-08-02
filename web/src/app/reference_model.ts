// reference_model.ts - filtering the engine reference, as data rather than as hidden DOM nodes.
//
// This screen used to list the ten CARD BANKS. It now lists every engine that has documentation - all
// twenty-two - because "what is the shuttle engine" and "what does shuttle expect on the card" are the
// same question asked by the same person, and only ten engines could answer the second one. The twelve
// that read no card said nothing at all; they were a sentence in a footnote saying they existed.
//
// The one genuinely subtle rule here is testable because it lives outside the view: a chip is a
// SELECTION and the text box is a SEARCH, and they are separate states because half the engine names
// appear in each other's text (`tape` is in granular's blurb and in shuttle's filenames), so a chip
// that filtered by substring would answer a request for one engine with four.

import type { Catalogue, EngineEntry } from '../core/engines.ts';
import type { Layout } from '../core/layout.ts';
import { Store } from './store.ts';

export interface ReferenceState {
  query: string;
  /** The engine selected by a chip or by the Engines menu, or null when the text box is in charge. */
  pinned: string | null;
  showSources: boolean;
}

/** An engine plus the text the search matches against, computed once. */
export interface ReferenceItem {
  entry: EngineEntry;
  haystack: string;
}

export class ReferenceModel {
  readonly store = new Store<ReferenceState>({ query: '', pinned: null, showSources: false });
  readonly items: ReferenceItem[];

  constructor(private readonly layout: Layout, catalogue: Catalogue) {
    this.items = catalogue.entries.map((entry) => ({
      entry,
      // An engine is findable by its name, its folders, its format or anything its description
      // mentions, because "the one that wants raw files" is as likely a starting point as "radio".
      haystack: [
        entry.doc.name, entry.doc.title, entry.doc.summary, entry.doc.body,
        entry.bank ? [
          entry.bank.dirs.join(' '), entry.bank.readers.join(' '),
          entry.bank.fmt.describe, entry.bank.blurb, entry.bank.slots.join(' '), entry.bank.target,
        ].join(' ') : 'no card needed',
      ].join(' ').toLowerCase(),
    }));
  }

  setQuery(query: string): void {
    // Typing releases a pinned chip: the search the user is now performing wins over the selection
    // they made a moment ago, and leaving both active shows a stale intersection of the two.
    this.store.set({ query, pinned: null });
  }

  toggleChip(engine: string): void {
    const pinned = this.store.get().pinned === engine ? null : engine;
    this.store.set({ pinned, query: '' });
  }

  /** Select one engine outright - what the Engines menu and an `#engine/<name>` link both do. */
  select(engine: string): void {
    if (!this.items.some((i) => i.entry.doc.name === engine)) return;
    this.store.set({ pinned: engine, query: '' });
  }

  setShowSources(on: boolean): void {
    this.store.set({ showSources: on });
  }

  visible(): EngineEntry[] {
    const { query, pinned } = this.store.get();
    const q = query.trim().toLowerCase();
    return this.items
      .filter((i) => (pinned ? i.entry.doc.name === pinned : !q || i.haystack.includes(q)))
      .map((i) => i.entry);
  }

  /** The line under the filter box: a count, and whether it is a subset. */
  status(): string {
    const { query, pinned } = this.store.get();
    const shown = this.visible().length;
    const readers = this.items.filter((i) => i.entry.bank).length;
    return pinned || query.trim()
      ? `${shown} of ${this.items.length} shown`
      : `${this.items.length} engines, ${readers} of them read the card`;
  }

  scan(): Layout['scan'] {
    return this.layout.scan;
  }
}
