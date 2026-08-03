// dom_shim.ts - just enough DOM to mount the views in bun.
//
// Not a browser, and not pretending to be one. The point is narrower than it used to be, too: now that
// every tab's state and behaviour lives in a view-model (src/app/), the interesting logic is tested
// without any of this. What is left for the shim is the one thing a view can still get wrong on its
// own - build its whole UI in a mount function and throw halfway through, which is a blank tab in the
// real app and a thrown error here.
//
// What it deliberately does NOT model: layout, styling, real events, or anything asynchronous.
//
// Typed loosely on purpose. It is a stand-in for the DOM, not an implementation of it, and making the
// shim satisfy lib.dom's interfaces would be a far larger lie than the `any` at its boundary.

/* eslint-disable @typescript-eslint/no-explicit-any */

type Any = any;

class ClassList {
  constructor(private readonly node: ShimNode) {}

  private get set(): Set<string> {
    return new Set(this.node.className.split(/\s+/).filter(Boolean));
  }

  private write(s: Set<string>): void {
    this.node.className = [...s].join(' ');
  }

  add(...c: string[]): void {
    const s = this.set;
    c.forEach((x) => s.add(x));
    this.write(s);
  }

  remove(...c: string[]): void {
    const s = this.set;
    c.forEach((x) => s.delete(x));
    this.write(s);
  }

  contains(c: string): boolean {
    return this.set.has(c);
  }

  toggle(c: string, on?: boolean): void {
    const want = on === undefined ? !this.contains(c) : on;
    if (want) this.add(c);
    else this.remove(c);
  }
}

export class ShimText {
  parentNode: ShimNode | null = null;

  constructor(private readonly text: string) {}

  get textContent(): string {
    return this.text;
  }
}

export class ShimNode {
  readonly tagName: string;
  children: Array<ShimNode | ShimText> = [];
  parentNode: ShimNode | null = null;
  attributes: Record<string, string> = {};
  dataset: Record<string, string> = {};
  listeners: Record<string, Array<(e: Any) => unknown>> = {};
  className = '';
  classList: ClassList;
  private text = '';
  private val = '';

  // Properties the views assign directly. Present so el()'s `k in node` branch takes the property
  // path rather than falling through to setAttribute, which is what a real element does.
  hidden = false;
  disabled = false;
  checked = false;
  type = '';
  href = '';
  target = '';
  rel = '';
  title = '';
  id = '';
  width = 0;
  height = 0;
  placeholder = '';
  multiple = false;
  accept = '';
  min = '';
  max = '';
  step = '';
  autocomplete = '';
  spellcheck = true;
  scrollTop = 0;
  clientHeight = 0;
  scrollHeight = 0;

  constructor(tag: string) {
    this.tagName = String(tag).toUpperCase();
    this.classList = new ClassList(this);
  }

  /**
   * Two real-DOM behaviours the views legitimately rely on and that a naive shim gets wrong:
   * an unset <select> reads as its FIRST option's value, and an <option> with no value attribute
   * reads as its own text. Without these, every view that reads a select at mount time sees "".
   */
  get value(): string {
    if (this.tagName === 'SELECT' && this.val === '') {
      const first = this.children.find((c): c is ShimNode => c instanceof ShimNode && c.tagName === 'OPTION');
      return first ? first.value : '';
    }
    if (this.tagName === 'OPTION' && this.val === '') return this.textContent;
    return this.val;
  }

  set value(v: string) {
    this.val = String(v);
  }

  get textContent(): string {
    return this.children.length ? this.children.map((c) => c.textContent).join('') : this.text;
  }

  set textContent(v: string) {
    this.text = String(v);
    this.children = [];
  }

  get childElementCount(): number {
    return this.children.filter((c) => c instanceof ShimNode).length;
  }

  get firstElementChild(): ShimNode | null {
    return this.children.find((c): c is ShimNode => c instanceof ShimNode) ?? null;
  }

  append(...nodes: Any[]): void {
    for (const n of nodes.flat()) {
      if (n == null) continue;
      const node = n instanceof ShimNode || n instanceof ShimText ? n : new ShimText(String(n));
      node.parentNode = this;
      this.children.push(node);
    }
  }

  appendChild(n: Any): void {
    this.append(n);
  }

  replaceChildren(...nodes: Any[]): void {
    this.children = [];
    this.append(...nodes);
  }

  remove(): void {
    if (this.parentNode) {
      this.parentNode.children = this.parentNode.children.filter((c) => c !== this);
      this.parentNode = null;
    }
  }

  setAttribute(k: string, v: Any): void {
    this.attributes[k] = String(v);
  }

  getAttribute(k: string): string | null {
    return this.attributes[k] ?? null;
  }

  addEventListener(type: string, fn: (e: Any) => unknown): void {
    (this.listeners[type] ||= []).push(fn);
  }

  removeEventListener(type: string, fn: (e: Any) => unknown): void {
    this.listeners[type] = (this.listeners[type] || []).filter((f) => f !== fn);
  }

