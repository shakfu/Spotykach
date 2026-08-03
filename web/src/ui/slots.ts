// slots.ts - how a view finds the places in its markup that it fills.
//
// Every panel's prose lives in index.html and every panel's moving parts are built here, so each view
// needs to say "put this bit there". Three hooks cover it, and there is deliberately no fourth:
//
//   [data-mount]        the one place a view renders its main dynamic block
//   [data-slot="name"]  a named hole for a generated fragment inside otherwise-static prose
//   [data-fill="name"]  a text placeholder - a count, a status word
//
// WHY THE `?? root` FALLBACK. A view mounted against a bare element - which is exactly what the test
// harness does, and what a future embedding might do - renders into that element directly. Without it
// every view would need markup to exist before it could be mounted at all, which would make the views
// untestable in isolation and couple each one to a specific document. With it, the markup is an
// enhancement: it decides WHERE the pieces go, and its absence costs only that.

/** Where a view renders its dynamic content. The root itself when the markup declares no slot. */
export function mountPoint(root: ParentNode & { querySelector: Element['querySelector'] }): HTMLElement {
  return (root.querySelector('[data-mount]') as HTMLElement | null) ?? (root as unknown as HTMLElement);
}

/**
 * A named hole for generated content - falling back to the mount point, never to nothing.
 *
 * The first version returned null when the markup had no such slot, and that was wrong in a way the
 * tests caught immediately: the folder table simply disappeared, with no error anywhere. Content that
 * cannot find its declared home belongs in a worse position, not in no position - a misplaced table is
 * a visible layout bug, a missing one looks like the feature was never built.
 */
export function slot(root: ParentNode & { querySelector: Element['querySelector'] },
                     name: string): HTMLElement {
  return (root.querySelector(`[data-slot="${name}"]`) as HTMLElement | null) ?? mountPoint(root);
}

/** Fill a named text placeholder. A no-op when the markup has none, which keeps views embeddable. */
export function fill(root: ParentNode, name: string, text: string): void {
  const node = root.querySelector(`[data-fill="${name}"]`) as HTMLElement | null;
  if (node) node.textContent = text;
}
