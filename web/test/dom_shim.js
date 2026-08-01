// dom_shim.js - just enough DOM to mount the views in node/bun.
//
// Not a browser, and not pretending to be one. The point is narrower: every view builds its whole UI
// imperatively in a mount function, so a typo in an el() property, a missing import, or an exception
// during construction is a blank tab in the real app and a thrown error here. That is the class of bug
// worth catching without a headless browser, and it costs a hundred lines.
//
// What it deliberately does NOT model: layout, styling, real events, or anything asynchronous. Tests
// using this assert that a view MOUNTS and wires up the controls it claims to, not that it looks right.

class ClassList {
  constructor(node) {
    this.node = node;
  }

  get _set() {
    return new Set(this.node.className.split(/\s+/).filter(Boolean));
  }

  _write(s) {
    this.node.className = [...s].join(' ');
  }

  add(...c) {
    const s = this._set;
    c.forEach((x) => s.add(x));
    this._write(s);
  }

  remove(...c) {
    const s = this._set;
    c.forEach((x) => s.delete(x));
    this._write(s);
  }

  contains(c) {
    return this._set.has(c);
  }

  toggle(c, on) {
    if (on === undefined) on = !this.contains(c);
    return on ? this.add(c) : this.remove(c);
  }
}

class Node {
  constructor(tag) {
    this.tagName = String(tag).toUpperCase();
    this.children = [];
    this.parentNode = null;
    this.attributes = {};
    this.dataset = {};
    this.listeners = {};
    this.className = '';
    this.classList = new ClassList(this);
    this._text = '';
    this._value = '';
    // Properties the views assign directly. Present so el()'s `k in node` branch takes the property
    // path rather than falling through to setAttribute, which is what a real element does.
    this.hidden = false;
    this.disabled = false;
    this.checked = false;
    this.type = '';
    this.href = '';
    this.target = '';
    this.rel = '';
    this.title = '';
    this.id = '';
    this.width = 0;
    this.height = 0;
    this.placeholder = '';
    this.multiple = false;
    this.accept = '';
    this.min = '';
    this.max = '';
    this.step = '';
    this.autocomplete = '';
    this.spellcheck = true;
    this.scrollTop = 0;
    this.clientHeight = 0;
    this.scrollHeight = 0;
  }

  /**
   * Two real-DOM behaviours the views legitimately rely on and that a naive shim gets wrong:
   * an unset <select> reads as its FIRST option's value, and an <option> with no value attribute
   * reads as its own text. Without these, every view that reads a select at mount time sees "".
   */
  get value() {
    if (this.tagName === 'SELECT' && this._value === '') {
      const first = this.children.find((c) => c instanceof Node && c.tagName === 'OPTION');
      return first ? first.value : '';
    }
    if (this.tagName === 'OPTION' && this._value === '') return this.textContent;
    return this._value;
  }

  set value(v) {
    this._value = String(v);
  }

  get textContent() {
    return this.children.length ? this.children.map((c) => c.textContent).join('') : this._text;
  }

  set textContent(v) {
    this._text = String(v);
    this.children = [];
  }

  get childElementCount() {
    return this.children.filter((c) => c instanceof Node).length;
  }

  get firstElementChild() {
    return this.children.find((c) => c instanceof Node) || null;
  }

  append(...nodes) {
    for (const n of nodes.flat()) {
      if (n == null) continue;
      const node = n instanceof Node || n instanceof TextNode ? n : new TextNode(String(n));
      node.parentNode = this;
      this.children.push(node);
    }
  }

  replaceChildren(...nodes) {
    this.children = [];
    this.append(...nodes);
  }

  remove() {
    if (this.parentNode) {
      this.parentNode.children = this.parentNode.children.filter((c) => c !== this);
      this.parentNode = null;
    }
  }

  setAttribute(k, v) {
    this.attributes[k] = String(v);
  }

  getAttribute(k) {
    return this.attributes[k] ?? null;
  }

  addEventListener(type, fn) {
    (this.listeners[type] ||= []).push(fn);
  }

