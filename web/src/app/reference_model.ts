// reference_model.ts - filtering the layout reference, as data rather than as hidden DOM nodes.
//
// The old version put this logic in the view: it toggled `hidden` on sections and asked the DOM what
// was showing. Pulling it out makes the one genuinely subtle rule here testable on its own - a chip is
// a SELECTION and the text box is a SEARCH, and they are separate states because half the engine names
// appear in each other's text (`tape` is in granular's blurb and in shuttle's filenames), so a chip
// that filtered by substring would answer a request for one engine with four.

import type { Layout } from '../core/layout.ts';
import type { Bank } from '../core/types.ts';
import { Store } from './store.ts';

export interface ReferenceState {
  query: string;
  /** The engine selected by a chip, or null when the text box is in charge. */
  pinned: string | null;
  showSources: boolean;
}

/** A bank plus the text the search matches against, computed once. */
export interface ReferenceEntry {
  bank: Bank;
  haystack: string;
}

export class ReferenceModel {
  readonly store = new Store<ReferenceState>({ query: '', pinned: null, showSources: false });
  readonly entries: ReferenceEntry[];

  constructor(private readonly layout: Layout) {
    this.entries = layout.banks.map((bank) => ({
      bank,
      // An engine is findable by its name, its folders, its format or anything its blurb mentions,
      // because "the one that wants raw files" is as likely a starting point as "radio".
      haystack: [
        bank.engine, bank.dirs.join(' '), bank.readers.join(' '), bank.fmt.describe, bank.blurb,
        bank.slots.join(' '), bank.target,
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

  setShowSources(on: boolean): void {
    this.store.set({ showSources: on });
  }

  visible(): Bank[] {
    const { query, pinned } = this.store.get();
    const q = query.trim().toLowerCase();
    return this.entries
      .filter((e) => (pinned ? e.bank.engine === pinned : !q || e.haystack.includes(q)))
      .map((e) => e.bank);
  }

  /** The line under the filter box: a count, and whether it is a subset. */
  status(): string {
    const { query, pinned } = this.store.get();
    const shown = this.visible().length;
    return pinned || query.trim()
      ? `${shown} of ${this.entries.length} shown`
      : `${this.entries.length} folder layouts`;
  }

  scan(): Layout['scan'] {
    return this.layout.scan;
  }
}
