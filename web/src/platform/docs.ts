// docs.ts - fetching a generated engine page.

import type { DocSource } from '../core/ports.ts';

export const httpDocs: DocSource = {
  async fetchPage(path) {
    const res = await fetch(path);
    if (!res.ok) throw new Error(`cannot load ${path}: HTTP ${res.status}`);
    return res.text();
  },
};