  removeEventListener(type, fn) {
    this.listeners[type] = (this.listeners[type] || []).filter((f) => f !== fn);
  }

  /** Synchronously invoke the handlers for `type`. Enough to click a button in a test. */
  fire(type, event = {}) {
    return Promise.all((this.listeners[type] || []).map((f) => f({ preventDefault() {}, stopPropagation() {}, target: this, ...event })));
  }

  contains(other) {
    if (other === this) return true;
    return this.children.some((c) => c instanceof Node && c.contains(other));
  }

  get descendants() {
    const out = [];
    for (const c of this.children) {
      if (!(c instanceof Node)) continue;
      out.push(c, ...c.descendants);
    }
    return out;
  }

  matches(sel) {
    if (sel.startsWith('#')) return this.id === sel.slice(1);
    if (sel.startsWith('.')) return this.classList.contains(sel.slice(1));
    return this.tagName === sel.toUpperCase();
  }

  /** Handles `tag`, `.class`, `#id`, `a b` (descendant) and `a > b` (child) - all this app uses. */
  querySelectorAll(sel) {
    const parts = sel.trim().split(/\s+/);
    let pool = this.descendants;
    for (let i = 0; i < parts.length; i++) {
      const p = parts[i];
      if (p === '>') {
        const next = parts[++i];
        pool = pool.flatMap((n) => n.children.filter((c) => c instanceof Node && c.matches(next)));
        continue;
      }
      pool = i === 0 ? pool.filter((n) => n.matches(p))
        : pool.flatMap((n) => n.descendants.filter((c) => c.matches(p)));
    }
    return [...new Set(pool)];
  }

  querySelector(sel) {
    return this.querySelectorAll(sel)[0] || null;
  }

  getContext() {
    // A no-op 2D context: the CPU plot draws on every poll, and it must not throw when it does.
    const noop = () => {};
    return new Proxy({
      canvas: this, fillStyle: '', strokeStyle: '', lineWidth: 1, font: '',
      measureText: () => ({ width: 0 }),
    }, { get: (t, k) => (k in t ? t[k] : noop), set: (t, k, v) => { t[k] = v; return true; } });
  }
}

class TextNode {
  constructor(text) {
    this._text = String(text);
    this.parentNode = null;
  }

  get textContent() {
    return this._text;
  }
}

/**
 * Install the shim onto globalThis. Returns the document, and a teardown.
 *
 * Defaults to the LEAST capable browser - no File System Access, no WebSerial - because that is the
 * configuration the app has to degrade into, and a shim that quietly presents a full Chromium would
 * never exercise the fallbacks. Pass `{fileSystemAccess: true}` / `{serial: true}` to test the other
 * branch.
 */
export function installDom({ fileSystemAccess = false, serial = false } = {}) {
  const doc = new Node('html');
  doc.createElement = (tag) => new Node(tag);
  doc.createTextNode = (t) => new TextNode(t);
  doc.body = new Node('body');
  doc.append(doc.body);

  const saved = {};
  const set = (k, v) => {
    saved[k] = globalThis[k];
    globalThis[k] = v;
  };

  const win = { confirm: () => true };
  if (fileSystemAccess) {
    win.showDirectoryPicker = async () => {
      throw Object.assign(new Error('cancelled'), { name: 'AbortError' });
    };
  }

  set('Node', Node);
  set('document', doc);
  set('window', win);
  set('navigator', serial ? { serial: { requestPort: async () => {} } } : {});
  set('getComputedStyle', () => ({ getPropertyValue: () => '' }));
  set('location', { hash: '', protocol: 'http:' });
  set('URL', globalThis.URL);

  return {
    document: doc,
    Node,
    TextNode,
    restore() {
      for (const [k, v] of Object.entries(saved)) {
        if (v === undefined) delete globalThis[k];
        else globalThis[k] = v;
      }
    },
  };
}

/** Every element under `root` whose tag matches, for asserting a view built what it claims to. */
export const tags = (root, tag) => root.descendants.filter((n) => n.tagName === tag.toUpperCase());

/** All text in the subtree, for asserting a view says the thing it must say. */
export const textOf = (root) => root.textContent;
