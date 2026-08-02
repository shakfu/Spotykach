// engine_model.ts - one engine's page: its documentation, fetched on demand.
//
// Lazy because the rendered docs are 184 KB across 22 engines and nobody reads twenty-two of them.
// Cached because going back to an engine you just looked at should not re-fetch it.

import type { Catalogue, EngineEntry } from '../core/engines.ts';
import type { DocSource } from '../core/ports.ts';
import { Store } from './store.ts';

export interface EngineState {
  entry: EngineEntry | null;
  /** The rendered documentation fragment, or '' while loading or on failure. */
  html: string;
  loading: boolean;
  error: string | null;
}

export class EngineModel {
  readonly store = new Store<EngineState>({
    entry: null, html: '', loading: false, error: null,
  });

  private readonly cache = new Map<string, string>();

  constructor(
    private readonly catalogue: Catalogue,
    private readonly docs: DocSource,
  ) {}

  async show(name: string): Promise<void> {
    const entry = this.catalogue.get(name);
    if (!entry) {
      // A mistyped `#engine/<name>` link. Say which name failed - "not found" without the name is
      // useless when the name came from a URL somebody else sent you.
      this.store.set({ entry: null, html: '', loading: false, error: `No engine called "${name}".` });
      return;
    }

    const cached = this.cache.get(name);
    if (cached !== undefined) {
      this.store.set({ entry, html: cached, loading: false, error: null });
      return;
    }

    // The entry is set before the fetch so the heading and the card format appear immediately; only
    // the prose waits on the network.
    this.store.set({ entry, html: '', loading: true, error: null });
    try {
      const html = await this.docs.fetchPage(entry.doc.page);
      this.cache.set(name, html);
      // Ignore a fetch that finished after the user moved on, or its prose lands under the wrong name.
      if (this.store.get().entry?.doc.name !== name) return;
      this.store.set({ html, loading: false });
    } catch (e) {
      if (this.store.get().entry?.doc.name !== name) return;
      this.store.set({ loading: false, error: (e as Error).message });
    }
  }
}