  /** Synchronously invoke the handlers for `type`. Enough to click a button in a test. */
  fire(type: string, event: Record<string, unknown> = {}): Promise<unknown[]> {
    return Promise.all((this.listeners[type] || []).map((f) =>
      f({ preventDefault() {}, stopPropagation() {}, target: this, ...event })));
  }

  contains(other: Any): boolean {
    if (other === this) return true;
    return this.children.some((c) => c instanceof ShimNode && c.contains(other));
  }

  get descendants(): ShimNode[] {
    const out: ShimNode[] = [];
    for (const c of this.children) {
      if (!(c instanceof ShimNode)) continue;
      out.push(c, ...c.descendants);
    }
    return out;
  }

  matches(sel: string): boolean {
    if (sel.startsWith('#')) return this.id === sel.slice(1);
    if (sel.startsWith('.')) return this.classList.contains(sel.slice(1));
    // `[attr]` and `[attr="value"]`. Added when the overview moved into index.html: its actions are
    // static markup carrying `data-view`, and the view finds them by attribute. The shim models what
    // the app uses, so it grows when the app does - bending the app to the double would be backwards.
    if (sel.startsWith('[') && sel.endsWith(']')) {
      const [name, ...rest] = sel.slice(1, -1).split('=');
      const value = rest.join('=').replace(/^["']|["']$/g, '');
      const have = this.getAttribute(name);
      return rest.length ? have === value : have != null;
    }
    return this.tagName === sel.toUpperCase();
  }

  /** Handles `tag`, `.class`, `#id`, `a b` (descendant) and `a > b` (child) - all this app uses. */
  querySelectorAll(sel: string): ShimNode[] {
    const parts = sel.trim().split(/\s+/);
    let pool = this.descendants;
    for (let i = 0; i < parts.length; i++) {
      const p = parts[i];
      if (p === '>') {
        const next = parts[++i];
        pool = pool.flatMap((n) =>
          n.children.filter((c): c is ShimNode => c instanceof ShimNode && c.matches(next)));
        continue;
      }
      pool = i === 0 ? pool.filter((n) => n.matches(p))
        : pool.flatMap((n) => n.descendants.filter((c) => c.matches(p)));
    }
    return [...new Set(pool)];
  }

  querySelector(sel: string): ShimNode | null {
    return this.querySelectorAll(sel)[0] ?? null;
  }

  getContext(): Any {
    // A no-op 2D context: the CPU plot draws on every poll, and it must not throw when it does.
    const noop = (): void => {};
    return new Proxy({
      canvas: this, fillStyle: '', strokeStyle: '', lineWidth: 1, font: '',
      measureText: () => ({ width: 0 }),
    } as Any, {
      get: (t, k) => (k in t ? t[k] : noop),
      set: (t, k, v) => {
        t[k] = v;
        return true;
      },
    });
  }
}

export interface ShimCapabilities {
  fileSystemAccess?: boolean;
  serial?: boolean | Record<string, unknown>;
}

export interface InstalledDom {
  document: Any;
  Node: typeof ShimNode;
  restore(): void;
}

/**
 * Install the shim onto globalThis. Returns the document, and a teardown.
 *
 * Defaults to the LEAST capable browser - no File System Access, no WebSerial - because that is the
 * configuration the app has to degrade into, and a shim that quietly presents a full Chromium would
 * never exercise the fallbacks. Pass `{fileSystemAccess: true}` / `{serial: true}` to test the other
 * branch, or pass an OBJECT as `serial` to script what `navigator.serial.requestPort` does.
 */
export function installDom({ fileSystemAccess = false, serial = false }: ShimCapabilities = {}): InstalledDom {
  const doc = new ShimNode('html') as Any;
  doc.createElement = (tag: string) => new ShimNode(tag);
  doc.createTextNode = (t: string) => new ShimText(t);
  doc.body = new ShimNode('body');
  doc.append(doc.body);

  const saved: Record<string, unknown> = {};
  const g = globalThis as Any;
  const set = (k: string, v: unknown): void => {
    saved[k] = g[k];
    g[k] = v;
  };

  const win: Any = { confirm: () => true };
  if (fileSystemAccess) {
    win.showDirectoryPicker = async (): Promise<never> => {
      throw Object.assign(new Error('cancelled'), { name: 'AbortError' });
    };
  }

  set('Node', ShimNode);
  set('document', doc);
  set('window', win);
  set('navigator', serial
    ? { serial: serial === true ? { requestPort: async () => {} } : serial }
    : {});
  set('getComputedStyle', () => ({ getPropertyValue: () => '' }));
  set('location', { hash: '', protocol: 'http:' });

  return {
    document: doc,
    Node: ShimNode,
    restore() {
      for (const [k, v] of Object.entries(saved)) {
        if (v === undefined) delete g[k];
        else g[k] = v;
      }
    },
  };
}

/** Every element under `root` whose tag matches, for asserting a view built what it claims to. */
export const tags = (root: ShimNode, tag: string): ShimNode[] =>
  root.descendants.filter((n) => n.tagName === tag.toUpperCase());

/** All text in the subtree, for asserting a view says the thing it must say. */
export const textOf = (root: ShimNode): string => root.textContent;
