// dom.js - the twenty lines of helper that make the rest of the UI readable without a framework.

export const $ = (sel, root = document) => root.querySelector(sel);
export const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];

/**
 * el('div', {class: 'row'}, 'text', el('b', {}, 'bold'))
 *
 * Text is always set via textContent, never innerHTML: findings, filenames and device replies are all
 * untrusted strings, and a card whose filename contains markup should not be able to do anything with
 * that fact.
 */
export function el(tag, props = {}, ...children) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (v == null || v === false) continue;
    if (k === 'class') node.className = v;
    else if (k === 'dataset') Object.assign(node.dataset, v);
    else if (k.startsWith('on') && typeof v === 'function') node.addEventListener(k.slice(2), v);
    else if (k in node && k !== 'list') node[k] = v;
    else node.setAttribute(k, v);
  }
  for (const c of children.flat()) {
    if (c == null || c === false) continue;
    node.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return node;
}

export function clear(node) {
  node.replaceChildren();
  return node;
}

/** Bytes, in the units a person reading a card thinks in. */
export function humanBytes(n) {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / (1024 * 1024)).toFixed(1)} MB`;
}

/**
 * A blocking confirm for the destructive verbs. Deliberately a real prompt rather than a toast:
 * docs/dev/terminal-target-b.md flags that sweeping controls can clear a recorded buffer or write the
 * card, so these must not happen behind one click.
 */
export function confirmDestructive(what) {
  return window.confirm(`${what}\n\nThis changes state on the device and cannot be undone from here. Continue?`);
}

/** Wire drag-and-drop on `node`, calling `onDrop(DataTransfer)`. Returns a teardown function. */
export function dropTarget(node, onDrop) {
  const stop = (e) => {
    e.preventDefault();
    e.stopPropagation();
  };
  const enter = (e) => {
    stop(e);
    node.classList.add('dragging');
  };
  const leave = (e) => {
    stop(e);
    if (!node.contains(e.relatedTarget)) node.classList.remove('dragging');
  };
  const drop = async (e) => {
    stop(e);
    node.classList.remove('dragging');
    await onDrop(e.dataTransfer);
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
export function showError(node, e) {
  clear(node).append(el('div', { class: 'finding error' },
    el('div', { class: 'problem' }, e instanceof Error ? e.message : String(e))));
}
