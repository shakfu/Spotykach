// route.ts - the URL fragment, parsed.
//
// Two shapes, and the second is why this is a file rather than three lines inline: `#engine/<name>`
// has to be shareable. Somebody answering "what format does bard want?" should be able to paste a link
// that lands on it, and that is a parsing question with edge cases (an unknown engine, a trailing
// slash, an empty name), which is exactly the kind of thing worth testing without a browser.

export interface Route {
  view: string;
  /** Set only for `#engine/<name>`; the caller decides whether the name is real. */
  engine: string | null;
}

/**
 * `#build` -> the Build tab. `#engine/bard` -> the Reference tab, focused on bard.
 *
 * An unrecognised fragment returns an empty view and lets the caller fall back, rather than guessing.
 */
export function parseHash(hash: string): Route {
  const raw = hash.replace(/^#/, '');
  if (!raw) return { view: '', engine: null };
  const [head, ...rest] = raw.split('/');
  if (head === 'engine') {
    const engine = rest.join('/').trim();
    return engine ? { view: 'reference', engine } : { view: 'reference', engine: null };
  }
  return { view: head, engine: null };
}
