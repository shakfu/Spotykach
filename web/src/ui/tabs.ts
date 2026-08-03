// tabs.ts - the keyboard contract that `role="tablist"` promises.
//
// The markup has said `role="tablist"` / `role="tab"` since the first version, and none of what those
// roles mean was implemented. That is worse than using plain buttons: the roles tell assistive tech
// that arrow keys move between tabs and that the group is ONE tab stop, so a screen-reader user is
// handed a contract the page does not honour and the widget appears broken rather than plain.
//
// Two halves. This file is the half with no DOM in it - given a key and where focus is, which tab
// should take it - so it can be tested directly. main.ts owns the other half: moving focus, and the
// roving `tabindex` that makes the tab row a single stop in the page's tab order.

/**
 * Where a keypress should move tab focus, or null if the key is not ours.
 *
 * Wraps at both ends, which is what the ARIA authoring practices specify for a horizontal tablist and
 * what makes a five-tab row feel like a ring rather than a dead end.
 */
export function nextTabIndex(key: string, current: number, count: number): number | null {
  if (count <= 0 || current < 0) return null;
  switch (key) {
    case 'ArrowRight':
      return (current + 1) % count;
    case 'ArrowLeft':
      return (current - 1 + count) % count;
    case 'Home':
      return 0;
    case 'End':
      return count - 1;
    default:
      return null;
  }
}
