// home_view.ts - the two things the overview needs code for.
//
// The overview's CONTENT is markup, in index.html. This file used to hold all of it as string
// literals passed to el(), which was the wrong place for it twice over: prose is easier to write and
// review as prose, and a document assembled imperatively cannot be seen until the bundle has loaded
// and run. Every other view is built in code because its content comes from a card, a device or the
// layout - this is the one screen that is writing.
//
// So what is left here is exactly what markup cannot do:
//
//   1. the two counts, which must be DERIVED - a number typed into the page is the thing that goes
//      stale the first time an engine is added, and it did;
//   2. the navigation wiring, because a button in static markup has no way to reach the router.
//
// The wiring is generic - any element in the panel carrying `data-view` is routed - so adding an
// action to the overview is a markup edit and nothing else.

import type { ViewContext } from './context.ts';

export function mountHome(root: HTMLElement, ctx: ViewContext): void {
  // NOT cleared: the panel already holds the page. Clearing it here is the mistake this refactor
  // exists to prevent, and it would present as a blank front page rather than an error.

  // Only entries with a rendered page are engines. The catalogue also carries a synthetic entry per
  // card bank that no documented engine reads - the shared `SK/` folder is one - and counting those
  // as engines is how the front page came to claim one more than exists.
  const engines = ctx.engines.entries.filter((e) => e.doc.page);
  const released = engines.filter((e) => e.doc.released).length;
  const cardReaders = engines.filter((e) => e.bank).length;

  const stats = root.querySelector('#home-stats');
  if (stats) {
    stats.textContent =
      `${engines.length} engines in the tree, ${released} of them in the released set. `
      + `${cardReaders} read the SD card; the rest need no card at all. `
      + `${ctx.layout.banks.length} card layouts, names up to ${ctx.layout.scan.max_name} `
      + `characters, files from ${ctx.layout.scan.min_bytes / 1024} KB.`;
  }

  const count = root.querySelector('#home-engine-count');
  if (count) count.textContent = String(engines.length);

  for (const node of root.querySelectorAll('[data-view]')) {
    const view = node.getAttribute('data-view');
    if (view) node.addEventListener('click', () => ctx.go(view));
  }
}
