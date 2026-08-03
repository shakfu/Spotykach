// engines_view.ts - the engine catalogue, as a grid of cards.
//
// The Engines dropdown lists 22 names and nothing else, which is a fine way to reach an engine you can
// already name and a useless way to find one you cannot. This is the browsable half: a card per
// engine, its name and what it does, clicking through to its own page.
//
// THE DESCRIPTION PROBLEM. `engines.json` carries a `summary` field, and only 6 of the 22 engines have
// one - it is the tail of an em-dash heading (`# Delay - tempo-synced ...`) and most docs simply do
// not write their title that way. The generator says as much in its own comment: "Often empty: only
// some headings carry an em-dash tail. The body is the real description." So the fallback is `body`,
// the first real paragraph of the manual, which every engine has.
//
// Body is markdown, and it is trimmed to one sentence here rather than at the generator. That is a
// deliberate split: `web_export.py` stays a faithful extractor of what the docs say, and the decision
// about how much of it fits on a card stays with the thing that draws the card. Fixing it upstream
// would mean 16 doc headings rewritten to a house style, which is a docs change wearing a UI change's
// clothes - worth doing, but not as a side effect of building a grid.

import { el, clear } from './dom.ts';
import type { ViewContext } from './context.ts';
import type { EngineEntry } from '../core/engines.ts';

/** How much prose fits on a card before it stops being a summary and starts being the manual. */
const MAX_CHARS = 190;

/**
 * Inline markdown to plain text: `**bold**`, `_em_`, `` `code` ``, `[text](url)`.
 *
 * Deliberately not a markdown parser. This runs on one paragraph of known-shape prose to produce a
 * card subtitle; anything that survives is text, which is the correct outcome for a subtitle even
 * when the stripping is imperfect.
 */
export function plainText(md: string): string {
  return md
    .replace(/!\[[^\]]*\]\([^)]*\)/g, '')      // images: nothing useful in a one-line summary
    .replace(/\[([^\]]*)\]\([^)]*\)/g, '$1')   // links: keep the text, drop the target
    .replace(/`([^`]*)`/g, '$1')
    .replace(/\*\*([^*]*)\*\*/g, '$1')
    .replace(/(?<![*\w])\*([^*]+)\*(?!\w)/g, '$1')
    .replace(/(?<![_\w])_([^_]+)_(?!\w)/g, '$1')
    .replace(/\s+/g, ' ')
    .trim();
}

/**
 * One line describing an engine: its summary if it has one, else the opening of its manual.
 *
 * Cuts at a sentence end where there is one within budget, because a description ending mid-clause
 * reads as truncation whereas a short complete sentence reads as a summary. Falls back to a word
 * boundary and an ellipsis, never a hard slice through a word.
 */
export function describe(entry: EngineEntry, max = MAX_CHARS): string {
  const summary = entry.doc.summary?.trim();
  if (summary) return plainText(summary);

  const body = plainText(entry.doc.body ?? '');
  if (!body) return '';
  if (body.length <= max) return body;

  // A sentence end is `. ` followed by a capital - not a bare period, which would cut at "48 kHz."
  // inside a sentence, or at an abbreviation.
  const window = body.slice(0, max + 1);
  const sentence = [...window.matchAll(/\.\s+(?=[A-Z])/g)].pop();
  if (sentence && sentence.index > max * 0.4) return window.slice(0, sentence.index + 1);

  const cut = window.lastIndexOf(' ');
  return `${window.slice(0, cut > 0 ? cut : max).replace(/[,;:\s]+$/, '')}...`;
}

export function mountEngines(root: HTMLElement, ctx: ViewContext): void {
  // Engines only. The catalogue also carries a synthetic entry for every card bank no documented
  // engine reads - the shared `SK/` config folder is one - which belongs in the card Reference, not
  // in a grid of instruments. They are recognisable by having no rendered page.
  const entries = ctx.engines.entries.filter((e) => e.doc.page);

  clear(root).append(
    // The window header already says "Engines"; repeating it here is a heading that adds nothing.
    el('p', { class: 'lead text-base' },
      'One firmware image each: flash the one you want, and the device becomes that instrument. '
      + 'Pick one to see what it does and what it expects on the card.'),

    el('div', { class: 'engine-grid' }, entries.map((e) => {
      const name = e.doc.name;
      // A real link, not a div with a click handler: it has to be middle-clickable, copyable and
      // reachable by keyboard, and `#engine/<name>` is already the shareable route for exactly this.
      return el('a', {
        class: 'engine-card',
        href: `#engine/${name}`,
        onclick: (ev: Event) => {
          // Left-click routes in-page; anything with a modifier is left to the browser so
          // open-in-new-tab keeps working.
          //
          // `button` is checked only when PRESENT. A synthetic click - a test, or an assistive
          // technology activating the link - carries no button, and treating absent as "not the
          // primary button" would silently refuse to navigate for both.
          const m = ev as MouseEvent;
          if (m.metaKey || m.ctrlKey || m.shiftKey) return;
          if (m.button != null && m.button !== 0) return;
          ev.preventDefault();
          ctx.goEngine(name);
        },
      },
      el('span', { class: 'engine-card-title' }, e.doc.title || name),
      // Said once, on the cards that need it. The 12 card-less engines are not lesser - they simply
      // synthesise rather than play back - but "do I need to prepare an SD card for this?" is the
      // first practical question about any of them, and it is answerable here.
      e.bank
        ? el('span', { class: 'engine-card-tag' }, 'reads the card')
        : el('span', { class: 'engine-card-tag muted' }, 'no card needed'),
      el('span', { class: 'engine-card-desc' }, describe(e)));
    })),
  );
}
