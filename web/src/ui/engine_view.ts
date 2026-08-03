// engine_view.ts - one page per engine: what it is, what it wants on the card, and its full manual.
//
// This replaced a link to GitHub, which is a strange thing for a tool to do about a file sitting in
// its own repository: it costs a round trip, a tab, and the reader's place. The markdown is rendered
// to HTML at export time by `scripts/md2html.py`, so the browser needs no parser and the page needs
// no dependency - the same trade the card rules already make.
//
// Unlike the tabs, this view re-renders: it is one screen showing whichever engine the route names,
// not five screens mounted once.

import { EngineModel } from '../app/engine_model.ts';
import { folderLabel } from '../core/layout.ts';
import type { EngineEntry } from '../core/engines.ts';
import { httpDocs } from '../platform/docs.ts';
import { createLightbox } from './lightbox.ts';
import { append, clear, el } from './dom.ts';
import type { ViewContext } from './context.ts';

/** The card format, in one line, for the summary strip above the documentation. */
function formatLine(entry: EngineEntry): string {
  const bank = entry.bank;
  if (!bank) return 'Reads nothing from the card.';
  return `${folderLabel(bank.dirs)} - ${bank.fmt.describe}`;
}

export function mountEngine(root: HTMLElement, ctx: ViewContext): void {
  const model = new EngineModel(ctx.engines, httpDocs);
  const lightbox = createLightbox();

  // No heading element: the window header carries the engine's name, set by the router from the route
  // itself. Keeping one here put the name on screen twice, a few lines apart.
  const meta = el('p', { class: 'muted note engine-meta' });
  const summary = el('div', { class: 'callout engine-format' });
  const doc = el('div', { class: 'engine-doc' });

  // The ENGINE-SPECIFIC actions. What separates these from the menu bar's is that every one of them
  // needs an engine to be meaningful - and two of the three only apply to the engines that read the
  // card, so they are built per engine rather than declared once.
  //
  // The card-less engines get Flash and nothing else. They are not lesser - they synthesise rather
  // than play back - but offering them a disabled "Convert audio" would be a dead affordance, which
  // is the exact thing the menu bar was pruned of.
  const actions = el('div', { class: 'controls' });

  const renderActions = (entry: EngineEntry | null): void => {
    clear(actions);
    if (!entry) return;
    const name = entry.doc.name;
    actions.append(
      el('button', { type: 'button', class: 'primary', onclick: () => ctx.go('flash') },
        `Flash ${name}`),
    );
    if (entry.bank) {
      actions.append(
        el('button', { type: 'button', onclick: () => ctx.go('convert') }, 'Convert audio for it'),
        el('button', { type: 'button', onclick: () => ctx.go('reference') }, 'Its card layout'),
      );
    }
    actions.append(
      el('button', { type: 'button', onclick: () => ctx.go('engines') }, 'All engines'),
    );
  };

  model.store.subscribe((s) => {
    if (s.error) {
      // The header still shows the name that was asked for; this says it did not resolve.
      renderActions(null);
      clear(meta);
      clear(summary).append(s.error);
      clear(doc);
      return;
    }
    if (!s.entry) return;
    const { doc: info, bank } = s.entry;

    renderActions(s.entry);
    // NOT info.source: that is the doc's own `ENGINE=...` citation line in raw markdown, and the
    // rendered fragment below already contains it, properly formatted. Printing it here showed the
    // same line twice, the first time with its backticks visible.
    append(clear(meta), [!info.released && 'Not in the released set.']);
    append(clear(summary), [
      el('strong', {}, bank ? 'On the card: ' : 'No card needed: '),
      formatLine(s.entry),
      bank && el('span', { class: 'muted' }, '  Full format on the '),
      bank && el('a', { href: '#reference' }, 'Reference tab'),
      bank && '.',
    ]);

    clear(doc);
    if (s.loading) {
      doc.append(el('p', { class: 'muted' }, 'Loading the documentation...'));
      return;
    }
    if (!info.page) {
      doc.append(el('p', { class: 'muted' }, 'This entry is part of the card layout rather than an '
        + 'engine, so it has no manual.'));
      return;
    }
    // innerHTML, deliberately and narrowly.
    //
    // Everywhere else in this app text goes in via textContent, because filenames and device replies
    // are untrusted. This string is different in kind: it is generated at build time by
    // scripts/md2html.py from files in this repository, and that generator escapes every piece of
    // source text before inserting a tag - so a stray `<script>` in a doc arrives as visible text.
    // A test asserts no raw tag survives from source into the committed fragments.
    doc.innerHTML = s.html;
  });

  // Delegated, because the documentation is injected as a blob and its figures do not exist yet when
  // this runs. The generated markup wraps each diagram in a link to itself, which stays as the answer
  // when scripting is unavailable; intercepting it is the enhancement.
  doc.addEventListener('click', (e) => {
    const link = (e.target as Element | null)?.closest?.('figure a') as HTMLAnchorElement | null;
    // Only the link WRAPPING the diagram. The caption carries a download link too, and swallowing
    // that would turn "download PDF" into "open the picture again".
    if (!link || !link.querySelector('img')) return;
    e.preventDefault();
    const figure = link.closest('figure');
    const caption = figure?.querySelector('figcaption');
    lightbox.open(
      link.getAttribute('href') ?? '',
      caption?.firstChild?.textContent?.replace(/ - open full size\s*$/, '').trim() ?? '',
      figure?.querySelector<HTMLAnchorElement>('a.pdf-link')?.getAttribute('href') ?? null);
  });

  ctx.engineFocus.subscribe(({ engine }) => {
    if (engine) void model.show(engine);
  });

  root.append(meta, summary, actions, doc);
}
