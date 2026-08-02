// dom.ts - the twenty lines of helper that make the rest of the UI readable without a framework.
//
// The only module outside src/ui/ that is allowed to know the DOM exists is src/platform/. Everything
// here is presentation; no rule, no state and no decision about what the card means lives in this
// directory.

export const $ = <T extends Element = Element>(sel: string, root: ParentNode = document): T | null =>
  root.querySelector<T>(sel);

export const $$ = <T extends Element = Element>(sel: string, root: ParentNode = document): T[] =>
  [...root.querySelectorAll<T>(sel)];

export type Child = Node | string | number | false | null | undefined | Child[];

export interface Props {
  class?: string | null;
  dataset?: Record<string, string>;
  [key: string]: unknown;
}

/**
 * el('div', {class: 'row'}, 'text', el('b', {}, 'bold'))
 *
 * Text is always set via textContent, never innerHTML: findings, filenames and device replies are all
 * untrusted strings, and a card whose filename contains markup should not be able to do anything with
 * that fact.
 */
export function el<K extends keyof HTMLElementTagNameMap>(
  tag: K, props: Props = {}, ...children: Child[]
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (v == null || v === false) continue;
    if (k === 'class') node.className = String(v);
    else if (k === 'dataset') Object.assign(node.dataset, v);
    else if (k.startsWith('on') && typeof v === 'function') {
      node.addEventListener(k.slice(2), v as EventListener);
    } else if (k in node && k !== 'list') (node as unknown as Record<string, unknown>)[k] = v;
    else node.setAttribute(k, String(v));
  }
  append(node, children);
  return node;
}

export function append(node: Node, children: Child[]): void {
  for (const c of children) {
    if (c == null || c === false) continue;
    // Hand-rolled rather than `.flat(Infinity)`: the depth argument makes TypeScript expand `Child`
    // recursively and it gives up with "type instantiation is excessively deep".
    if (Array.isArray(c)) append(node, c);
    else node.appendChild(c instanceof Node ? c : document.createTextNode(String(c)));
  }
}

export function clear<T extends Element>(node: T): T {
  node.replaceChildren();
  return node;
}

/**
 * Collapsed explanation: `aside('Why 32 kHz?', 'because ...')`.
 *
 * Every rule this app enforces has a reason, and the reasons are worth keeping - a user who hits the
 * 12-character filename limit needs to know it is the firmware's directory scan, not a whim. But that
 * material was being printed above the controls, so the landing tab asked for 121 words of reading
 * before offering two buttons. Folding it away keeps the answer one click from the question it
 * answers, and keeps it out of the way of everyone who did not ask.
 */
export function aside(summary: string, ...children: Child[]): HTMLElement {
  return el('details', { class: 'aside' }, el('summary', {}, summary), ...children);
}

/** Bytes, in the units a person reading a card thinks in. */
export function humanBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / (1024 * 1024)).toFixed(1)} MB`;
}

/**
 * A blocking confirm for the destructive verbs. Deliberately a real prompt rather than a toast:
 * docs/dev/terminal-target-b.md flags that sweeping controls can clear a recorded buffer or write the
 * card, so these must not happen behind one click.
 */
export function confirmDestructive(what: string): boolean {
  return window.confirm(`${what}\n\nThis changes state on the device and cannot be undone from here. Continue?`);
}

/** Wire drag-and-drop on `node`, calling `onDrop(DataTransfer)`. Returns a teardown function. */
export function dropTarget(node: HTMLElement, onDrop: (dt: DataTransfer) => unknown): () => void {
  const stop = (e: Event): void => {
    e.preventDefault();
    e.stopPropagation();
  };
  const enter = (e: Event): void => {
    stop(e);
    node.classList.add('dragging');
  };
  const leave = (e: Event): void => {
    stop(e);
    if (!node.contains((e as DragEvent).relatedTarget as Node)) node.classList.remove('dragging');
  };
  const drop = async (e: Event): Promise<void> => {
    stop(e);
    node.classList.remove('dragging');
    const dt = (e as DragEvent).dataTransfer;
    if (dt) await onDrop(dt);
  };
  node.addEventListener('dragenter', enter);
  node.addEventListener('dragover', enter);
  node.addEventListener('dragleave', leave);
  node.addEventListener('drop', drop);
  return () => {
    node.removeEventListener('dragenter', enter);
    node.removeEventListener('dragover', enter);
    node.removeEventListener('dragleave', leave);
    node.removeEventListener('drop', drop);
  };
}

/** Render an error into a panel rather than only into the console, where nobody looks. */
export function showError(node: Element, e: unknown): void {
  clear(node).append(el('div', { class: 'finding error' },
    el('div', { class: 'problem' }, e instanceof Error ? e.message : String(e))));
}

/** A `verdict`/`finding` block, the two shapes every result in this app takes. */
export function finding(cls: string, path: string, problem: string, fix?: string): HTMLElement {
  return el('div', { class: `finding ${cls}` },
    path && el('div', { class: 'path' }, path),
    el('div', { class: 'problem' }, problem),
    fix && el('div', { class: 'fix' }, fix));
}
