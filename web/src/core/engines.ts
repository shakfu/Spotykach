// engines.ts - the engine catalogue, joined to the card layout.
//
// Two generated files describe two different things: `card_layout.json` says what a bank expects ON
// THE CARD, and `engines.json` says what an engine IS. Ten engines appear in both, twelve appear only
// in the second because they read nothing from a card at all. Joining them here is what lets one
// screen answer both questions about one engine.

import type { Layout } from './layout.ts';
import type { Bank, EngineData, EngineDoc } from './types.ts';

export const ENGINES_SCHEMA = 1;

export interface EngineEntry {
  doc: EngineDoc;
  /** The bank this engine reads, resolved against the layout. Null for the card-less engines. */
  bank: Bank | null;
}

export interface Catalogue {
  readonly entries: EngineEntry[];
  get(name: string): EngineEntry | undefined;
  /** Engines that read a card, in layout order - the ones with a format to state. */
  readers(): EngineEntry[];
}

export function makeCatalogue(data: EngineData, layout: Layout): Catalogue {
  if (!data || data.schema !== ENGINES_SCHEMA) {
    throw new Error(`engines.json: unsupported schema ${data && data.schema} (expected ${ENGINES_SCHEMA})`);
  }
  const entries: EngineEntry[] = data.engines.map((doc) => ({
    doc,
    bank: doc.bank ? layout.bank(doc.bank) ?? null : null,
  }));

  /*
   * Banks that no documented engine reads still belong here.
   *
   * `platform` is the case that matters and the one that caught this: it is not an engine, it is the
   * shared `SK/` folder holding config.txt and the saved state, so it has no docs/engines/ page - and
   * dropping it took the only listing of the config file's accepted properties with it. A bank with
   * nothing to say for itself borrows the blurb the layout already carries.
   */
  const documented = new Set(entries.filter((e) => e.bank).map((e) => e.bank!.engine));
  for (const bank of layout.banks) {
    if (documented.has(bank.engine)) continue;
    entries.push({
      bank,
      doc: {
        name: bank.engine,
        title: bank.engine,
        summary: '',
        body: bank.blurb,
        source: '',
        doc: '', // no page to link to; the view drops the link rather than pointing at a 404
        page: '', // and no rendered documentation either - there is no markdown to render
        released: false,
        bank: bank.engine,
      },
    });
  }
  const byName = new Map(entries.map((e) => [e.doc.name, e]));
  return {
    entries,
    get: (name) => byName.get(name),
    readers: () => entries.filter((e) => e.bank !== null),
  };
}
