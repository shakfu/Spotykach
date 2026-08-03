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
 * `#build` -> the Build view. `#engines` -> the engine catalogue. `#engine/bard` -> bard's own page.
 *
 * Note `#engines` (the grid) and `#engine/<name>` (one engine) differ by a single letter. That is the
 * URL a person would guess for each, so it is worth the near-collision - but it means the `engine`
 * prefix has to be matched EXACTLY rather than by `startsWith`, or the grid would route to a
 * nonexistent engine named "s".
 *
 * An unrecognised fragment returns an empty view and lets the caller fall back, rather than guessing.
 */
export function parseHash(hash: string): Route {
  const raw = hash.replace(/^#/, '');
  if (!raw) return { view: '', engine: null };
  const [head, ...rest] = raw.split('/');
  if (head === 'engine') {
    const engine = rest.join('/').trim();
    // A bare `#engine/` names nobody; send it to the catalogue rather than to a blank engine page.
    return engine ? { view: 'engine', engine } : { view: 'engines', engine: null };
  }
  return { view: head, engine: null };
}
