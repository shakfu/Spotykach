// src/core/layout.ts
var SCHEMA = 1;
function makeLayout(data) {
  if (!data || data.schema !== SCHEMA) {
    throw new Error(`card_layout.json: unsupported schema ${data && data.schema} (expected ${SCHEMA})`);
  }
  const byEngine = new Map(data.banks.map((b) => [b.engine, b]));
  const sidecars = new Set(data.sidecar_names);
  const sourceExts = new Set(data.source_extensions);
  const skipDirs = new Set(data.skip_dirs);
  return {
    data,
    scan: data.scan,
    banks: data.banks,
    allDirs: data.all_dirs,
    granularTapes: data.granular_tapes,
    defaultConfig: data.default_config,
    configProperties: data.config_properties,
    readmes: data.readmes,
    rootReadme: data.root_readme,
    bank: (engine) => byEngine.get(engine),
    isSidecar: (name) => sidecars.has(name.toUpperCase()),
    isSourceExt: (ext) => sourceExts.has(ext.toLowerCase()),
    isSkippedDir: (name) => skipDirs.has(name),
    audioBanks: () => data.banks.filter((b) => b.target),
    bankForPath(rel) {
      let best = null;
      let bestLen = -1;
      for (const bank of data.banks) {
        for (const d of bank.dirs) {
          if ((rel === d || rel.startsWith(`${d}/`)) && d.length > bestLen) {
            best = bank;
            bestLen = d.length;
          }
        }
      }
      return best;
    },
    scanNameOk(name) {
      if (!name || name.startsWith("."))
        return false;
      if (name.length > data.scan.max_name)
        return false;
      if (!name.includes("."))
        return false;
      return data.scan.extensions.includes(name.split(".").pop().toLowerCase());
    }
  };
}
function folderLabel(dirs) {
  if (dirs.length === 1)
    return dirs[0];
  const first = dirs[0];
  const cut = first.lastIndexOf("/");
  const parent = cut < 0 ? "" : first.slice(0, cut);
  const leaves = dirs.map((d) => d.slice(d.lastIndexOf("/") + 1));
  if (!dirs.every((d) => d.slice(0, d.lastIndexOf("/")) === parent))
    return dirs.join(", ");
  const nums = leaves.map(Number);
  const contiguous = nums.every(Number.isInteger) && nums.every((n, i) => i === 0 || n === nums[i - 1] + 1);
  const set = contiguous ? `${nums[0]}..${nums[nums.length - 1]}` : leaves.join(",");
  return parent ? `${parent}/{${set}}` : `{${set}}`;
}
function formatTarget(template, i, vars = {}) {
  const { deck = "a", bank = 0, tape = "B" } = vars;
  return template.replaceAll("{i02}", String(i).padStart(2, "0")).replaceAll("{i}", String(i)).replaceAll("{deck}", deck).replaceAll("{bank}", String(bank)).replaceAll("{tape}", tape);
}

// src/core/engines.ts
var ENGINES_SCHEMA = 1;
function makeCatalogue(data, layout) {
  if (!data || data.schema !== ENGINES_SCHEMA) {
    throw new Error(`engines.json: unsupported schema ${data && data.schema} (expected ${ENGINES_SCHEMA})`);
  }
  const entries = data.engines.map((doc) => ({
    doc,
    bank: doc.bank ? layout.bank(doc.bank) ?? null : null
  }));
  const documented = new Set(entries.filter((e) => e.bank).map((e) => e.bank.engine));
  for (const bank of layout.banks) {
    if (documented.has(bank.engine))
      continue;
    entries.push({
      bank,
      doc: {
        name: bank.engine,
        title: bank.engine,
        summary: "",
        body: bank.blurb,
        source: "",
        doc: "",
        page: "",
        released: false,
        bank: bank.engine
      }
    });
  }
  const byName = new Map(entries.map((e) => [e.doc.name, e]));
  return {
    entries,
    get: (name) => byName.get(name),
    readers: () => entries.filter((e) => e.bank !== null)
  };
}

// src/app/store.ts
class Store {
  state;
  listeners = new Set;
  constructor(initial) {
    this.state = initial;
  }
  get() {
    return this.state;
  }
  set(patch) {
    this.state = { ...this.state, ...patch };
    for (const fn of [...this.listeners])
      fn(this.state);
  }
  subscribe(fn) {
    this.listeners.add(fn);
    fn(this.state);
    return () => this.listeners.delete(fn);
  }
}

// src/ui/dom.ts
var $ = (sel, root = document) => root.querySelector(sel);
var $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
function el(tag, props = {}, ...children) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (v == null || v === false)
      continue;
    if (k === "class")
      node.className = String(v);
    else if (k === "dataset")
      Object.assign(node.dataset, v);
    else if (k.startsWith("on") && typeof v === "function") {
      node.addEventListener(k.slice(2), v);
    } else if (k in node && k !== "list")
      node[k] = v;
    else
      node.setAttribute(k, String(v));
  }
  append(node, children);
  return node;
}
function append(node, children) {
  for (const c of children) {
    if (c == null || c === false)
      continue;
    if (Array.isArray(c))
      append(node, c);
    else
      node.appendChild(c instanceof Node ? c : document.createTextNode(String(c)));
  }
}
function clear(node) {
  node.replaceChildren();
  return node;
}
function humanBytes(n) {
  if (n < 1024)
    return `${n} B`;
  if (n < 1024 * 1024)
    return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / (1024 * 1024)).toFixed(1)} MB`;
}
function confirmDestructive(what) {
  return window.confirm(`${what}

This changes state on the device and cannot be undone from here. Continue?`);
}
function dropTarget(node, onDrop) {
  const stop = (e) => {
    e.preventDefault();
    e.stopPropagation();
  };
  const enter = (e) => {
    stop(e);
    node.classList.add("dragging");
  };
  const leave = (e) => {
    stop(e);
    if (!node.contains(e.relatedTarget))
      node.classList.remove("dragging");
  };
  const drop = async (e) => {
    stop(e);
    node.classList.remove("dragging");
    const dt = e.dataTransfer;
    if (dt)
      await onDrop(dt);
  };
  node.addEventListener("dragenter", enter);
  node.addEventListener("dragover", enter);
  node.addEventListener("dragleave", leave);
  node.addEventListener("drop", drop);
  return () => {
    node.removeEventListener("dragenter", enter);
    node.removeEventListener("dragover", enter);
    node.removeEventListener("dragleave", leave);
    node.removeEventListener("drop", drop);
  };
}
function showError(node, e) {
  clear(node).append(el("div", { class: "finding error" }, el("div", { class: "problem" }, e instanceof Error ? e.message : String(e))));
}
var SEVERITY = { error: "ERROR", warn: "WARNING", ok: "OK" };
function finding(cls, path, problem, fix) {
  const label = SEVERITY[cls];
  return el("div", { class: `finding ${cls}` }, el("div", { class: "problem" }, label && el("span", { class: "badge" }, label), path && el("span", { class: "path" }, `${path}  `), problem), fix && el("div", { class: "fix" }, fix));
}

// src/core/build.ts
var ascii = (s) => new TextEncoder().encode(s);
function buildCard(layout, patches = {}) {
  const files = [];
  const add = (path, text) => files.push({ path, bytes: ascii(text) });
  for (const bank of layout.banks) {
    for (const d of bank.dirs)
      add(`${d}/README.TXT`, layout.readmes[d]);
  }
  add("SK/config.txt", layout.defaultConfig);
  for (const bank of layout.banks) {
    for (const [path, content] of Object.entries(bank.extras))
      add(path, content);
  }
  for (const [path, text] of Object.entries(patches))
    add(path, text);
  add("README.TXT", layout.rootReadme.bare);
  files.sort((a, b) => a.path < b.path ? -1 : a.path > b.path ? 1 : 0);
  return { dirs: [...layout.allDirs], files };
}
function missingFrom(built, existingPaths) {
  const have = new Set([...existingPaths].map((p) => p.toUpperCase()));
  return {
    dirs: built.dirs,
    files: built.files.filter((f) => !have.has(f.path.toUpperCase()))
  };
}

// src/core/zip.ts
var CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0;i < 256; i++) {
    let c = i;
    for (let k = 0;k < 8; k++)
      c = c & 1 ? 3988292384 ^ c >>> 1 : c >>> 1;
    t[i] = c >>> 0;
  }
  return t;
})();
function crc32(bytes) {
  let c = 4294967295;
  for (let i = 0;i < bytes.length; i++)
    c = CRC_TABLE[(c ^ bytes[i]) & 255] ^ c >>> 8;
  return (c ^ 4294967295) >>> 0;
}
var DOS_DATE = 1980 - 1980 << 9 | 1 << 5 | 1;
var DOS_TIME = 0;
var METHOD_STORE = 0;
var METHOD_DEFLATE = 8;
var ZIP_MIME = "application/zip";
function u32(view, off, v) {
  view.setUint32(off, v >>> 0, true);
}
async function makeZip(files, dirs = [], deflate) {
  const entries = [];
  for (const d of dirs)
    entries.push({ name: d.replace(/\/?$/, "/"), bytes: new Uint8Array(0), dir: true });
  for (const f of files)
    entries.push({ name: f.path, bytes: f.bytes, dir: false });
  const chunks = [];
  const central = [];
  let offset = 0;
  const enc = new TextEncoder;
  for (const e of entries) {
    const name = enc.encode(e.name);
    const crc = crc32(e.bytes);
    let method = METHOD_STORE;
    let body = e.bytes;
    if (deflate && !e.dir && e.bytes.length > 0) {
      const packed = await deflate(e.bytes);
      if (packed && packed.length < e.bytes.length) {
        method = METHOD_DEFLATE;
        body = packed;
      }
    }
    const local = new Uint8Array(30 + name.length);
    const lv = new DataView(local.buffer);
    u32(lv, 0, 67324752);
    lv.setUint16(4, 20, true);
    lv.setUint16(6, 0, true);
    lv.setUint16(8, method, true);
    lv.setUint16(10, DOS_TIME, true);
    lv.setUint16(12, DOS_DATE, true);
    u32(lv, 14, crc);
    u32(lv, 18, body.length);
    u32(lv, 22, e.bytes.length);
    lv.setUint16(26, name.length, true);
    lv.setUint16(28, 0, true);
    local.set(name, 30);
    chunks.push(local, body);
    central.push({ name, crc, method, comp: body.length, raw: e.bytes.length, offset, dir: e.dir });
    offset += local.length + body.length;
  }
  const centralStart = offset;
  for (const c of central) {
    const rec = new Uint8Array(46 + c.name.length);
    const cv = new DataView(rec.buffer);
    u32(cv, 0, 33639248);
    cv.setUint16(4, 20, true);
    cv.setUint16(6, 20, true);
    cv.setUint16(8, 0, true);
    cv.setUint16(10, c.method, true);
    cv.setUint16(12, DOS_TIME, true);
    cv.setUint16(14, DOS_DATE, true);
    u32(cv, 16, c.crc);
    u32(cv, 20, c.comp);
    u32(cv, 24, c.raw);
    cv.setUint16(28, c.name.length, true);
    cv.setUint16(30, 0, true);
    cv.setUint16(32, 0, true);
    cv.setUint16(34, 0, true);
    cv.setUint16(36, 0, true);
    u32(cv, 38, c.dir ? 16877 << 16 | 16 : 33188 << 16);
    u32(cv, 42, c.offset);
    rec.set(c.name, 46);
    chunks.push(rec);
    offset += rec.length;
  }
  const end = new Uint8Array(22);
  const ev = new DataView(end.buffer);
  u32(ev, 0, 101010256);
  ev.setUint16(8, central.length, true);
  ev.setUint16(10, central.length, true);
  u32(ev, 12, offset - centralStart);
  u32(ev, 16, centralStart);
  chunks.push(end);
  const total = chunks.reduce((n, c) => n + c.length, 0);
  const out = new Uint8Array(total);
  let at = 0;
  for (const c of chunks) {
    out.set(c, at);
    at += c.length;
  }
  return out;
}

// src/app/build_model.ts
var INITIAL = {
  status: "",
  verdict: null,
  failures: [],
  error: null,
  busy: false
};

class BuildModel {
  layout;
  patches;
  deps;
  store = new Store({ ...INITIAL });
  constructor(layout, patches, deps) {
    this.layout = layout;
    this.patches = patches;
    this.deps = deps;
  }
  built() {
    return buildCard(this.layout, this.patches);
  }
  canWriteInPlace() {
    return this.deps.access.hasDirectAccess();
  }
  async downloadZip() {
    this.store.set({ ...INITIAL, busy: true, status: "Packing..." });
    try {
      const b = this.built();
      const bytes = await makeZip(b.files, b.dirs, this.deps.deflate);
      this.deps.downloader.save(bytes, "sk-card-starter.zip", ZIP_MIME);
      this.store.set({
        busy: false,
        status: `${b.files.length} files, ${b.dirs.length} folders, ${bytes.length} bytes`,
        verdict: {
          kind: "good",
          title: "Downloaded.",
          detail: "Unpack it onto a FAT32-formatted card so the folders sit at the card's root - the " + "card should contain SK, tapes, radio and the rest directly, not a folder containing them."
        }
      });
    } catch (e) {
      this.store.set({ busy: false, status: "", error: e.message });
    }
  }
  async writeInPlace() {
    this.store.set({ ...INITIAL, busy: true });
    try {
      const card = await this.deps.access.pickDirectory("readwrite");
      const b = this.built();
      const todo = missingFrom(b, card.files.map((f) => f.path));
      this.store.set({ status: `Writing ${todo.files.length} missing files...` });
      const { written, failed } = await this.deps.access.writeInto(card.handle, todo.files, todo.dirs);
      const skipped = b.files.length - todo.files.length;
      this.store.set({
        busy: false,
        status: `${written.length} written, ${skipped} already present, ${failed.length} failed`,
        failures: failed,
        verdict: {
          kind: failed.length ? "mixed" : "good",
          title: failed.length ? "Finished with problems." : "Card is ready.",
          detail: skipped ? `${skipped} files were already there and were left untouched.` : "Every folder, config and README is in place."
        }
      });
      return true;
    } catch (e) {
      const err = e;
      this.store.set({ busy: false, status: "" });
      if (err.name === "AbortError")
        return false;
      this.store.set({ error: err.message });
      return false;
    }
  }
}

// src/platform/cardsource.ts
var hasFileSystemAccess = () => typeof window !== "undefined" && ("showDirectoryPicker" in window);
function entryFor(path, file) {
  return {
    path,
    size: file.size,
    async read(max) {
      const blob = max != null && max < file.size ? file.slice(0, max) : file;
      return new Uint8Array(await blob.arrayBuffer());
    }
  };
}
async function fromDirectoryHandle(handle) {
  const files = [];
  const dirs = new Set;
  const walk = async (dir, prefix, depth) => {
    if (depth > 6)
      return;
    for await (const [name, child] of dir.entries()) {
      const path = prefix ? `${prefix}/${name}` : name;
      if (child.kind === "directory") {
        dirs.add(path);
        await walk(child, path, depth + 1);
      } else {
        files.push(entryFor(path, await child.getFile()));
      }
    }
  };
  await walk(handle, "", 0);
  return { files, dirs, handle };
}
async function pickDirectory(mode = "read") {
  const w = window;
  if (!w.showDirectoryPicker) {
    throw new Error("This browser has no File System Access API. Drop the card folder onto the page " + "instead, or use Chrome/Edge to edit a card in place.");
  }
  return fromDirectoryHandle(await w.showDirectoryPicker({ mode, id: "sk-card" }));
}
function fromFileList(list) {
  const files = [];
  const dirs = new Set;
  for (const file of Array.from(list)) {
    const rel = (file.webkitRelativePath || file.name).split("/").slice(1).join("/") || file.name;
    files.push(entryFor(rel, file));
    let d = rel.includes("/") ? rel.slice(0, rel.lastIndexOf("/")) : "";
    while (d) {
      dirs.add(d);
      d = d.includes("/") ? d.slice(0, d.lastIndexOf("/")) : "";
    }
  }
  return { files, dirs, handle: null };
}
async function fromDataTransfer(dt) {
  const items = [...dt.items].filter((i) => i.kind === "file");
  const roots = items.map((i) => i.webkitGetAsEntry ? i.webkitGetAsEntry() : null).filter((e) => Boolean(e));
  const plainFiles = [...dt.files];
  const pendingHandle = items.length === 1 && typeof items[0].getAsFileSystemHandle === "function" ? items[0].getAsFileSystemHandle() : null;
  if (pendingHandle) {
    const handle = await pendingHandle;
    if (handle && handle.kind === "directory")
      return fromDirectoryHandle(handle);
  }
  const files = [];
  const dirs = new Set;
  const readEntry = (entry, prefix) => new Promise((resolve) => {
    const path = prefix ? `${prefix}/${entry.name}` : entry.name;
    if (entry.isFile) {
      entry.file((f) => {
        files.push(entryFor(path, f));
        resolve();
      }, () => resolve());
    } else if (entry.isDirectory) {
      dirs.add(path);
      const reader = entry.createReader();
      const batch = () => reader.readEntries(async (list) => {
        if (!list.length)
          return resolve();
        await Promise.all(list.map((e) => readEntry(e, path)));
        batch();
      }, () => resolve());
      batch();
    } else {
      resolve();
    }
  });
  if (roots.length === 1 && roots[0].isDirectory) {
    const reader = roots[0].createReader();
    await new Promise((resolve) => {
      const batch = () => reader.readEntries(async (list) => {
        if (!list.length)
          return resolve();
        await Promise.all(list.map((e) => readEntry(e, "")));
        batch();
      }, () => resolve());
      batch();
    });
    return { files, dirs, handle: null };
  }
  await Promise.all(roots.map((e) => readEntry(e, "")));
  if (!roots.length) {
    for (const f of plainFiles)
      files.push(entryFor(f.name, f));
  }
  return { files, dirs, handle: null };
}
async function writeInto(handle, files, dirs = []) {
  const root = handle;
  const written = [];
  const failed = [];
  const dirHandle = async (path) => {
    let cur = root;
    for (const part of path.split("/").filter(Boolean)) {
      cur = await cur.getDirectoryHandle(part, { create: true });
    }
    return cur;
  };
  for (const d of dirs) {
    try {
      await dirHandle(d);
    } catch (e) {
      failed.push({ path: d, error: e.message });
    }
  }
  for (const f of files) {
    try {
      const i = f.path.lastIndexOf("/");
      const dir = i < 0 ? root : await dirHandle(f.path.slice(0, i));
      const fh = await dir.getFileHandle(f.path.slice(i + 1), { create: true });
      const w = await fh.createWritable();
      await w.write(f.bytes);
      await w.close();
      written.push(f.path);
    } catch (e) {
      failed.push({ path: f.path, error: e.message });
    }
  }
  return { written, failed };
}
var cardAccess = {
  hasDirectAccess: hasFileSystemAccess,
  pickDirectory,
  writeInto
};

// src/platform/download.ts
function saveBytes(bytes, filename, mime = "application/octet-stream") {
  const blob = new Blob([bytes], { type: mime });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1e4);
}
var downloader = { save: saveBytes };
var deflateRaw = async (bytes) => {
  if (typeof CompressionStream === "undefined")
    return null;
  try {
    const stream = new Blob([bytes]).stream().pipeThrough(new CompressionStream("deflate-raw"));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  } catch {
    return null;
  }
};

// src/ui/slots.ts
function mountPoint(root) {
  return root.querySelector("[data-mount]") ?? root;
}
function slot(root, name) {
  return root.querySelector(`[data-slot="${name}"]`) ?? mountPoint(root);
}
function fill(root, name, text) {
  const node = root.querySelector(`[data-fill="${name}"]`);
  if (node)
    node.textContent = text;
}

// src/ui/build_view.ts
function mountBuild(root, ctx) {
  const model = new BuildModel(ctx.layout, ctx.patches, {
    access: cardAccess,
    downloader,
    deflate: deflateRaw
  });
  const status = el("div", { class: "status" });
  const out = el("div", { class: "results" });
  const inPlace = el("button", { onclick: () => model.writeInPlace() }, "Write onto a card");
  if (!model.canWriteInPlace()) {
    inPlace.disabled = true;
    inPlace.title = "This browser has no File System Access API - use the zip";
  }
  model.store.subscribe((s) => {
    status.textContent = s.status;
    clear(out);
    if (s.error) {
      out.append(finding("error", "", s.error));
      return;
    }
    if (s.verdict) {
      out.append(el("div", { class: `verdict ${s.verdict.kind}` }, el("strong", {}, s.verdict.title), el("p", {}, s.verdict.detail)));
    }
    for (const f of s.failures)
      out.append(finding("error", f.path, f.error));
  });
  const b = model.built();
  const folders = el("table", { class: "layout" }, el("thead", {}, el("tr", {}, el("th", {}, "Folder"), el("th", {}, "Engine"), el("th", {}, "Format"))), el("tbody", {}, ctx.layout.banks.map((bank) => el("tr", {}, el("td", { class: "mono" }, folderLabel(bank.dirs)), el("td", {}, bank.readers.join(", ")), el("td", { class: "muted" }, bank.fmt.describe)))));
  mountPoint(root).append(el("div", { class: "controls" }, el("button", { class: "primary", onclick: () => model.downloadZip() }, "Download a starter card (.zip)"), inPlace), status, out);
  fill(root, "files", String(b.files.length));
  fill(root, "dirs", String(b.dirs.length));
  slot(root, "folders").append(folders);
}

// src/core/wav.ts
var F32 = "f32";
var INT16 = "int16";
var U8 = "u8";
var INT24 = "int24";
var INT32 = "int32";
var WAVE_FORMAT_PCM = 1;
var WAVE_FORMAT_FLOAT = 3;
var WAVE_FORMAT_EXTENSIBLE = 65534;
var MAX_CHUNKS = 64;

class WavError extends Error {
}

class WavNeedMore extends Error {
  needed;
  constructor(needed) {
    super(`need at least ${needed} bytes of header`);
    this.needed = needed;
  }
}
var ENC_LABEL = new Map([
  [`${WAVE_FORMAT_FLOAT}/32`, "32-bit float"],
  [`${WAVE_FORMAT_PCM}/16`, "16-bit PCM"],
  [`${WAVE_FORMAT_PCM}/8`, "8-bit PCM"],
  [`${WAVE_FORMAT_PCM}/24`, "24-bit PCM"],
  [`${WAVE_FORMAT_PCM}/32`, "32-bit INTEGER PCM"]
]);

class WavInfo {
  fmt;
  channels;
  rate;
  bits;
  dataOffset;
  dataSize;
  constructor(fmt, channels, rate, bits, dataOffset, dataSize) {
    this.fmt = fmt;
    this.channels = channels;
    this.rate = rate;
    this.bits = bits;
    this.dataOffset = dataOffset;
    this.dataSize = dataSize;
  }
  get encoding() {
    if (this.fmt === WAVE_FORMAT_FLOAT && this.bits === 32)
      return F32;
    if (this.fmt !== WAVE_FORMAT_PCM)
      return null;
    switch (this.bits) {
      case 8:
        return U8;
      case 16:
        return INT16;
      case 24:
        return INT24;
      case 32:
        return INT32;
      default:
        return null;
    }
  }
  get frames() {
    const bytesPerFrame = Math.max(1, Math.floor(this.bits / 8) * Math.max(1, this.channels));
    return Math.floor(this.dataSize / bytesPerFrame);
  }
  get seconds() {
    return this.rate ? this.frames / this.rate : 0;
  }
  describe() {
    const enc = ENC_LABEL.get(`${this.fmt}/${this.bits}`) ?? `format tag ${this.fmt}, ${this.bits}-bit`;
    const ch = this.channels === 1 ? "mono" : this.channels === 2 ? "stereo" : `${this.channels}ch`;
    return `${enc}, ${ch}, ${this.rate} Hz`;
  }
}
var ascii2 = (view, off) => String.fromCharCode(view.getUint8(off), view.getUint8(off + 1), view.getUint8(off + 2), view.getUint8(off + 3));
function parseWav(bytes, totalSize = bytes.length) {
  const size = totalSize;
  const have = bytes.length;
  if (size < 12)
    throw new WavError("file is too short to be a WAV (under 12 bytes)");
  if (have < 12)
    throw new WavNeedMore(12);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (ascii2(view, 0) !== "RIFF" || ascii2(view, 8) !== "WAVE") {
    throw new WavError("not a RIFF/WAVE file (missing the 'RIFF'/'WAVE' magic)");
  }
  let fmt = 0;
  let channels = 0;
  let bits = 0;
  let rate = 0;
  let haveFmt = false;
  let pos = 12;
  for (let n = 0;n < MAX_CHUNKS; n++) {
    if (pos + 8 > size)
      throw new WavError("ran off the end of the file without finding a 'data' chunk");
    if (pos + 8 > have)
      throw new WavNeedMore(pos + 8);
    const cid = ascii2(view, pos);
    const csize = view.getUint32(pos + 4, true);
    const body = pos + 8;
    if (cid === "fmt ") {
      if (csize < 16 || body + 16 > size) {
        throw new WavError("'fmt ' chunk is shorter than the 16 bytes the parser needs");
      }
      if (body + 16 > have)
        throw new WavNeedMore(body + 16);
      fmt = view.getUint16(body + 0, true);
      channels = view.getUint16(body + 2, true);
      rate = view.getUint32(body + 4, true);
      bits = view.getUint16(body + 14, true);
      if (fmt === WAVE_FORMAT_EXTENSIBLE && csize >= 40 && body + 26 <= size) {
        if (body + 26 > have)
          throw new WavNeedMore(body + 26);
        fmt = view.getUint16(body + 24, true);
      }
      haveFmt = true;
    } else if (cid === "data") {
      if (!haveFmt)
        throw new WavError("'data' chunk appears before 'fmt ' - the parser needs fmt first");
      return new WavInfo(fmt, channels, rate, bits, body, Math.min(csize, size - body));
    }
    pos = body + csize + (csize & 1);
  }
  throw new WavError(`no 'data' chunk within the first ${MAX_CHUNKS} chunks`);
}
function pack(samples, encoding) {
  const n = samples.length;
  if (encoding === F32) {
    const out2 = new Uint8Array(n * 4);
    const view2 = new DataView(out2.buffer);
    for (let i = 0;i < n; i++)
      view2.setFloat32(i * 4, samples[i], true);
    return out2;
  }
  const out = new Uint8Array(n * 2);
  const view = new DataView(out.buffer);
  for (let i = 0;i < n; i++) {
    const s = Math.max(-1, Math.min(1, samples[i]));
    view.setInt16(i * 2, Math.trunc(s * 32767), true);
  }
  return out;
}
function writeWav(samples, rate, channels, encoding) {
  const body = pack(samples, encoding);
  const bits = encoding === F32 ? 32 : 16;
  const fmt = encoding === F32 ? WAVE_FORMAT_FLOAT : WAVE_FORMAT_PCM;
  const blockAlign = channels * bits / 8;
  const out = new Uint8Array(44 + body.length);
  const view = new DataView(out.buffer);
  const tag = (off, s) => {
    for (let i = 0;i < 4; i++)
      view.setUint8(off + i, s.charCodeAt(i));
  };
  tag(0, "RIFF");
  view.setUint32(4, 36 + body.length, true);
  tag(8, "WAVE");
  tag(12, "fmt ");
  view.setUint32(16, 16, true);
  view.setUint16(20, fmt, true);
  view.setUint16(22, channels, true);
  view.setUint32(24, rate, true);
  view.setUint32(28, rate * blockAlign, true);
  view.setUint16(32, blockAlign, true);
  view.setUint16(34, bits, true);
  tag(36, "data");
  view.setUint32(40, body.length, true);
  out.set(body, 44);
  return out;
}
function writeRaw(samples) {
  return pack(samples, INT16);
}
function padToBytes(samples, minBytes, bytesPerSample) {
  if (!samples.length)
    return samples;
  const need = Math.ceil(minBytes / bytesPerSample);
  if (samples.length >= need)
    return samples;
  const out = new Float64Array(need);
  for (let i = 0;i < need; i++)
    out[i] = samples[i % samples.length];
  return out;
}

// src/core/convert.ts
function encodeForBank(layout, bank, samples, rate) {
  const notes = [];
  const channels = bank.fmt.channels ?? 1;
  const encoding = bank.fmt.encodings[0];
  let out = samples;
  if (bank.max_seconds) {
    const cap = Math.floor(bank.max_seconds * rate * channels);
    if (out.length > cap) {
      notes.push(`trimmed to ${bank.max_seconds.toFixed(0)} s (${bank.engine} loads into RAM)`);
      out = out.slice(0, cap);
    }
  }
  if (bank.scanned) {
    const bps = encoding === INT16 ? 2 : 4;
    const before = out.length;
    out = padToBytes(out, layout.scan.min_bytes, bps);
    if (out.length > before) {
      notes.push(`looped up to ${layout.scan.min_bytes / 1024} KB - shorter files are skipped by the ` + "directory scan");
    }
  }
  const bytes = bank.fmt.container === "raw" ? writeRaw(out) : writeWav(out, rate, channels, encoding);
  return { bytes, notes };
}
async function convertOne(layout, bank, decoder, input, opts) {
  if (!bank.target) {
    throw new Error(`the ${bank.engine} engine reads text patches, not audio (${bank.fmt.describe})`);
  }
  const rate = bank.fmt.rate ?? opts.rate ?? 48000;
  const channels = bank.fmt.channels ?? 1;
  const { samples, sourceRate, sourceChannels } = await decoder.decode(input.data, rate, channels);
  const { bytes, notes } = encodeForBank(layout, bank, samples, rate);
  const path = formatTarget(bank.target, opts.index, {
    deck: opts.deck ?? "a",
    bank: opts.bank ?? 0,
    tape: opts.tape ?? "B"
  });
  return { path, bytes, notes, sourceRate, sourceChannels };
}
function targetSummary(bank, rate) {
  const enc = bank.fmt.encodings[0] === F32 ? "32-bit float" : "16-bit PCM";
  const container = bank.fmt.container === "raw" ? "headerless .raw" : ".wav";
  const ch = bank.fmt.channels === 2 ? "stereo" : "mono";
  return `${container}, ${enc}, ${ch}, ${bank.fmt.rate ?? rate} Hz`;
}

// src/app/convert_model.ts
class ConvertModel {
  layout;
  deps;
  store;
  constructor(layout, deps) {
    this.layout = layout;
    this.deps = deps;
    const first = layout.audioBanks()[0];
    this.store = new Store({
      engine: first ? first.engine : "",
      deck: "a",
      bank: 0,
      tape: layout.granularTapes[0] ?? "B",
      slot: 1,
      rate: 48000,
      files: [],
      results: [],
      status: "",
      error: null,
      busy: false
    });
    this.setEngine(this.store.get().engine);
  }
  banks() {
    return this.layout.audioBanks();
  }
  bank() {
    return this.layout.bank(this.store.get().engine);
  }
  fields() {
    const t = this.bank().target;
    return {
      deck: t.includes("{deck}"),
      bank: t.includes("{bank}"),
      tape: t.includes("{tape}"),
      rate: this.bank().fmt.rate == null
    };
  }
  summary() {
    const b = this.bank();
    return `Writes ${targetSummary(b, this.store.get().rate)}` + (b.max_seconds ? ` - trimmed to ${b.max_seconds} s, because this engine loads into RAM` : "") + (b.scanned ? `, looped up to ${this.layout.scan.min_bytes / 1024} KB if shorter` : "");
  }
  setEngine(engine) {
    const bank = this.layout.bank(engine);
    if (!bank)
      return;
    const rate = bank.fmt.rate ?? (bank.engine === "bard" ? 24000 : 48000);
    this.store.set({ engine, rate, results: [], status: "" });
  }
  setField(key, value) {
    this.store.set({ [key]: value });
  }
  addFiles(files) {
    const next = [...this.store.get().files, ...files];
    this.store.set({ files: next, results: [], status: this.describeQueue(next) });
  }
  removeFile(index) {
    const next = this.store.get().files.filter((_, i) => i !== index);
    this.store.set({ files: next, results: [], status: this.describeQueue(next) });
  }
  describeQueue(files) {
    return files.length ? `${files.length} file(s) -> ${this.store.get().engine}, starting at slot ${this.store.get().slot}` : "";
  }
  canConvert() {
    return this.store.get().files.length > 0 && !this.store.get().busy;
  }
  canSaveToCard() {
    return this.store.get().results.length > 0 && this.deps.access.hasDirectAccess();
  }
  async convert() {
    const s = this.store.get();
    const bank = this.bank();
    const results = [];
    this.store.set({ busy: true, results: [], error: null, status: "Decoding..." });
    try {
      for (let i = 0;i < s.files.length; i++) {
        const f = s.files[i];
        this.store.set({ status: `Decoding ${f.name} (${i + 1}/${s.files.length})...` });
        results.push(await convertOne(this.layout, bank, this.deps.decoder, { name: f.name, data: await f.bytes() }, { index: s.slot + i, deck: s.deck, bank: s.bank, tape: s.tape, rate: s.rate }));
      }
      this.store.set({ busy: false, results, status: `${results.length} file(s) converted` });
    } catch (e) {
      this.store.set({ busy: false, status: "", results: [], error: e.message });
    }
  }
  async downloadZip() {
    const results = this.store.get().results;
    const bytes = await makeZip(results.map((r) => ({ path: r.path, bytes: r.bytes })), [], this.deps.deflate);
    this.deps.downloader.save(bytes, `sk-${this.store.get().engine}-files.zip`, ZIP_MIME);
  }
  async saveToCard() {
    try {
      const card = await this.deps.access.pickDirectory("readwrite");
      const files = this.store.get().results.map((r) => ({ path: r.path, bytes: r.bytes }));
      const { written, failed } = await this.deps.access.writeInto(card.handle, files);
      this.store.set({
        status: `${written.length} written to the card, ${failed.length} failed`,
        error: failed.length ? failed.map((f) => `${f.path}: ${f.error}`).join(`
`) : null
      });
      return true;
    } catch (e) {
      const err = e;
      if (err.name === "AbortError")
        return false;
      this.store.set({ error: err.message });
      return false;
    }
  }
}

// src/platform/audio.ts
async function decodeTo(data, rate, channels) {
  if (typeof OfflineAudioContext === "undefined") {
    throw new Error("this browser has no Web Audio API, so it cannot decode audio");
  }
  const decodeCtx = new OfflineAudioContext(1, 1, rate);
  const decoded = await decodeCtx.decodeAudioData(data);
  let buf = decoded;
  if (decoded.numberOfChannels !== channels) {
    const mixCtx = new OfflineAudioContext(channels, decoded.length, rate);
    const src = mixCtx.createBufferSource();
    src.buffer = decoded;
    src.connect(mixCtx.destination);
    src.start();
    buf = await mixCtx.startRendering();
  }
  const n = buf.length;
  const out = new Float32Array(n * channels);
  for (let c = 0;c < channels; c++) {
    const chan = buf.getChannelData(c);
    for (let i = 0;i < n; i++)
      out[i * channels + c] = chan[i];
  }
  return {
    samples: out,
    rate,
    channels,
    sourceRate: decoded.sampleRate,
    sourceChannels: decoded.numberOfChannels
  };
}
var browserDecoder = { decode: decodeTo };

// src/ui/convert_view.ts
var inputFor = (f) => ({
  name: f.name,
  size: f.size,
  bytes: () => f.arrayBuffer()
});
function mountConvert(root, ctx) {
  const model = new ConvertModel(ctx.layout, {
    decoder: browserDecoder,
    access: cardAccess,
    downloader,
    deflate: deflateRaw
  });
  const status = el("div", { class: "status" });
  const out = el("div", { class: "results" });
  const fileList = el("ul", { class: "filelist" });
  const targetNote = el("div", { class: "muted note" });
  const field = (label, control) => el("label", { class: "field" }, el("span", {}, label), control);
  const engineSel = el("select", { onchange: () => model.setEngine(engineSel.value) }, model.banks().map((b) => el("option", { value: b.engine }, b.engine)));
  const deckSel = el("select", { onchange: () => model.setField("deck", deckSel.value) }, [el("option", {}, "a"), el("option", {}, "b")]);
  const bankSel = el("select", { onchange: () => model.setField("bank", Number(bankSel.value)) }, Array.from({ length: 16 }, (_, i) => el("option", {}, String(i))));
  const tapeSel = el("select", { onchange: () => model.setField("tape", tapeSel.value) }, ctx.layout.granularTapes.map((t) => el("option", {}, t)));
  const slotInput = el("input", {
    type: "number",
    min: "0",
    max: "48",
    value: "1",
    class: "slot",
    oninput: () => model.setField("slot", Number(slotInput.value) || 0)
  });
  const rateInput = el("input", {
    type: "number",
    min: "3000",
    max: "96000",
    value: "48000",
    class: "slot",
    oninput: () => model.setField("rate", Number(rateInput.value) || 48000)
  });
  const deckField = field("Deck", deckSel);
  const bankField = field("Bank / shelf", bankSel);
  const tapeField = field("Tape", tapeSel);
  const slotField = field("First slot", slotInput);
  const rateField = field("Sample rate", rateInput);
  const convertBtn = el("button", { class: "primary", disabled: true, onclick: () => model.convert() }, "Convert");
  const zipBtn = el("button", { disabled: true, onclick: () => model.downloadZip() }, "Download as .zip");
  const saveBtn = el("button", { disabled: true, onclick: () => model.saveToCard() }, "Save onto the card");
  const drop = el("div", { class: "dropzone" }, el("p", {}, "Drop audio files here"), el("p", { class: "muted" }, "mp3, flac, wav, ogg, m4a - decoded by the browser, nothing uploaded"));
  dropTarget(drop, (dt) => model.addFiles([...dt.files].map(inputFor)));
  const picker = el("input", {
    type: "file",
    multiple: true,
    accept: "audio/*",
    class: "hidden",
    onchange: (e) => {
      const files = e.target.files;
      if (files)
        model.addFiles([...files].map(inputFor));
    }
  });
  model.store.subscribe((s) => {
    const fields = model.fields();
    deckField.hidden = !fields.deck;
    bankField.hidden = !fields.bank;
    tapeField.hidden = !fields.tape;
    rateField.hidden = !fields.rate;
    if (rateInput.value !== String(s.rate))
      rateInput.value = String(s.rate);
    targetNote.textContent = model.summary();
    status.textContent = s.status;
    clear(fileList);
    s.files.forEach((f, i) => {
      fileList.append(el("li", {}, el("span", { class: "name" }, f.name), el("span", { class: "muted" }, humanBytes(f.size)), el("button", { class: "link", onclick: () => model.removeFile(i) }, "remove")));
    });
    convertBtn.disabled = !model.canConvert();
    zipBtn.disabled = s.results.length === 0;
    saveBtn.disabled = !model.canSaveToCard();
    clear(out);
    if (s.error) {
      out.append(finding("error", "", `Could not decode: ${s.error}`, "The browser decodes mp3, flac, wav, ogg and m4a. A DRM-protected or unusual file may need " + "converting with ffmpeg first."));
    }
    for (const r of s.results) {
      out.append(finding("ok", `${r.path}`, `${humanBytes(r.bytes.length)}, from ${r.sourceRate} Hz ` + `${r.sourceChannels === 2 ? "stereo" : "mono"}`, r.notes.length ? r.notes.join("; ") : undefined));
    }
  });
  mountPoint(root).append(el("div", { class: "fields" }, field("Engine", engineSel), deckField, bankField, tapeField, slotField, rateField), targetNote, el("div", { class: "controls" }, el("button", { onclick: () => picker.click() }, "Choose audio files"), convertBtn, zipBtn, saveBtn, picker), drop, fileList, status, out);
}

// src/core/verify.ts
var HEADER_PREFIX = 64 * 1024;
var finding2 = (level, path, problem, fix = "") => ({ level, path, problem, fix });
var suffixOf = (name) => {
  const i = name.lastIndexOf(".");
  return i <= 0 ? "" : name.slice(i);
};
var stemOf = (name) => {
  const i = name.lastIndexOf(".");
  return i <= 0 ? name : name.slice(0, i);
};
var dirOf = (path) => {
  const i = path.lastIndexOf("/");
  return i < 0 ? "" : path.slice(0, i);
};
var baseOf = (path) => path.slice(path.lastIndexOf("/") + 1);
function shortNameSuggestion(name) {
  const stem = [...stemOf(name)].filter((c) => /[0-9A-Za-z]/.test(c)).join("").toUpperCase().slice(0, 8);
  return `${stem || "TRACK01"}${suffixOf(name).toUpperCase()}`;
}
function checkScanVisibility(layout, entry, bank, out) {
  const name = baseOf(entry.path);
  const rel = entry.path;
  let ok = true;
  if (name.startsWith(".")) {
    out.push(finding2("warn", rel, "name starts with a dot, so the scan skips it", "This is usually a macOS metadata stub (._NAME or .DS_Store). Delete it; on macOS use " + "`dot_clean` on the card before ejecting."));
    return false;
  }
  const max = layout.scan.max_name;
  if (name.length > max) {
    ok = false;
    out.push(finding2("error", rel, `filename is ${name.length} characters; the scan skips anything over ${max}, so this file is ` + "INVISIBLE to the device", `Rename to ${max} characters or fewer including the extension (e.g. ${shortNameSuggestion(name)}). ` + "For a whole library, scripts/prepare_audiobooks.py does the renaming and records the real " + "titles in BOOKS.TXT."));
  }
  const ext = suffixOf(name);
  if (!name.includes(".") || !layout.scan.extensions.includes(ext.toLowerCase().replace(/^\./, ""))) {
    ok = false;
    if (layout.isSourceExt(ext)) {
      out.push(finding2("error", rel, `${ext} is a compressed/unsupported source format - the firmware has no decoder, and the scan ` + "only indexes .raw/.wav", `Convert it on the Convert tab, or: sk_card.py convert --engine ${bank.engine} CARD ${name}`));
    } else {
      out.push(finding2("error", rel, `extension ${ext || "(none)"} is not indexed by the scan`, `Use .raw or .wav (${bank.accepts.describe}).`));
    }
  }
  const floor = layout.scan.min_bytes;
  if (entry.size < floor) {
    ok = false;
    out.push(finding2("error", rel, `file is ${(entry.size / 1024).toFixed(1)} KB; the scan skips anything under ${floor / 1024} KB, ` + "so this file is INVISIBLE to the device", "Make the clip longer (the floor exists to drop macOS metadata stubs, and catches genuinely " + "short clips too)."));
  }
  return ok;
}
async function parseHeader(entry) {
  const prefix = await entry.read(Math.min(entry.size, HEADER_PREFIX));
  try {
    return parseWav(prefix, entry.size);
  } catch (e) {
    if (!(e instanceof WavNeedMore))
      throw e;
    return parseWav(await entry.read(), entry.size);
  }
}
async function checkAudioFormat(layout, entry, bank, out) {
  const rel = entry.path;
  const acc = bank.accepts;
  const ext = suffixOf(baseOf(rel)).toLowerCase();
  if (acc.containers.includes("raw") && ext === ".raw") {
    if (entry.size % 2) {
      out.push(finding2("warn", rel, "odd byte count for a 16-bit format (last frame is partial)", "Harmless - the firmware floors to a whole frame - but usually means the file was truncated " + "or is not actually int16."));
    }
    return;
  }
  let info;
  try {
    info = await parseHeader(entry);
  } catch (e) {
    if (!(e instanceof WavError))
      throw e;
    out.push(finding2("error", rel, `the firmware's WAV parser would reject this file: ${e.message}`, `Re-encode it: sk_card.py convert --engine ${bank.engine} CARD ${baseOf(rel)}`));
    return;
  }
  const problems = [];
  if (!info.encoding || !acc.encodings.includes(info.encoding)) {
    problems.push(`the firmware cannot decode ${info.describe().split(",")[0]}`);
  }
  if (info.channels > acc.max_channels) {
    problems.push(`${info.channels} channels is past the ${acc.max_channels}-channel downmix bound`);
  }
  if (acc.rate != null && info.rate !== acc.rate) {
    problems.push(`${info.rate} Hz, and nothing on this path resamples`);
  } else if (info.rate < layout.data.rate_bounds.min || info.rate > layout.data.rate_bounds.max) {
    problems.push(`${info.rate} Hz is outside the ${layout.data.rate_bounds.min}..` + `${layout.data.rate_bounds.max} Hz the resampler takes`);
  }
  if (problems.length) {
    out.push(finding2("error", rel, `this will not load (${problems.join("; ")})`, `Accepts: ${acc.describe}. Fix it on the Convert tab, or: sk_card.py convert --engine ` + `${bank.engine} CARD ${baseOf(rel)}`));
    return;
  }
  if (bank.max_seconds && info.seconds > bank.max_seconds * 1.02) {
    out.push(finding2("warn", rel, `${info.seconds.toFixed(0)} s exceeds the ~${bank.max_seconds.toFixed(0)} s this engine holds in RAM`, "It will load truncated. Trim it, or use the tape engine, which streams."));
  }
}
function checkSlotName(layout, entry, bank, out) {
  const rel = entry.path;
  const name = baseOf(rel);
  const names = new Map(bank.slots.map((s) => [s.toLowerCase(), s]));
  if (!names.has(name.toLowerCase())) {
    const ext = suffixOf(name);
    if (layout.isSourceExt(ext)) {
      out.push(finding2("error", rel, `${ext} is a compressed/unsupported source format - the firmware has no decoder and never ` + "opens this file", `Convert it on the Convert tab, or: sk_card.py convert --engine ${bank.engine} CARD ${name}`));
    } else {
      out.push(finding2("warn", rel, "not one of this engine's slot filenames, so it is never opened", `Expected one of: ${bank.slots.slice(0, 6).join(", ")}${bank.slots.length > 6 ? " ..." : ""}`));
    }
    return false;
  }
  const canonical = names.get(name.toLowerCase());
  const isUpper = canonical !== canonical.toLowerCase() && canonical === canonical.toUpperCase();
  if (name !== canonical && isUpper) {
    out.push(finding2("warn", rel, `name is ${name}, documented as ${canonical}`, "FAT is case-insensitive so this generally still opens, but match the documented case to be safe."));
  }
  return true;
}
async function checkConfig(layout, entry, out) {
  const rel = "SK/config.txt";
  const text = new TextDecoder("utf-8", { fatal: false }).decode(await entry.read());
  const lines = text.split(/\r\n|\r|\n/).map((l) => l.trim()).filter(Boolean);
  if (lines.some((l) => l.includes("="))) {
    out.push(finding2("error", rel, "looks like `key=value`, but the parser expects the property name and its value on separate lines", `Write:
    pre_load
    1`));
    return;
  }
  const known = layout.configProperties;
  for (let i = 0;i + 1 < lines.length; i += 2) {
    const key = lines[i];
    const val = lines[i + 1];
    if (!(key in known)) {
      out.push(finding2("warn", rel, `unknown property '${key}'`, `Known: ${Object.keys(known).join(", ")}`));
      continue;
    }
    const [lo, hi] = known[key];
    const digits = val.replace(/^-+/, "");
    const numeric = digits.length > 0 && /^[0-9]+$/.test(digits);
    if (!numeric || !(Number(val) >= lo && Number(val) <= hi)) {
      out.push(finding2("error", rel, `${key} = '${val}' is outside ${lo}..${hi}`, `Set a value in ${lo}..${hi}.`));
    }
  }
  if (lines.length % 2) {
    out.push(finding2("warn", rel, "odd number of lines - the last property has no value", "Every property name needs a value on the following line."));
  }
}
async function verifyCard(layout, card) {
  const out = [];
  const files = card.files.filter((f) => !f.path.split("/").slice(0, -1).some((c) => layout.isSkippedDir(c)));
  const dirs = new Set(card.dirs ?? []);
  for (const f of files) {
    let d = dirOf(f.path);
    while (d) {
      dirs.add(d);
      d = dirOf(d);
    }
  }
  const present = layout.allDirs.filter((d) => dirs.has(d));
  if (!present.length) {
    out.push(finding2("error", ".", "no recognised engine folders found here", "Is this the card's root? Build a fresh one on the Build tab, or: sk_card.py init CARD"));
    return out;
  }
  const byDir = new Map;
  for (const f of files) {
    const d = dirOf(f.path);
    let list = byDir.get(d);
    if (!list)
      byDir.set(d, list = []);
    list.push(f);
  }
  const seenBanks = new Set;
  let configEntry = null;
  for (const dir of [...byDir.keys()].sort()) {
    const bank = dir ? layout.bankForPath(dir) : null;
    let counted = 0;
    for (const entry of byDir.get(dir).sort((a, b) => a.path < b.path ? -1 : 1)) {
      const name = baseOf(entry.path);
      if (entry.path === "SK/config.txt")
        configEntry = entry;
      if (!bank) {
        if (!entry.path.startsWith(".") && dir === "" && name.toUpperCase() !== "README.TXT") {
          out.push(finding2("warn", entry.path, "file in the card root belongs to no engine", "Harmless, but the device never reads it."));
        }
        continue;
      }
      seenBanks.add(bank.engine);
      if (layout.isSidecar(name))
        continue;
      if (bank.fmt.container === "text") {
        const slots = new Set(bank.slots.map((s) => s.toLowerCase()));
        if (bank.slots.length && !slots.has(name.toLowerCase())) {
          out.push(finding2("warn", entry.path, "not a slot the engine loads", `Expected ${bank.slots.join(", ")}`));
        }
        continue;
      }
      if (bank.scanned) {
        if (name.toUpperCase().endsWith(".TXT"))
          continue;
        if (checkScanVisibility(layout, entry, bank, out)) {
          await checkAudioFormat(layout, entry, bank, out);
          counted++;
        }
      } else if (checkSlotName(layout, entry, bank, out)) {
        await checkAudioFormat(layout, entry, bank, out);
        counted++;
      }
    }
    if (bank && bank.scanned && bank.max_files && counted > bank.max_files) {
      out.push(finding2("warn", dir, `${counted} playable files but only the first ${bank.max_files} (alphabetically) are indexed`, `Move the rest to another ${bank.engine} folder.`));
    }
  }
  if (configEntry)
    await checkConfig(layout, configEntry, out);
  for (const bank of layout.banks) {
    if (bank.fmt.container === "text" || seenBanks.has(bank.engine))
      continue;
    if (bank.dirs.some((d) => dirs.has(d))) {
      out.push(finding2("warn", bank.dirs[0], `no files for the ${bank.engine} engine`, `${bank.blurb.split(".")[0]}.`));
    }
  }
  return out;
}
function summarize(findings) {
  const errors = findings.filter((f) => f.level === "error");
  const warns = findings.filter((f) => f.level === "warn");
  return { errors, warns, ok: errors.length === 0 };
}

// src/app/verify_model.ts
var INITIAL2 = {
  status: "",
  findings: [],
  summary: null,
  checked: false,
  editable: false,
  fileCount: 0,
  totalBytes: 0,
  error: null,
  busy: false
};

class VerifyModel {
  layout;
  store = new Store({ ...INITIAL2 });
  constructor(layout) {
    this.layout = layout;
  }
  async run(getCard) {
    this.store.set({ ...INITIAL2, busy: true, status: "Reading the card..." });
    try {
      const card = await getCard();
      this.store.set({ status: `Checking ${card.files.length} files...` });
      const findings = await verifyCard(this.layout, card);
      const totalBytes = card.files.reduce((n, f) => n + f.size, 0);
      this.store.set({
        busy: false,
        checked: true,
        editable: card.handle != null,
        fileCount: card.files.length,
        totalBytes,
        findings,
        summary: summarize(findings),
        status: ""
      });
    } catch (e) {
      const err = e;
      this.store.set({ busy: false, status: "" });
      if (err.name !== "AbortError")
        this.store.set({ error: err.message });
    }
  }
}

// src/ui/verify_view.ts
var engineBanks = (layout) => layout.banks.filter((b) => b.kind !== "config").length;
var audioFormats = (layout) => new Set(layout.banks.filter((b) => b.fmt.container !== "text").map((b) => b.fmt.describe)).size;
function mountVerify(root, ctx) {
  const model = new VerifyModel(ctx.layout);
  const results = el("div", { class: "results" });
  const status = el("div", { class: "status" });
  const drop = el("div", { class: "dropzone" }, el("p", {}, "Drop the card folder here"), el("p", { class: "muted" }, "or pick it below. Nothing is uploaded - the check runs in your browser."));
  const pickBtn = el("button", {
    class: "primary",
    onclick: () => model.run(() => pickDirectory("read"))
  }, "Choose card folder");
  const fileInput = el("input", {
    type: "file",
    webkitdirectory: "",
    multiple: true,
    class: "hidden",
    onchange: (e) => {
      const files = e.target.files;
      if (files?.length)
        model.run(async () => fromFileList(files));
    }
  });
  const browseBtn = el("button", { onclick: () => fileInput.click() }, "Browse for folder");
  dropTarget(drop, (dt) => model.run(() => fromDataTransfer(dt)));
  model.store.subscribe((s) => {
    clear(results);
    if (s.busy || !s.checked) {
      status.textContent = s.status;
      if (s.error)
        results.append(finding("error", "", s.error));
      return;
    }
    status.textContent = `${s.fileCount} files, ${humanBytes(s.totalBytes)}` + `${s.editable ? " - this card can be edited in place" : ""}`;
    const { errors, warns, ok } = s.summary;
    if (ok && !warns.length) {
      results.append(el("div", { class: "verdict good" }, el("strong", {}, "No problems found."), el("p", {}, "Every file present is in a format the firmware accepts.")));
      return;
    }
    results.append(el("div", { class: `verdict ${errors.length ? "bad" : "mixed"}` }, el("strong", {}, `${errors.length} error${errors.length === 1 ? "" : "s"}, ` + `${warns.length} warning${warns.length === 1 ? "" : "s"}`), el("p", {}, errors.length ? "Anything under WILL NOT WORK is silently ignored or misread by the device." : "Nothing is broken, but these are probably not what you meant.")));
    for (const [group, label, cls] of [
      [errors, "WILL NOT WORK", "error"],
      [warns, "Worth checking", "warn"]
    ]) {
      if (!group.length)
        continue;
      results.append(el("h3", {}, `${label} (${group.length})`));
      for (const f of group)
        results.append(finding(cls, f.path, f.problem, f.fix));
    }
  });
  const controls = el("div", { class: "controls" }, pickBtn, browseBtn, fileInput);
  mountPoint(root).append(controls, drop, status, results);
  fill(root, "banks", String(engineBanks(ctx.layout)));
  fill(root, "formats", String(audioFormats(ctx.layout)));
  fill(root, "maxname", String(ctx.layout.scan.max_name));
  if (!hasFileSystemAccess()) {
    pickBtn.disabled = true;
    pickBtn.title = "This browser has no File System Access API";
    controls.append(el("span", { class: "muted note" }, "In-place card access needs Chrome or Edge; dropping a folder works here."));
  }
}

// src/app/reference_model.ts
class ReferenceModel {
  layout;
  store = new Store({ query: "", pinned: null, showSources: false });
  items;
  constructor(layout, catalogue) {
    this.layout = layout;
    this.items = catalogue.entries.map((entry) => ({
      entry,
      haystack: [
        entry.doc.name,
        entry.doc.title,
        entry.doc.summary,
        entry.doc.body,
        entry.bank ? [
          entry.bank.dirs.join(" "),
          entry.bank.readers.join(" "),
          entry.bank.fmt.describe,
          entry.bank.blurb,
          entry.bank.slots.join(" "),
          entry.bank.target
        ].join(" ") : "no card needed"
      ].join(" ").toLowerCase()
    }));
  }
  setQuery(query) {
    this.store.set({ query, pinned: null });
  }
  toggleChip(engine) {
    const pinned = this.store.get().pinned === engine ? null : engine;
    this.store.set({ pinned, query: "" });
  }
  select(engine) {
    if (!this.items.some((i) => i.entry.doc.name === engine))
      return;
    this.store.set({ pinned: engine, query: "" });
  }
  setShowSources(on) {
    this.store.set({ showSources: on });
  }
  visible() {
    const { query, pinned } = this.store.get();
    const q = query.trim().toLowerCase();
    return this.items.filter((i) => pinned ? i.entry.doc.name === pinned : !q || i.haystack.includes(q)).map((i) => i.entry);
  }
  status() {
    const { query, pinned } = this.store.get();
    const shown = this.visible().length;
    const readers = this.items.filter((i) => i.entry.bank).length;
    return pinned || query.trim() ? `${shown} of ${this.items.length} shown` : `${this.items.length} engines, ${readers} of them read the card`;
  }
  scan() {
    return this.layout.scan;
  }
}

// src/ui/reference_view.ts
var range = ([lo, hi]) => lo === hi ? String(lo) : `${lo}-${hi}`;
var extList = (exts) => exts.map((e) => `.${e}`).join("/");
var seconds = (s) => Number.isInteger(s) ? String(s) : s.toFixed(1);
var slotList = (slots, shown = 5) => slots.length <= shown ? slots.join(", ") : `${slots.slice(0, shown).join(", ")} ... +${slots.length - shown} more`;
function specRow(label, value, mono = false, cls) {
  return el("tr", { class: cls ?? null }, el("th", {}, label), el("td", { class: mono ? "mono" : null }, value));
}
function everywhere(scan) {
  return el("div", { class: "callout" }, el("strong", {}, "Every scanned folder: "), `at most ${scan.max_name} characters in the name, ending ${extList(scan.extensions)}, ` + `at least ${scan.min_bytes / 1024} KB. `, el("span", { class: "muted" }, "Break any of these and the file is skipped silently; get the format wrong and it plays as " + "noise. Verify finds both."));
}
function configTable(layout) {
  return el("table", { class: "layout" }, el("thead", {}, el("tr", {}, el("th", {}, "Property"), el("th", {}, "Range"))), el("tbody", {}, Object.entries(layout.configProperties).map(([k, v]) => el("tr", {}, el("td", { class: "mono" }, k), el("td", { class: "mono" }, range(v))))));
}
function engineSection(layout, entry) {
  const { doc, bank } = entry;
  const rows = [];
  if (bank) {
    rows.push(specRow("Format", bank.fmt.describe));
    if (bank.slots.length) {
      const row = specRow(`Names (${bank.slots.length})`, slotList(bank.slots), true);
      row.querySelector("td").title = bank.slots.join(", ");
      rows.push(row);
    }
    if (bank.scanned) {
      rows.push(specRow("Scanned", `any name of at most ${layout.scan.max_name} characters ending ` + `${extList(layout.scan.extensions)}, at least ${layout.scan.min_bytes / 1024} KB` + `${bank.max_files ? `, at most ${bank.max_files} per folder` : ""}`));
    }
    if (bank.max_seconds) {
      rows.push(specRow("Length", `about ${seconds(bank.max_seconds)} s at most - this engine loads ` + "the whole file into RAM, so anything longer is trimmed"));
    }
    for (const name of bank.sidecars) {
      const label = bank.kind === "config" ? "File" : "Also needs";
      const dflt = bank.extras[name];
      rows.push(specRow(label, dflt ? `${name} - defaults to ${JSON.stringify(dflt.trim())}` : name, true));
    }
    if (bank.target)
      rows.push(specRow("Convert writes", bank.target, true));
    if (bank.readers.length > 1)
      rows.push(specRow("Read by", bank.readers.join(", ")));
    rows.push(specRow("Firmware", bank.source, true, "src"));
  }
  return el("section", { class: "ref-bank", dataset: { engine: doc.name } }, el("h3", {}, doc.name, " ", el("span", { class: "mono muted" }, bank ? folderLabel(bank.dirs) : "needs no card")), doc.summary && el("p", { class: "summary" }, doc.summary), doc.body && el("p", {}, doc.body), el("p", { class: "muted note" }, !doc.released && doc.doc && "(not in the released set) ", doc.page ? el("a", { href: `#engine/${doc.name}` }, "Open the manual") : null), rows.length ? el("table", { class: "layout spec" }, el("tbody", {}, rows)) : null, bank && bank.kind === "config" ? configTable(layout) : null);
}
function mountReference(root, ctx) {
  const model = new ReferenceModel(ctx.layout, ctx.engines);
  const status = el("div", { class: "status" });
  const sections = new Map(ctx.engines.entries.map((e) => [e.doc.name, engineSection(ctx.layout, e)]));
  const filter = el("input", {
    type: "text",
    class: "filter",
    placeholder: "Filter by engine, folder or format",
    autocomplete: "off",
    oninput: () => model.setQuery(filter.value)
  });
  const chips = el("div", { class: "chips" }, ctx.engines.entries.map((e) => el("button", { class: "link", onclick: () => model.toggleChip(e.doc.name) }, e.doc.name)));
  const srcToggle = el("input", {
    type: "checkbox",
    onchange: () => model.setShowSources(srcToggle.checked)
  });
  const banksEl = el("div", { class: "ref-banks" }, [...sections.values()]);
  model.store.subscribe((s) => {
    const visible = new Set(model.visible().map((e) => e.doc.name));
    for (const [engine, node] of sections)
      node.hidden = !visible.has(engine);
    for (const chip of [...chips.children]) {
      chip.classList.toggle("on", chip.textContent === s.pinned);
    }
    if (filter.value !== s.query)
      filter.value = s.query;
    banksEl.classList.toggle("show-src", s.showSources);
    status.textContent = model.status();
  });
  ctx.engineFocus.subscribe(({ engine }) => {
    if (!engine)
      return;
    model.select(engine);
    sections.get(engine)?.scrollIntoView?.({ block: "start" });
  });
  mountPoint(root).append(el("div", { class: "controls" }, filter, el("label", { class: "field inline" }, srcToggle, el("span", {}, "firmware sources"))), chips, status, everywhere(model.scan()), banksEl);
}

// src/core/protocol.ts
var LOG_PREFIX = "[";
var isLog = (line) => line.startsWith(LOG_PREFIX);

class LineAssembler {
  buf = "";
  push(chunk) {
    this.buf += chunk;
    const out = [];
    let i;
    while ((i = this.buf.indexOf(`
`)) >= 0) {
      out.push(this.buf.slice(0, i).replace(/\r$/, ""));
      this.buf = this.buf.slice(i + 1);
    }
    return out;
  }
  get pending() {
    return this.buf;
  }
}

class CommandError extends Error {
  reason;
  constructor(reason) {
    super(reason);
    this.reason = reason;
  }
}

class Timeout extends Error {
}
function parseReply(line) {
  if (line === "ok")
    return { kind: "ok", value: "" };
  if (line.startsWith("ok "))
    return { kind: "ok", value: line.slice(3) };
  if (line.startsWith("err "))
    throw new CommandError(line.slice(4));
  throw new CommandError(`unexpected: ${JSON.stringify(line)}`);
}
var DESTRUCTIVE = [
  /^pad\s+clear\b/,
  /^seq\s+clear\b/,
  /^clear\b/,
  /^preset\s+save\b/,
  /^reset\b(?!\s+cpu\b)/
];
function isDestructive(line) {
  const s = line.trim().toLowerCase();
  return DESTRUCTIVE.some((re) => re.test(s));
}
function parseDescribe(lines) {
  const d = {
    engine: "",
    version: "",
    masked: false,
    params: new Map,
    configs: new Map,
    queries: new Map,
    caps: 0
  };
  for (const line of lines) {
    const tok = line.trim().split(/\s+/).filter(Boolean);
    if (!tok.length)
      continue;
    switch (tok[0]) {
      case "descr": {
        const kv = new Map(tok.slice(1).filter((t) => t.includes("=")).map((t) => {
          const i = t.indexOf("=");
          return [t.slice(0, i), t.slice(i + 1)];
        }));
        d.engine = kv.get("engine") ?? "";
        d.version = kv.get("version") ?? "";
        d.masked = kv.get("masked") === "1";
        break;
      }
      case "param": {
        if (tok.length < 4)
          break;
        const [lo, hi] = tok[3].split("..").map(Number);
        d.params.set(tok[1], { name: tok[1], scope: tok[2], lo, hi });
        break;
      }
      case "config": {
        if (tok.length < 2)
          break;
        d.configs.set(tok[1], { name: tok[1], values: enumValues(tok.slice(2)) });
        break;
      }
      case "query": {
        if (tok.length < 2)
          break;
        d.queries.set(tok[1], {
          name: tok[1],
          scope: tok.length > 2 ? tok[2] : "global",
          kind: tok.length > 3 ? tok[3] : "text",
          values: enumValues(tok.slice(4))
        });
        break;
      }
      case "caps":
        if (tok.length > 1)
          d.caps = parseInt(tok[1], 16) || 0;
        break;
      default:
        break;
    }
  }
  return d;
}
function enumValues(tokens) {
  const out = new Map;
  for (const t of tokens) {
    const i = t.indexOf(":");
    if (i < 0)
      continue;
    const k = Number(t.slice(0, i));
    if (Number.isInteger(k))
      out.set(k, t.slice(i + 1));
  }
  return out;
}
function vocabulary(desc) {
  const words = new Set([
    "set",
    "get",
    "param",
    "config",
    "query",
    "cv",
    "gate",
    "midi",
    "pad",
    "seq",
    "fx",
    "reset",
    "preset",
    "caps",
    "mode",
    "describe",
    "help",
    "note",
    "msg",
    "transport",
    "test",
    "run",
    "save",
    "load",
    "play",
    "rec",
    "stop",
    "clear",
    "trig",
    "arm",
    "disarm",
    "flux",
    "grit",
    "lock",
    "gritmode",
    "voct",
    "mix",
    "size",
    "xfade",
    "cpu",
    "A",
    "B"
  ]);
  for (const k of desc.params.keys())
    words.add(k);
  for (const k of desc.configs.keys())
    words.add(k);
  for (const k of desc.queries.keys())
    words.add(k);
  return [...words].sort();
}
function parseUsbDiag(reply) {
  return reply.trim().split(/\s+/).filter((t) => t.includes("=")).map((t) => {
    const i = t.indexOf("=");
    return { key: t.slice(0, i), value: t.slice(i + 1) };
  });
}

// src/core/device.ts
var DEFAULT_TIMEOUT_MS = 3000;

class Device {
  transport;
  timeout;
  logSink;
  lines = [];
  waiters = [];
  busy = null;
  constructor(transport, opts = {}) {
    this.transport = transport;
    this.timeout = opts.timeout ?? DEFAULT_TIMEOUT_MS;
    this.logSink = opts.logSink ?? null;
    transport.onLine((line) => this.push(line));
  }
  push(line) {
    const w = this.waiters.shift();
    if (w) {
      if (w.timer)
        clearTimeout(w.timer);
      w.resolve(line);
    } else {
      this.lines.push(line);
    }
  }
  readLine() {
    const buffered = this.lines.shift();
    if (buffered !== undefined)
      return Promise.resolve(buffered);
    return new Promise((resolve, reject) => {
      const waiter = { resolve, reject, timer: null };
      waiter.timer = setTimeout(() => {
        const i = this.waiters.indexOf(waiter);
        if (i >= 0)
          this.waiters.splice(i, 1);
        reject(new Timeout("no reply"));
      }, this.timeout);
      this.waiters.push(waiter);
    });
  }
  drainStale() {
    if (!this.lines.length)
      return;
    if (this.logSink)
      for (const ln of this.lines)
        this.logSink(`[stale] ${ln}`);
    this.lines = [];
  }
  async exclusive(fn) {
    const prev = this.busy ?? Promise.resolve();
    let release;
    this.busy = new Promise((r) => {
      release = r;
    });
    await prev.catch(() => {});
    try {
      return await fn();
    } finally {
      release();
    }
  }
  async readReply() {
    for (;; ) {
      const line = await this.readLine();
      if (isLog(line)) {
        if (this.logSink)
          this.logSink(line);
        continue;
      }
      return parseReply(line).value;
    }
  }
  cmd(line) {
    return this.exclusive(async () => {
      this.drainStale();
      await this.transport.write(`${line}\r
`);
      return this.readReply();
    });
  }
  setParam(name, deck, value) {
    return this.cmd(`set param ${name} ${deck} ${fmt(value)}`);
  }
  async getParam(name, deck) {
    return Number(await this.cmd(`get param ${name} ${deck}`));
  }
  async setConfig(name, deck, v) {
    return await this.cmd(`config ${name} ${deck} ${v}`) === "1";
  }
  cv(kind, deck, value) {
    return this.cmd(`cv ${kind} ${deck} ${fmt(value)}`);
  }
  gate(deck) {
    return this.cmd(`gate ${deck}`);
  }
  pad(action, deck, rev = false) {
    return this.cmd(`pad ${action} ${deck}${rev ? " rev" : ""}`);
  }
  fx(kind, deck, on) {
    return this.cmd(`fx ${kind} ${deck} ${on ? "on" : "off"}`);
  }
  query(name, deck = "") {
    return this.cmd(`query ${name} ${deck}`.trimEnd());
  }
  async caps() {
    return parseInt(await this.cmd("caps"), 16);
  }
  resetCpu() {
    return this.cmd("reset cpu");
  }
  async cpu() {
    const avg = Number(await this.query("cpu"));
    const min = Number(await this.query("cpumin"));
    const max = Number(await this.query("cpumax"));
    return { avg, min, max };
  }
  describeLines() {
    return this.exclusive(async () => {
      this.drainStale();
      await this.transport.write(`describe\r
`);
      const lines = [];
      for (;; ) {
        const line = await this.readLine();
        if (isLog(line)) {
          if (this.logSink)
            this.logSink(line);
          continue;
        }
        if (line === "end")
          return lines;
        lines.push(line);
      }
    });
  }
  testMode(on) {
    return this.cmd(`mode ${on ? "test" : "run"}`);
  }
  close() {
    for (const w of this.waiters) {
      if (w.timer)
        clearTimeout(w.timer);
      w.reject(new Timeout("closed"));
    }
    this.waiters = [];
    return this.transport.close();
  }
}
function fmt(v) {
  const n = Number(v);
  if (!Number.isFinite(n))
    throw new CommandError("bad-arg");
  if (Number.isInteger(n) && Math.abs(n) < 1e6)
    return String(n);
  return String(Number(n.toPrecision(6)));
}

// src/core/osc.ts
var END = 192;
var ESC = 219;
var ESC_END = 220;
var ESC_ESC = 221;
function slipEncode(payload) {
  const out = new Uint8Array(payload.length * 2 + 2);
  let n = 0;
  out[n++] = END;
  for (const b of payload) {
    if (b === END) {
      out[n++] = ESC;
      out[n++] = ESC_END;
    } else if (b === ESC) {
      out[n++] = ESC;
      out[n++] = ESC_ESC;
    } else {
      out[n++] = b;
    }
  }
  out[n++] = END;
  return out.subarray(0, n);
}

class SlipDecoder {
  buf = [];
  escaped = false;
  feed(data) {
    const frames = [];
    for (const b of data) {
      if (b === END) {
        if (this.buf.length)
          frames.push(Uint8Array.from(this.buf));
        this.buf = [];
        this.escaped = false;
      } else if (this.escaped) {
        this.buf.push(b === ESC_END ? END : b === ESC_ESC ? ESC : b);
        this.escaped = false;
      } else if (b === ESC) {
        this.escaped = true;
      } else {
        this.buf.push(b);
      }
    }
    return frames;
  }
  get pending() {
    return this.buf.length;
  }
}
var oscInt = (n) => ({ __osc: "i", value: Math.trunc(n) });
var isOscInt = (a) => typeof a === "object" && a !== null && a.__osc === "i";
var padded = (n) => n + 3 & ~3;
function encodeString(s) {
  const bytes = new Uint8Array(padded(s.length + 1));
  for (let i = 0;i < s.length; i++)
    bytes[i] = s.charCodeAt(i) & 127;
  return bytes;
}
function encode(address, ...args) {
  const addr = encodeString(address);
  if (!args.length)
    return addr;
  let tags = ",";
  let bodyLen = 0;
  for (const a of args) {
    if (typeof a === "boolean")
      tags += a ? "T" : "F";
    else if (isOscInt(a)) {
      tags += "i";
      bodyLen += 4;
    } else if (typeof a === "number") {
      tags += "f";
      bodyLen += 4;
    } else if (typeof a === "string") {
      tags += "s";
      bodyLen += padded(a.length + 1);
    } else
      throw new TypeError(`unsupported OSC argument: ${JSON.stringify(a)}`);
  }
  const tagBytes = encodeString(tags);
  const out = new Uint8Array(addr.length + tagBytes.length + bodyLen);
  out.set(addr, 0);
  out.set(tagBytes, addr.length);
  const view = new DataView(out.buffer, out.byteOffset);
  let off = addr.length + tagBytes.length;
  for (const a of args) {
    if (typeof a === "boolean")
      continue;
    if (isOscInt(a)) {
      view.setInt32(off, a.value, false);
      off += 4;
    } else if (typeof a === "number") {
      view.setFloat32(off, a, false);
      off += 4;
    } else {
      const s = encodeString(a);
      out.set(s, off);
      off += s.length;
    }
  }
  return out;
}
function readString(packet, off) {
  let end = off;
  while (end < packet.length && packet[end] !== 0)
    end++;
  if (end >= packet.length)
    throw new Error("malformed OSC packet: unterminated string");
  let s = "";
  for (let i = off;i < end; i++)
    s += String.fromCharCode(packet[i]);
  return [s, off + padded(end - off + 1)];
}
function decode(packet) {
  const [address, start] = readString(packet, 0);
  if (start >= packet.length)
    return { address, args: [] };
  const [tags, tagEnd] = readString(packet, start);
  const view = new DataView(packet.buffer, packet.byteOffset, packet.byteLength);
  const args = [];
  let off = tagEnd;
  for (const t of tags.slice(1)) {
    switch (t) {
      case "i":
        args.push(view.getInt32(off, false));
        off += 4;
        break;
      case "f":
        args.push(view.getFloat32(off, false));
        off += 4;
        break;
      case "d":
        args.push(view.getFloat64(off, false));
        off += 8;
        break;
      case "s":
      case "S": {
        const [s, next] = readString(packet, off);
        args.push(s);
        off = next;
        break;
      }
      case "T":
        args.push(true);
        break;
      case "F":
        args.push(false);
        break;
      default:
        throw new Error(`unsupported OSC type tag ${JSON.stringify(t)}`);
    }
  }
  return { address, args };
}
var BUNDLE = "#bundle\x00";
function isBundle(packet) {
  if (packet.length < 8)
    return false;
  for (let i = 0;i < 8; i++)
    if (packet[i] !== BUNDLE.charCodeAt(i))
      return false;
  return true;
}
function decodePacket(packet) {
  if (!isBundle(packet))
    return [decode(packet)];
  const view = new DataView(packet.buffer, packet.byteOffset, packet.byteLength);
  const out = [];
  let off = 16;
  while (off + 4 <= packet.length) {
    const size = view.getInt32(off, false);
    off += 4;
    if (size < 0 || off + size > packet.length)
      break;
    out.push(...decodePacket(packet.subarray(off, off + size)));
    off += size;
  }
  return out;
}

// src/core/oscdevice.ts
var DEV_QUERIES = new Set(["cpu", "cpumin", "cpumax", "usb"]);

class OscDevice {
  transport;
  timeout;
  logSink;
  frames = [];
  waiters = [];
  busy = null;
  globals = null;
  constructor(transport, opts = {}) {
    this.transport = transport;
    this.timeout = opts.timeout ?? DEFAULT_TIMEOUT_MS;
    this.logSink = opts.logSink ?? null;
    transport.onFrame((f) => this.push(f));
    if (opts.ack ?? true)
      this.send("/sk/dev/mode/ack", true);
  }
  push(frame) {
    const w = this.waiters.shift();
    if (w) {
      if (w.timer)
        clearTimeout(w.timer);
      w.resolve(frame);
    } else {
      this.frames.push(frame);
    }
  }
  readFrame() {
    const buffered = this.frames.shift();
    if (buffered !== undefined)
      return Promise.resolve(buffered);
    return new Promise((resolve, reject) => {
      const waiter = { resolve, reject, timer: null };
      waiter.timer = setTimeout(() => {
        const i = this.waiters.indexOf(waiter);
        if (i >= 0)
          this.waiters.splice(i, 1);
        reject(new Timeout("no reply"));
      }, this.timeout);
      this.waiters.push(waiter);
    });
  }
  drainStale() {
    if (!this.frames.length)
      return;
    if (this.logSink) {
      for (const f of this.frames) {
        const { address } = decode(f);
        this.logSink(`[stale] ${address}`);
      }
    }
    this.frames = [];
  }
  async exclusive(fn) {
    const prev = this.busy ?? Promise.resolve();
    let release;
    this.busy = new Promise((r) => {
      release = r;
    });
    await prev.catch(() => {});
    try {
      return await fn();
    } finally {
      release();
    }
  }
  send(address, ...args) {
    return this.transport.send(encode(address, ...args));
  }
  async readReply() {
    for (;; ) {
      const msg = decode(await this.readFrame());
      if (msg.address === "/sk/log") {
        if (this.logSink)
          this.logSink(String(msg.args[0] ?? ""));
        continue;
      }
      if (msg.address === "/sk/err") {
        throw new CommandError(String(msg.args.length > 1 ? msg.args[1] : "unknown"));
      }
      return msg;
    }
  }
  request(address, ...args) {
    return this.exclusive(async () => {
      this.drainStale();
      await this.send(address, ...args);
      const { args: vals } = await this.readReply();
      return vals.length === 1 ? vals[0] : vals;
    });
  }
  write(address, ...args) {
    return this.request(address, ...args);
  }
  async scope() {
    if (!this.globals) {
      const d = await this.describe();
      this.globals = {
        params: new Set([...d.params.values()].filter((p) => p.scope === "global").map((p) => p.name)),
        queries: new Set([...d.queries.values()].filter((q) => q.scope === "global").map((q) => q.name))
      };
    }
    return this.globals;
  }
  async prefix(deck, name, kind = "param") {
    if (name !== undefined) {
      const { params, queries } = await this.scope();
      const global = kind === "cfg" ? name === "route" : kind === "param" ? params.has(name) : queries.has(name);
      if (global)
        return "/sk";
    }
    return deck ? `/sk/${deck.toLowerCase()}` : "/sk";
  }
  async setParam(name, deck, value) {
    await this.write(`${await this.prefix(deck, name)}/param/${name}`, Number(value));
    return "";
  }
  async getParam(name, deck) {
    return Number(await this.request(`${await this.prefix(deck, name)}/param/${name}`));
  }
  async setConfig(name, deck, v) {
    await this.write(`${await this.prefix(deck, name, "cfg")}/cfg/${name}`, oscInt(Number(v)));
    return true;
  }
  async cv(kind, deck, value) {
    await this.write(`${await this.prefix(deck)}/cv/${kind}`, Number(value));
    return "";
  }
  async gate(deck) {
    await this.write(`${await this.prefix(deck)}/gate`);
    return "";
  }
  async midiNote(ch, note) {
    await this.write("/sk/midi/note", oscInt(ch), oscInt(note));
    return "";
  }
  async pad(action, deck, rev = false) {
    const addr = `${await this.prefix(deck)}/pad/${action}`;
    if (action === "play") {
      const r = await this.request(addr, rev);
      return typeof r === "string" && r.startsWith("ok ") ? r.slice(3) : String(r);
    }
    if (rev)
      await this.write(addr, true);
    else
      await this.write(addr);
    return "";
  }
  async fx(kind, deck, on) {
    await this.write(`${await this.prefix(deck)}/fx/${kind}`, on);
    return "";
  }
  async query(name, deck = "") {
    const v = DEV_QUERIES.has(name) ? await this.request(`/sk/dev/${name}`) : await this.request(`${await this.prefix(deck, name, "state")}/state/${name}`);
    if (typeof v === "boolean")
      return v ? "1" : "0";
    if (typeof v === "number") {
      return Number.isInteger(v) ? String(v) : v.toFixed(4);
    }
    return String(v);
  }
  async caps() {
    return Number(await this.request("/sk/dev/caps"));
  }
  async resetCpu() {
    await this.request("/sk/dev/reset/cpu");
    return "";
  }
  async cpu() {
    return {
      avg: Number(await this.query("cpu")),
      min: Number(await this.query("cpumin")),
      max: Number(await this.query("cpumax"))
    };
  }
  describeRows() {
    return this.exclusive(async () => {
      this.drainStale();
      await this.send("/sk/dev/describe");
      return decodePacket(await this.readFrame());
    });
  }
  async describe() {
    return describeFromRows(await this.describeRows());
  }
  async testMode(on) {
    await this.write(`/sk/dev/mode/${on ? "test" : "run"}`);
    return "";
  }
  close() {
    for (const w of this.waiters) {
      if (w.timer)
        clearTimeout(w.timer);
      w.reject(new Timeout("closed"));
    }
    this.waiters = [];
    return this.transport.close();
  }
}
function labelMap(s) {
  const out = new Map;
  for (const t of String(s).split(/\s+/).filter((x) => x.includes(":"))) {
    const i = t.indexOf(":");
    const k = Number(t.slice(0, i));
    if (Number.isInteger(k))
      out.set(k, t.slice(i + 1));
  }
  return out;
}
function describeFromRows(rows) {
  const d = {
    engine: "",
    version: "",
    masked: false,
    params: new Map,
    configs: new Map,
    queries: new Map,
    caps: 0
  };
  const leaf = (addr) => String(addr).split("/").pop() ?? "";
  for (const { address, args } of rows) {
    switch (address) {
      case "/sk/reply/dev/describe":
        if (args.length >= 3) {
          d.engine = String(args[0]);
          d.version = String(args[1]);
          d.masked = args[2] === "masked=1";
        }
        break;
      case "/sk/reply/dev/describe/param": {
        if (args.length < 5)
          break;
        const name = leaf(args[0]);
        d.params.set(name, { name, scope: String(args[4]), lo: Number(args[2]), hi: Number(args[3]) });
        break;
      }
      case "/sk/reply/dev/describe/cfg": {
        if (args.length < 3)
          break;
        const name = leaf(args[0]);
        d.configs.set(name, { name, values: labelMap(String(args[2])) });
        break;
      }
      case "/sk/reply/dev/describe/state": {
        if (args.length < 3)
          break;
        const name = leaf(args[0]);
        const scope = ["a", "b"].includes(String(args[0]).split("/")[2]) ? "deck" : "global";
        d.queries.set(name, {
          name,
          scope,
          kind: String(args[2]),
          values: labelMap(args.length >= 4 ? String(args[3]) : "")
        });
        break;
      }
      case "/sk/reply/dev/describe/caps":
        if (args.length)
          d.caps = Number(args[0]);
        break;
      default:
        break;
    }
  }
  return d;
}

// src/core/client.ts
function lineClient(device) {
  return {
    codec: "line",
    exec: (input) => device.cmd(input),
    destructive: isDestructive,
    describe: async () => parseDescribe(await device.describeLines()),
    query: (name, deck = "") => device.query(name, deck),
    cpu: () => device.cpu(),
    resetCpu: async () => {
      await device.resetCpu();
    },
    close: () => device.close(),
    example: "set param speed a 0.5",
    setParam: async (name, deck, value) => {
      await device.setParam(name, deck, value);
    },
    getParam: (name, deck) => device.getParam(name, deck),
    setConfig: async (name, deck, value) => {
      await device.setConfig(name, deck, value);
    },
    gate: async (deck) => {
      await device.gate(deck);
    },
    pad: (action, deck) => device.pad(action, deck)
  };
}
function oscClient(device) {
  return {
    codec: "osc",
    exec: (input) => execOsc(device, input),
    destructive: isDestructiveAddress,
    describe: () => device.describe(),
    query: (name, deck = "") => device.query(name, deck),
    cpu: () => device.cpu(),
    resetCpu: async () => {
      await device.resetCpu();
    },
    close: () => device.close(),
    example: "/sk/a/param/speed 0.5",
    setParam: async (name, deck, value) => {
      await device.setParam(name, deck, value);
    },
    getParam: (name, deck) => device.getParam(name, deck),
    setConfig: async (name, deck, value) => {
      await device.setConfig(name, deck, value);
    },
    gate: async (deck) => {
      await device.gate(deck);
    },
    pad: (action, deck) => device.pad(action, deck)
  };
}
async function execOsc(device, input) {
  const [address, ...rest] = input.trim().split(/\s+/).filter(Boolean);
  if (!address)
    return "";
  if (!address.startsWith("/")) {
    throw new Error(`not an OSC address: ${JSON.stringify(address)}. ` + "This build speaks OSC - try /sk/dev/describe, or use the controls above.");
  }
  const reply = await device.request(address, ...rest.map(parseArg));
  const vals = Array.isArray(reply) ? reply : [reply];
  return vals.map(String).join(" ");
}
function parseArg(tok) {
  if (tok === "true")
    return true;
  if (tok === "false")
    return false;
  if (/^-?\d+$/.test(tok))
    return oscInt(Number(tok));
  if (/^-?(\d+\.?\d*|\.\d+)(e[-+]?\d+)?$/i.test(tok))
    return Number(tok);
  return tok;
}
var DESTRUCTIVE_ADDRESSES = [
  /^\/sk(\/[ab])?\/pad\/clear\b/,
  /^\/sk(\/[ab])?\/seq\/clear\b/,
  /^\/sk(\/[ab])?\/clear\b/,
  /^\/sk\/dev\/preset\/save\b/,
  /^\/sk\/dev\/reset(?!\/cpu)/
];
function isDestructiveAddress(input) {
  const addr = input.trim().split(/\s+/)[0]?.toLowerCase() ?? "";
  return DESTRUCTIVE_ADDRESSES.some((re) => re.test(addr));
}

// src/app/terminal_model.ts
var CPU_HISTORY = 240;
var CONSOLE_LIMIT = 500;
var POLL_MS = 500;
var INITIAL3 = {
  connected: false,
  port: "",
  status: "",
  lines: [],
  descriptor: null,
  cpu: null,
  cpuHistory: [],
  cpuAvailable: true,
  polling: false,
  usb: [],
  usbAvailable: true,
  offerAllPorts: false,
  error: null,
  codec: "line",
  example: "set param speed a 0.5"
};

class TerminalModel {
  deps;
  store = new Store({ ...INITIAL3 });
  device = null;
  stopPoll = null;
  constructor(deps) {
    this.deps = deps;
  }
  supported() {
    return this.deps.serial.supported();
  }
  oscSupported() {
    return this.deps.serial.requestFrames != null;
  }
  setCodec(codec) {
    if (this.store.get().connected)
      return;
    this.store.set({ codec, example: codec === "osc" ? "/sk/a/param/speed 0.5" : "set param speed a 0.5" });
  }
  write(text, kind = "meta") {
    const lines = [...this.store.get().lines, { text, kind }];
    this.store.set({ lines: lines.slice(-CONSOLE_LIMIT) });
  }
  async connect({ filtered = true } = {}) {
    const codec = this.store.get().codec;
    try {
      const log = (l) => this.write(l, "log");
      let transport;
      if (codec === "osc") {
        const requestFrames = this.deps.serial.requestFrames;
        if (!requestFrames)
          throw new Error("this page cannot open an OSC session");
        transport = await requestFrames({ filtered });
        transport.onClose((why) => this.lost(why));
        this.device = oscClient(new OscDevice(transport, { logSink: log }));
      } else {
        transport = await this.deps.serial.request({ filtered });
        transport.onClose((why) => this.lost(why));
        this.device = lineClient(new Device(transport, { logSink: log }));
      }
      this.store.set({
        connected: true,
        port: transport.info(),
        status: `connected (${transport.info()}, ${codec === "osc" ? "OSC" : "line"} codec)`,
        example: this.device.example,
        offerAllPorts: false,
        error: null
      });
      this.write(`connected using the ${codec === "osc" ? "OSC" : "line"} codec`, "meta");
      await this.refreshDescribe();
      await this.refreshUsb();
      this.startPolling();
      return true;
    } catch (e) {
      const err = e;
      if (err.name === "NotFoundError") {
        if (filtered) {
          this.write("no port chosen. If the chooser was empty, nothing on this machine is reporting " + "the Daisy's USB vendor id - check the device is on and running a TERMINAL=1 build, or " + "list every serial port instead.", "meta");
          this.store.set({ offerAllPorts: true });
        } else {
          this.write("no port chosen.", "meta");
        }
        return false;
      }
      this.store.set({ error: err.message, status: "" });
      return false;
    }
  }
  lost(why) {
    if (!this.device)
      return;
    this.write(`device disconnected: ${why}`, "err");
    this.disconnect();
  }
  async disconnect() {
    this.stopPolling();
    try {
      await this.device?.close();
    } catch {}
    this.device = null;
    this.store.set({
      connected: false,
      port: "",
      status: "disconnected",
      descriptor: null,
      cpu: null,
      cpuHistory: [],
      usb: []
    });
    this.write("disconnected", "meta");
  }
  async send(line, { quiet = false } = {}) {
    if (!this.device)
      return null;
    if (this.device.destructive(line) && !this.deps.confirm(`Send "${line}"?`)) {
      this.write(`cancelled: ${line}`, "meta");
      return null;
    }
    if (!quiet)
      this.write(`> ${line}`, "sent");
    try {
      const reply = await this.device.exec(line);
      if (!quiet)
        this.write(reply === "" ? "ok" : `ok ${reply}`, "ok");
      return reply;
    } catch (e) {
      if (!quiet) {
        this.write(e instanceof CommandError ? `err ${e.reason}` : e instanceof Timeout ? "timeout - no reply" : String(e), "err");
      }
      return null;
    }
  }
  async perform(label, fn, { quiet = false, confirmAs = "" } = {}) {
    const device = this.device;
    if (!device)
      return null;
    if (confirmAs && !this.deps.confirm(`Send "${confirmAs}"?`)) {
      this.write(`cancelled: ${label}`, "meta");
      return null;
    }
    if (!quiet)
      this.write(`> ${label}`, "sent");
    try {
      const out = await fn(device);
      if (!quiet)
        this.write(out === "" || out === undefined ? "ok" : `ok ${out}`, "ok");
      return out;
    } catch (e) {
      if (!quiet) {
        this.write(e instanceof CommandError ? `err ${e.reason}` : e instanceof Timeout ? "timeout - no reply" : String(e), "err");
      }
      return null;
    }
  }
  setParam(name, deck, value, { quiet = false } = {}) {
    return this.perform(`${name} ${deck} = ${value}`, (d) => d.setParam(name, deck, value), { quiet });
  }
  getParam(name, deck, { quiet = false } = {}) {
    return this.perform(`read ${name} ${deck}`, (d) => d.getParam(name, deck), { quiet });
  }
  setConfig(name, deck, value) {
    return this.perform(`${name} ${deck} = ${value}`, (d) => d.setConfig(name, deck, value));
  }
  gate(deck) {
    return this.perform(`gate ${deck}`, (d) => d.gate(deck));
  }
  pad(action, deck) {
    return this.perform(`pad ${action} ${deck}`, (d) => d.pad(action, deck), { confirmAs: action === "clear" ? `pad clear ${deck}` : "" });
  }
  queryValue(name, deck) {
    return this.perform(`query ${name} ${deck}`.trimEnd(), (d) => d.query(name, deck), { quiet: true });
  }
  async refreshDescribe() {
    if (!this.device)
      return;
    try {
      this.store.set({ descriptor: await this.device.describe() });
    } catch {
      this.store.set({ descriptor: null });
    }
  }
  async refreshUsb() {
    if (!this.device)
      return;
    try {
      this.store.set({ usb: parseUsbDiag(await this.device.query("usb")), usbAvailable: true });
    } catch {
      this.store.set({ usb: [], usbAvailable: false });
    }
  }
  startPolling() {
    this.stopPolling();
    this.stopPoll = this.deps.clock.every(POLL_MS, () => void this.pollCpu());
    this.store.set({ polling: true });
    this.pollCpu();
  }
  stopPolling() {
    this.stopPoll?.();
    this.stopPoll = null;
    this.store.set({ polling: false });
  }
  togglePolling() {
    if (this.store.get().polling)
      this.stopPolling();
    else
      this.startPolling();
  }
  async pollCpu() {
    if (!this.device)
      return;
    try {
      const cpu = await this.device.cpu();
      const cpuHistory = [...this.store.get().cpuHistory, cpu.avg].slice(-CPU_HISTORY);
      this.store.set({ cpu, cpuHistory, cpuAvailable: true });
    } catch {
      this.stopPolling();
      this.store.set({ cpuAvailable: false });
    }
  }
  async resetCpu() {
    if (!this.device)
      return;
    this.write("> reset cpu", "sent");
    try {
      await this.device.resetCpu();
      this.write("ok", "ok");
    } catch (e) {
      this.write(e instanceof CommandError ? `err ${e.reason}` : String(e), "err");
      return;
    }
    this.store.set({ cpuHistory: [] });
  }
}

// src/platform/serial.ts
var DAISY_VID = 1155;
var BAUD = 115200;
var supported = () => typeof navigator !== "undefined" && navigator.serial != null;
async function openPort({ filtered = true } = {}) {
  if (!supported()) {
    throw new Error("This browser has no WebSerial. Use Chrome, Edge or another Chromium browser.");
  }
  const serial = navigator.serial;
  const port = await serial.requestPort(filtered ? { filters: [{ usbVendorId: DAISY_VID }] } : {});
  await port.open({ baudRate: BAUD });
  try {
    await port.setSignals?.({ dataTerminalReady: true });
  } catch {}
  return port;
}
async function requestPort(opts = {}) {
  return new SerialTransport(await openPort(opts));
}
async function requestFramePort(opts = {}) {
  return new OscSerialTransport(await openPort(opts));
}

class SerialTransport {
  port;
  assembler = new LineAssembler;
  onLineCb = () => {};
  onCloseCb = () => {};
  closed = false;
  reader = null;
  constructor(port) {
    this.port = port;
    this.pump();
  }
  onLine(cb) {
    this.onLineCb = cb;
  }
  onClose(cb) {
    this.onCloseCb = cb;
  }
  async pump() {
    const decoder = new TextDecoder;
    this.reader = this.port.readable.getReader();
    let reason = "the port closed";
    try {
      for (;; ) {
        const { value, done } = await this.reader.read();
        if (done)
          break;
        for (const line of this.assembler.push(decoder.decode(value, { stream: true }))) {
          this.onLineCb(line);
        }
      }
    } catch (e) {
      reason = e.message;
      if (!this.closed)
        this.onLineCb(`[transport] read failed: ${reason}`);
    } finally {
      try {
        this.reader.releaseLock();
      } catch {}
      if (!this.closed)
        this.onCloseCb(reason);
    }
  }
  async write(text) {
    const writer = this.port.writable.getWriter();
    try {
      await writer.write(new TextEncoder().encode(text));
    } finally {
      writer.releaseLock();
    }
  }
  async close() {
    this.closed = true;
    try {
      await this.reader?.cancel();
    } catch {}
    await this.port.close();
  }
  info() {
    return portInfo(this.port);
  }
}

class OscSerialTransport {
  port;
  decoder = new SlipDecoder;
  onFrameCb = () => {};
  onCloseCb = () => {};
  closed = false;
  reader = null;
  constructor(port) {
    this.port = port;
    this.pump();
  }
  onFrame(cb) {
    this.onFrameCb = cb;
  }
  onClose(cb) {
    this.onCloseCb = cb;
  }
  async pump() {
    this.reader = this.port.readable.getReader();
    let reason = "the port closed";
    try {
      for (;; ) {
        const { value, done } = await this.reader.read();
        if (done)
          break;
        for (const frame of this.decoder.feed(value))
          this.onFrameCb(frame);
      }
    } catch (e) {
      reason = e.message;
    } finally {
      try {
        this.reader.releaseLock();
      } catch {}
      if (!this.closed)
        this.onCloseCb(reason);
    }
  }
  async send(packet) {
    const writer = this.port.writable.getWriter();
    try {
      await writer.write(slipEncode(packet));
    } finally {
      writer.releaseLock();
    }
  }
  async close() {
    this.closed = true;
    try {
      await this.reader?.cancel();
    } catch {}
    await this.port.close();
  }
  info() {
    return portInfo(this.port);
  }
}
function portInfo(port) {
  const i = port.getInfo?.() ?? {};
  const hex = (v) => v == null ? "?" : `0x${v.toString(16).padStart(4, "0")}`;
  return `USB ${hex(i.usbVendorId)}:${hex(i.usbProductId)}`;
}
var webSerial = {
  supported,
  request: requestPort,
  requestFrames: requestFramePort
};

// src/platform/clock.ts
var browserClock = {
  every(ms, fn) {
    const id = setInterval(fn, ms);
    return () => clearInterval(id);
  }
};

// src/ui/cpu_plot.ts
function drawCpuPlot(canvas, history2, max) {
  const ctx = canvas.getContext("2d");
  if (!ctx)
    return;
  const { width: w, height: h } = canvas;
  const style = getComputedStyle(document.body);
  ctx.clearRect(0, 0, w, h);
  const ceiling = Math.max(100, Math.ceil((max ?? 0) / 25) * 25);
  const span = Math.max(1, history2.length - 1);
  ctx.strokeStyle = style.getPropertyValue("--grid") || "#333";
  ctx.lineWidth = 1;
  ctx.font = "10px ui-monospace, monospace";
  ctx.fillStyle = style.getPropertyValue("--muted") || "#888";
  for (let pct = 0;pct <= ceiling; pct += 25) {
    const y = h - pct / ceiling * (h - 12) - 6;
    ctx.beginPath();
    ctx.moveTo(28, y);
    ctx.lineTo(w, y);
    ctx.stroke();
    ctx.fillText(`${pct}%`, 2, y + 3);
  }
  if (history2.length > 1) {
    ctx.strokeStyle = style.getPropertyValue("--accent") || "#4ea1ff";
    ctx.lineWidth = 2;
    ctx.beginPath();
    history2.forEach((v, i) => {
      const x = 28 + i / span * (w - 30);
      const y = h - v / ceiling * (h - 12) - 6;
      if (i === 0)
        ctx.moveTo(x, y);
      else
        ctx.lineTo(x, y);
    });
    ctx.stroke();
  }
  if (max != null) {
    const y = h - max / ceiling * (h - 12) - 6;
    ctx.strokeStyle = style.getPropertyValue("--danger") || "#e05252";
    ctx.setLineDash([4, 3]);
    ctx.beginPath();
    ctx.moveTo(28, y);
    ctx.lineTo(w, y);
    ctx.stroke();
    ctx.setLineDash([]);
  }
}

// src/ui/terminal_view.ts
function mountTerminal(root, _ctx) {
  const model = new TerminalModel({
    serial: webSerial,
    clock: browserClock,
    confirm: confirmDestructive
  });
  const log = el("div", { class: "console" });
  const status = el("div", { class: "status" });
  const surface = el("div", { class: "surface" });
  const cpuPanel = el("div", { class: "cpu" });
  const usbPanel = el("div", { class: "usb" });
  const canvas = el("canvas", { class: "plot", width: 720, height: 160 });
  const readout = el("span", { class: "cpu-readout mono" }, "-");
  const input = el("input", {
    type: "text",
    class: "cmdline",
    placeholder: "type a command, e.g. query cpu   (Tab completes, Up recalls)",
    autocomplete: "off",
    disabled: true
  });
  const history2 = [];
  let historyPos = 0;
  let renderedLines = 0;
  const connectBtn = el("button", {
    class: "primary",
    onclick: () => model.store.get().connected ? model.disconnect() : model.connect()
  }, "Connect");
  const allPortsBtn = el("button", {
    hidden: true,
    onclick: () => model.connect({ filtered: false })
  }, "List every serial port");
  const codecSelect = el("select", {
    class: "codec",
    title: "Which codec the firmware was built with. OSC needs a TERMINAL=1 OSC=1 build.",
    onchange: (e) => model.setCodec(e.target.value)
  }, el("option", { value: "line" }, "line codec"), el("option", { value: "osc" }, "OSC codec"));
  function renderConsole(lines) {
    if (lines.length < renderedLines) {
      clear(log);
      renderedLines = 0;
    }
    const atBottom = log.scrollTop + log.clientHeight >= log.scrollHeight - 4;
    for (const l of lines.slice(renderedLines)) {
      log.append(el("div", { class: `line ${l.kind}` }, l.text));
    }
    renderedLines = lines.length;
    while (log.childElementCount > 500)
      log.firstElementChild?.remove();
    if (atBottom)
      log.scrollTop = log.scrollHeight;
  }
  input.addEventListener("keydown", async (e) => {
    if (e.key === "Enter") {
      const line = input.value.trim();
      if (!line)
        return;
      history2.push(line);
      historyPos = history2.length;
      input.value = "";
      await model.send(line);
      if (/^(config|set param|reset|preset)\b/.test(line) || /^\/sk\/.*\/(cfg|param)\//.test(line) || /^\/sk\/dev\/(reset|preset)\b/.test(line))
        renderSurface();
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      if (historyPos > 0)
        input.value = history2[--historyPos] ?? "";
    } else if (e.key === "ArrowDown") {
      e.preventDefault();
      historyPos = Math.min(historyPos + 1, history2.length);
      input.value = history2[historyPos] ?? "";
    } else if (e.key === "Tab") {
      e.preventDefault();
      complete();
    }
  });
  function complete() {
    const descriptor = model.store.get().descriptor;
    if (!descriptor)
      return;
    const words = vocabulary(descriptor);
    const parts = input.value.split(" ");
    const prefix = parts.at(-1);
    if (!prefix)
      return;
    const hits = words.filter((w) => w.startsWith(prefix));
    if (!hits.length)
      return;
    if (hits.length === 1) {
      parts[parts.length - 1] = hits[0];
      input.value = `${parts.join(" ")} `;
    } else {
      model.write(hits.join("  "), "meta");
    }
  }
  const queryLabel = (q, raw) => {
    if (q.kind === "enum" && q.values.has(Number(raw)))
      return `${raw} (${q.values.get(Number(raw))})`;
    if (q.kind === "bool")
      return raw === "1" ? "yes" : "no";
    return raw || "ok";
  };
  function paramRow(p, deck) {
    const out = el("span", { class: "mono value" }, "-");
    const slider = el("input", {
      type: "range",
      min: String(p.lo),
      max: String(p.hi),
      step: String((p.hi - p.lo) / 1000),
      value: String((p.lo + p.hi) / 2)
    });
    slider.addEventListener("input", () => {
      out.textContent = Number(slider.value).toPrecision(4);
    });
    slider.addEventListener("change", async () => {
      out.textContent = Number(slider.value).toPrecision(4);
      await model.setParam(p.name, deck, Number(slider.value), { quiet: true });
    });
    return el("div", { class: "row" }, el("label", {}, `${p.name}${p.scope === "deck" ? ` ${deck}` : ""}`), slider, out, el("button", {
      class: "link",
      onclick: async () => {
        const v = await model.getParam(p.name, deck, { quiet: true });
        if (v != null) {
          slider.value = String(v);
          out.textContent = v.toPrecision(4);
        }
      }
    }, "read"));
  }
  function renderSurface() {
    clear(surface);
    const descriptor = model.store.get().descriptor;
    if (!descriptor)
      return;
    surface.append(el("h3", {}, `${descriptor.engine || "engine"} `, el("span", { class: "muted" }, descriptor.version)));
    if (!descriptor.masked) {
      surface.append(el("div", { class: "callout warn" }, el("strong", {}, "masked=0: "), "this build does not declare which parameters it implements, so the list below is the whole " + "enum. Some of these controls will have no effect."));
    }
    const decks = ["A", "B"];
    if (descriptor.params.size) {
      const grid = el("div", { class: "grid gap-1" });
      for (const p of descriptor.params.values()) {
        for (const deck of p.scope === "deck" ? decks : ["A"])
          grid.append(paramRow(p, deck));
      }
      surface.append(el("h4", {}, "Parameters"), grid);
    }
    if (descriptor.configs.size) {
      const grid = el("div", { class: "grid gap-1" });
      for (const c of descriptor.configs.values()) {
        const sel = el("select", { onchange: () => model.setConfig(c.name, "A", Number(sel.value)) }, [...c.values.entries()].map(([v, lbl]) => el("option", { value: String(v) }, `${v} - ${lbl}`)));
        grid.append(el("div", { class: "row" }, el("label", {}, c.name), sel));
      }
      surface.append(el("h4", {}, "Configs"), grid);
    }
    const actions = el("div", { class: "actions" });
    for (const deck of decks) {
      actions.append(el("button", { onclick: () => model.gate(deck) }, `gate ${deck}`));
      for (const action of ["play", "rec", "stop", "clear"]) {
        actions.append(el("button", {
          class: action === "clear" ? "danger" : "",
          onclick: () => model.pad(action, deck)
        }, `${action} ${deck}`));
      }
    }
    surface.append(el("h4", {}, "Actions"), actions);
    if (descriptor.queries.size) {
      const list = el("div", { class: "grid gap-1" });
      for (const q of descriptor.queries.values()) {
        const value = el("span", { class: "mono value" }, "-");
        list.append(el("div", { class: "row" }, el("label", {}, q.name), el("button", {
          onclick: async () => {
            const r = await model.queryValue(q.name, q.scope === "deck" ? "A" : "");
            value.textContent = r == null ? "err" : queryLabel(q, r);
          }
        }, "read"), value));
      }
      surface.append(el("h4", {}, "Queries"), el("p", { class: "muted note" }, "Every query here is safe to call in any order - the two that are not (`fit`, which takes an " + "argument, and `reseed`, which self-clears when read) are deliberately absent from the " + "descriptor."), list);
    }
  }
  let lastDescriptor = null;
  model.store.subscribe((s) => {
    status.textContent = s.error ? s.error : s.status;
    connectBtn.textContent = s.connected ? "Disconnect" : "Connect";
    allPortsBtn.hidden = !s.offerAllPorts;
    input.disabled = !s.connected;
    codecSelect.value = s.codec;
    codecSelect.disabled = s.connected;
    input.placeholder = `type a command, e.g. ${s.example}   (Tab completes, Up recalls)`;
    renderConsole(s.lines);
    if (s.descriptor !== lastDescriptor) {
      lastDescriptor = s.descriptor;
      renderSurface();
    }
    readout.textContent = !s.cpuAvailable ? "not available on this build" : s.cpu ? `now ${s.cpu.avg.toFixed(1)}%   min ${s.cpu.min.toFixed(1)}%   max ${s.cpu.max.toFixed(1)}%` : "-";
    drawCpuPlot(canvas, s.cpuHistory, s.cpu?.max ?? null);
    clear(usbPanel);
    if (!s.usbAvailable) {
      usbPanel.append(el("p", { class: "muted" }, "`query usb` is not available on this build (it needs USBDIAG=1)."));
    } else if (s.usb.length) {
      usbPanel.append(el("table", { class: "layout" }, el("thead", {}, el("tr", {}, el("th", {}, "Field"), el("th", {}, "Value"))), el("tbody", {}, s.usb.map((r) => el("tr", {}, el("td", { class: "mono" }, r.key), el("td", { class: "mono" }, r.value))))));
    }
  });
  cpuPanel.append(el("div", { class: "controls" }, readout, el("button", {
    onclick: () => model.resetCpu()
  }, "reset cpu"), el("button", { onclick: () => model.togglePolling() }, "start / stop polling")), canvas, el("p", { class: "muted note" }, "min and max are extremes since the last reset, not a rolling window. The sequence a measurement " + "wants is: reset, drive the engine, then watch whether max stops climbing."));
  append(mountPoint(root), [
    el("div", { class: "controls" }, connectBtn, codecSelect, allPortsBtn, status),
    !model.supported() && el("div", { class: "callout" }, el("strong", {}, "This browser has no WebSerial. "), "Talking to hardware needs Chrome or Edge - and unlike the card screens there is no fallback " + "here, because there is no zip-shaped substitute for a serial port."),
    el("h3", {}, "Console"),
    log,
    input,
    el("h3", {}, "CPU load"),
    cpuPanel,
    el("h3", {}, "Control surface"),
    surface,
    el("h3", {}, "USB bring-up"),
    usbPanel
  ]);
}

// src/core/dfu.ts
var DFU_DNLOAD = 1;
var DFU_UPLOAD = 2;
var DFU_GETSTATUS = 3;
var DFU_CLRSTATUS = 4;
var DFU_ABORT = 6;
var STATE_DFU_IDLE = 2;
var STATE_DFU_DNBUSY = 4;
var STATE_DFU_MANIFEST = 7;
var STATE_DFU_ERROR = 10;
var CMD_SET_ADDRESS = 33;
var CMD_ERASE = 65;
var STATUS_TEXT = {
  0: "OK",
  1: "file rejected by the device",
  2: "file failed its target verification",
  3: "write failed - the address may be out of range",
  4: "erase failed",
  5: "erase check failed",
  6: "programming failed",
  7: "the device is write-protected",
  8: "address out of range",
  9: "the download ended early",
  10: "the firmware is corrupt",
  11: "vendor-specific error",
  12: "unexpected USB reset",
  13: "power-on reset detected",
  14: "unknown error",
  15: "the device stalled an unexpected request"
};
function statusText(status) {
  return STATUS_TEXT[status] ?? `device error 0x${status.toString(16).padStart(2, "0")}`;
}

class DfuError extends Error {
  status;
  constructor(message, status) {
    super(message);
    this.status = status;
    this.name = "DfuError";
  }
}
async function settle(dev, sleep, cap = 5000) {
  for (let i = 0;i < 1000; i++) {
    const st = await dev.getStatus();
    if (st.state === STATE_DFU_ERROR) {
      throw new DfuError(statusText(st.status), st);
    }
    if (st.state !== STATE_DFU_DNBUSY && st.state !== STATE_DFU_MANIFEST)
      return st;
    await sleep(Math.min(st.pollTimeout, cap));
  }
  throw new DfuError("the device never finished the last operation");
}
async function reset(dev, sleep) {
  const st = await dev.getStatus();
  if (st.state === STATE_DFU_ERROR) {
    await dev.clearStatus();
  } else if (st.state !== STATE_DFU_IDLE) {
    await dev.abort();
  }
  const after = await settle(dev, sleep);
  if (after.state !== STATE_DFU_IDLE) {
    throw new DfuError(`the device will not return to idle (state ${after.state})`);
  }
}
function le32(cmd, addr) {
  const b = new Uint8Array(5);
  b[0] = cmd;
  b[1] = addr & 255;
  b[2] = addr >>> 8 & 255;
  b[3] = addr >>> 16 & 255;
  b[4] = addr >>> 24 & 255;
  return b;
}
async function setAddress(dev, sleep, addr, abortAfter = false) {
  await dev.download(0, le32(CMD_SET_ADDRESS, addr));
  await settle(dev, sleep);
  if (abortAfter) {
    await dev.abort();
    await settle(dev, sleep);
  }
}
async function erasePage(dev, sleep, addr) {
  await dev.download(0, le32(CMD_ERASE, addr));
  await settle(dev, sleep);
}
function aborted(opts) {
  return opts.signal?.aborted === true;
}
function firstDifference(want, got) {
  for (let i = 0;i < want.length; i++)
    if (got[i] !== want[i])
      return i;
  return -1;
}
function hex(b, at) {
  return [...b.subarray(at, at + 8)].map((n) => n.toString(16).padStart(2, "0")).join(" ");
}
function blankNote(got) {
  if (got.length === 0)
    return " (the device returned no data at all)";
  if (got.every((b) => b === 255)) {
    return " - the whole block read as 0xFF (erased), so either nothing was written or this " + "bootloader does not read QSPI back";
  }
  if (got.every((b) => b === 0)) {
    return " - the whole block read as zero, which usually means UPLOAD is not implemented rather " + "than that the write failed";
  }
  return "";
}
async function uploadIsTrustworthy(dev, sleep, address, transferSize) {
  try {
    await reset(dev, sleep);
    await setAddress(dev, sleep, address, true);
    const probe = await dev.upload(2, transferSize);
    return probe.length > 0 && probe.every((b) => b === 255);
  } catch {
    return false;
  } finally {
    await reset(dev, sleep).catch(() => {});
  }
}
async function flash(dev, image, opts, sleep) {
  const { address, transferSize, eraseSize } = opts;
  const report = opts.onProgress ?? (() => {});
  await reset(dev, sleep);
  const firstSector = Math.floor(address / eraseSize) * eraseSize;
  const lastByte = address + image.length - 1;
  const sectors = [];
  for (let a = firstSector;a <= lastByte; a += eraseSize)
    sectors.push(a);
  for (let i = 0;i < sectors.length; i++) {
    if (aborted(opts))
      throw new DfuError("cancelled");
    await erasePage(dev, sleep, sectors[i]);
    report({ phase: "erase", done: i + 1, total: sectors.length });
  }
  let canVerify = false;
  let note;
  if (opts.verify) {
    canVerify = await uploadIsTrustworthy(dev, sleep, address, transferSize);
    if (!canVerify) {
      note = "this bootloader does not report memory through DFU UPLOAD, so the image could not be " + "read back. Confirm the device boots.";
    }
  }
  const blocks = Math.ceil(image.length / transferSize);
  for (let i = 0;i < blocks; i++) {
    if (aborted(opts))
      throw new DfuError("cancelled");
    const chunk = image.subarray(i * transferSize, Math.min((i + 1) * transferSize, image.length));
    await setAddress(dev, sleep, address + i * transferSize);
    await dev.download(2, chunk);
    await settle(dev, sleep);
    report({ phase: "write", done: i + 1, total: blocks });
  }
  if (opts.verify && canVerify) {
    await reset(dev, sleep);
    await setAddress(dev, sleep, address, true);
    for (let i = 0;i < blocks; i++) {
      if (aborted(opts))
        throw new DfuError("cancelled");
      const want = image.subarray(i * transferSize, Math.min((i + 1) * transferSize, image.length));
      const got = await dev.upload(i + 2, transferSize);
      if (got.length < want.length) {
        throw new DfuError(`read back ${got.length} bytes where ${want.length} were written`);
      }
      const at = firstDifference(want, got);
      if (at >= 0) {
        throw new DfuError(`read-back mismatch at 0x${(address + i * transferSize + at).toString(16)}: ` + `expected ${hex(want, at)}, device returned ${hex(got, at)}${blankNote(got)}`);
      }
      report({ phase: "verify", done: i + 1, total: blocks });
    }
  }
  report({ phase: "manifest", done: 0, total: 1 });
  try {
    await dev.download(0, new Uint8Array(0));
    await settle(dev, sleep);
  } catch {}
  report({ phase: "manifest", done: 1, total: 1 });
  return { verified: opts.verify === true && canVerify, note };
}

// src/core/image.ts
var APP_ADDRESS = 2416181248;
var QSPI_END = 2424307712;
var MAX_APP_BYTES = QSPI_END - APP_ADDRESS;
var MIN_BYTES = 512;
function readBanner(bytes) {
  let text = "";
  for (let i = 0;i < bytes.length; i += 32768) {
    text += String.fromCharCode(...bytes.subarray(i, Math.min(i + 32768, bytes.length)));
  }
  const m = /spotykach (\S+) engine=([a-z0-9_]+)/.exec(text);
  return m ? { version: m[1], engine: m[2] } : null;
}
function classify(resetVector) {
  const region = resetVector >>> 24;
  if (region === 36)
    return "sram-app";
  if (region === 144)
    return "qspi-app";
  if (region === 8)
    return "bootloader";
  return "unknown";
}
function inspectImage(buf) {
  const bytes = new Uint8Array(buf);
  const problems = [];
  const warnings = [];
  if (bytes.length < MIN_BYTES) {
    return {
      kind: "unknown",
      bytes: bytes.length,
      stackPointer: 0,
      resetVector: 0,
      version: null,
      engine: null,
      flashable: false,
      problems: [`only ${bytes.length} bytes - too small to be a firmware image`],
      warnings: []
    };
  }
  const view = new DataView(buf);
  const stackPointer = view.getUint32(0, true);
  const resetVector = view.getUint32(4, true);
  const kind = classify(resetVector);
  const banner = readBanner(bytes);
  if (kind === "bootloader") {
    problems.push("this is a bootloader image (its reset vector is in internal flash at " + `0x${resetVector.toString(16)}), not an engine. Installing a bootloader is a separate, ` + "device-level procedure and is deliberately not done from this page.");
  }
  if (kind === "unknown") {
    problems.push(`the reset vector (0x${resetVector.toString(16)}) points nowhere this hardware runs code from. ` + "This is probably not a Daisy firmware image at all.");
  }
  if (bytes.length > MAX_APP_BYTES) {
    problems.push(`${(bytes.length / 1024).toFixed(0)} KB does not fit the ` + `${(MAX_APP_BYTES / 1024 / 1024).toFixed(0)} MB QSPI app region`);
  }
  const spRegion = stackPointer >>> 24;
  if (spRegion !== 32 && spRegion !== 36) {
    warnings.push(`the initial stack pointer (0x${stackPointer.toString(16)}) is not in DTCM or AXI SRAM, ` + "which is unusual for a Daisy image");
  }
  if (!banner) {
    warnings.push("no spotykach version banner in this image, so its engine and version cannot be confirmed. " + "A released binary always carries one.");
  }
  return {
    kind,
    bytes: bytes.length,
    stackPointer,
    resetVector,
    version: banner?.version ?? null,
    engine: banner?.engine ?? null,
    flashable: problems.length === 0,
    problems,
    warnings
  };
}
function describeImage(info) {
  const size = info.bytes < 1024 * 1024 ? `${(info.bytes / 1024).toFixed(0)} KB` : `${(info.bytes / 1024 / 1024).toFixed(2)} MB`;
  if (info.engine && info.version) {
    const where = info.kind === "qspi-app" ? "runs from QSPI" : "runs from SRAM";
    return `${info.engine} ${info.version} - ${size}, ${where}`;
  }
  return `unidentified image - ${size}`;
}

// src/app/flash_model.ts
var ERASE_SIZE = 64 * 1024;
var EMPTY = {
  supported: false,
  device: null,
  image: null,
  filename: null,
  busy: false,
  phase: null,
  progress: 0,
  result: null,
  error: null
};
function assertTarget(address) {
  if (address !== APP_ADDRESS) {
    throw new DfuError(`refusing to write 0x${address.toString(16)}: this page only ever writes the application ` + `region at 0x${APP_ADDRESS.toString(16)}. Installing a bootloader is a separate procedure.`);
  }
}

class FlashModel {
  deps;
  store = new Store(EMPTY);
  dev = null;
  bytes = null;
  cancel = { aborted: false };
  constructor(deps) {
    this.deps = deps;
    this.store.set({ supported: deps.usb.supported() });
  }
  get sleep() {
    return this.deps.sleep ?? ((ms) => new Promise((r) => setTimeout(r, ms)));
  }
  select(filename, buf) {
    const image = inspectImage(buf);
    this.bytes = new Uint8Array(buf);
    this.store.set({ filename, image, result: null, error: null });
  }
  clearSelection() {
    this.bytes = null;
    this.store.set({ filename: null, image: null, result: null, error: null });
  }
  async connect() {
    if (!this.deps.usb.supported()) {
      this.store.set({ error: "this browser has no WebUSB" });
      return;
    }
    try {
      this.dev = await this.deps.usb.request();
      this.store.set({ device: this.dev.info(), error: null });
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      const cancelled = /No device selected|NotFoundError/i.test(msg);
      this.store.set({ device: null, error: cancelled ? null : msg });
    }
  }
  async disconnect() {
    await this.dev?.close();
    this.dev = null;
    this.store.set({ device: null, phase: null, progress: 0 });
  }
  abort() {
    this.cancel.aborted = true;
  }
  async write() {
    const s = this.store.get();
    if (!this.dev || !this.bytes || !s.image)
      return;
    if (!s.image.flashable) {
      this.store.set({ error: s.image.problems[0] });
      return;
    }
    this.cancel = { aborted: false };
    const what = describeImage(s.image);
    const ask = this.deps.confirm;
    if (ask && !await ask(`Overwrite the firmware on ${s.device} with ${what}?`))
      return;
    this.store.set({ busy: true, error: null, result: null, phase: "erase", progress: 0 });
    try {
      assertTarget(APP_ADDRESS);
      const outcome = await flash(this.dev, this.bytes, {
        address: APP_ADDRESS,
        transferSize: this.dev.transferSize(),
        eraseSize: ERASE_SIZE,
        verify: true,
        signal: this.cancel,
        onProgress: (p) => {
          this.store.set({ phase: p.phase, progress: p.total > 0 ? p.done / p.total : 0 });
        }
      }, this.sleep);
      this.store.set({
        busy: false,
        phase: null,
        progress: 1,
        result: {
          ok: true,
          verified: outcome.verified,
          message: outcome.verified ? `${what} written and read back byte for byte. Power-cycle the device to run it.` : `${what} written. ${outcome.note ?? "It could not be read back."} ` + "Power-cycle the device to run it."
        }
      });
      this.dev = null;
      this.store.set({ device: null });
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      this.store.set({
        busy: false,
        phase: null,
        error: /cancelled/.test(msg) ? "Cancelled. The app region is now partly written - flash again before using the device; " + "the bootloader is untouched, so hold Reset for 3 seconds to get back here." : msg
      });
    }
  }
}

// src/platform/usb.ts
var DFU_VID = 1155;
var DFU_PID = 57105;
var DFU_CLASS = 254;
var DFU_SUBCLASS = 1;
var supported2 = () => typeof navigator !== "undefined" && navigator.usb != null;
function findDfuInterface(dev) {
  const config = dev.configuration;
  if (!config)
    throw new Error("the device offered no USB configuration");
  for (const iface of config.interfaces) {
    for (const alt of iface.alternates) {
      if (alt.interfaceClass === DFU_CLASS && alt.interfaceSubclass === DFU_SUBCLASS) {
        if (alt.alternateSetting === 0) {
          return { iface: iface.interfaceNumber, alt: alt.alternateSetting };
        }
      }
    }
  }
  throw new Error("no DFU interface on that device - is it in bootloader mode?");
}

class WebUsbDfuDevice {
  dev;
  iface;
  xfer;
  constructor(dev, iface, xfer) {
    this.dev = dev;
    this.iface = iface;
    this.xfer = xfer;
  }
  setup(request, value) {
    return { requestType: "class", recipient: "interface", request, value, index: this.iface };
  }
  async download(block, data) {
    const res = await this.dev.controlTransferOut(this.setup(DFU_DNLOAD, block), data);
    if (res.status && res.status !== "ok")
      throw new Error(`download stalled (${res.status})`);
  }
  async upload(block, length) {
    const res = await this.dev.controlTransferIn(this.setup(DFU_UPLOAD, block), length);
    if (res.status && res.status !== "ok")
      throw new Error(`upload stalled (${res.status})`);
    if (!res.data)
      return new Uint8Array(0);
    return new Uint8Array(res.data.buffer, res.data.byteOffset, res.data.byteLength);
  }
  async getStatus() {
    const res = await this.dev.controlTransferIn(this.setup(DFU_GETSTATUS, 0), 6);
    if (!res.data || res.data.byteLength < 6)
      throw new Error("short GETSTATUS response");
    const d = res.data;
    const pollTimeout = d.getUint8(1) | d.getUint8(2) << 8 | d.getUint8(3) << 16;
    return { status: d.getUint8(0), state: d.getUint8(4), pollTimeout };
  }
  async clearStatus() {
    await this.dev.controlTransferOut(this.setup(DFU_CLRSTATUS, 0));
  }
  async abort() {
    await this.dev.controlTransferOut(this.setup(DFU_ABORT, 0));
  }
  async close() {
    try {
      await this.dev.releaseInterface(this.iface);
    } catch {}
    try {
      await this.dev.close();
    } catch {}
  }
  transferSize() {
    return this.xfer;
  }
  info() {
    const name = this.dev.productName || "DFU device";
    const id = `${this.dev.vendorId.toString(16).padStart(4, "0")}:` + `${this.dev.productId.toString(16).padStart(4, "0")}`;
    return `${name} ${id}`;
  }
}
async function request() {
  const usb = navigator.usb;
  if (!usb)
    throw new Error("this browser has no WebUSB");
  const dev = await usb.requestDevice({ filters: [{ vendorId: DFU_VID, productId: DFU_PID }] });
  await dev.open();
  if (!dev.configuration)
    await dev.selectConfiguration(1);
  const { iface, alt } = findDfuInterface(dev);
  await dev.claimInterface(iface);
  await dev.selectAlternateInterface(iface, alt);
  return new WebUsbDfuDevice(dev, iface, 1024);
}
var webUsbDfu = { supported: supported2, request };

// src/ui/flash_view.ts
var PHASE_LABEL = {
  erase: "Erasing",
  write: "Writing",
  verify: "Reading back",
  manifest: "Finishing"
};
function imageReport(info, filename) {
  const rows = [];
  const headline = info.engine && info.version ? `${info.engine} ${info.version}` : "unidentified image";
  rows.push(finding(info.flashable ? "ok" : "error", filename, `${headline} - ${humanBytes(info.bytes)}, reset vector 0x${info.resetVector.toString(16)}`));
  for (const p of info.problems)
    rows.push(finding("error", "", p));
  for (const w of info.warnings)
    rows.push(finding("warn", "", w));
  return el("div", {}, rows);
}
function mountFlash(root, _ctx) {
  const model = new FlashModel({ usb: webUsbDfu, confirm: (q) => confirmDestructive(q) });
  const status = el("div", { class: "status" });
  const report = el("div");
  const bar = el("div", { class: "console" });
  const result = el("div");
  const file = el("input", {
    type: "file",
    accept: ".bin",
    onchange: async (e) => {
      const f = e.target.files?.[0];
      if (f)
        model.select(f.name, await f.arrayBuffer());
    }
  });
  const connectBtn = el("button", {
    class: "primary",
    onclick: () => model.store.get().device ? model.disconnect() : model.connect()
  }, "Connect device");
  const flashBtn = el("button", { class: "danger", onclick: () => model.write() }, "Flash");
  const cancelBtn = el("button", { onclick: () => model.abort() }, "Cancel");
  const drop = el("div", { class: "dropzone" }, [
    el("p", {}, "Drop an engine .bin here, or choose one:"),
    file
  ]);
  dropTarget(drop, async (dt) => {
    const f = dt.files?.[0];
    if (f)
      model.select(f.name, await f.arrayBuffer());
  });
  append(mountPoint(root), [
    el("div", { class: "controls" }, [connectBtn, flashBtn, cancelBtn]),
    status,
    drop,
    report,
    bar,
    result
  ]);
  fill(root, "appaddr", `0x${APP_ADDRESS.toString(16)}`);
  fill(root, "dfucmd", `dfu-util -a 0 -s 0x${APP_ADDRESS.toString(16)}:leave -D sk-<engine>-<version>.bin -d ,0483:df11`);
  model.store.subscribe((s) => {
    if (!s.supported) {
      clear(status);
      append(status, [el("span", {}, "This browser has no WebUSB, so flashing is not possible here. " + "Use Chrome or Edge, or dfu-util (below).")]);
      connectBtn.disabled = true;
      flashBtn.disabled = true;
      cancelBtn.hidden = true;
      return;
    }
    connectBtn.textContent = s.device ? "Disconnect" : "Connect device";
    connectBtn.disabled = s.busy;
    file.disabled = s.busy;
    cancelBtn.hidden = !s.busy;
    const ready = !!s.device && !!s.image?.flashable && !s.busy;
    flashBtn.disabled = !ready;
    clear(status);
    const bits = [];
    bits.push(s.device ? `Device: ${s.device}` : "No device connected.");
    if (!s.image)
      bits.push("No image chosen.");
    else if (!s.image.flashable)
      bits.push("This image cannot be flashed - see below.");
    if (s.device && s.image?.flashable && !s.busy) {
      bits.push(`Ready to write 0x${APP_ADDRESS.toString(16)}.`);
    }
    append(status, [el("span", {}, bits.join(" "))]);
    clear(report);
    if (s.image && s.filename)
      append(report, [imageReport(s.image, s.filename)]);
    clear(bar);
    bar.hidden = !s.busy;
    if (s.busy && s.phase) {
      const pct = Math.round(s.progress * 100);
      append(bar, [
        el("div", { class: "line" }, `${PHASE_LABEL[s.phase] ?? s.phase}... ${pct}%`),
        el("div", { class: "line muted" }, "Do not unplug the device. If you do, hold Reset for 3 " + "seconds and flash again - the bootloader is not being written.")
      ]);
    }
    clear(result);
    if (s.error) {
      append(result, [el("div", { class: "verdict bad" }, [
        el("strong", {}, "Failed"),
        el("p", {}, s.error)
      ])]);
    } else if (s.result) {
      append(result, [el("div", { class: s.result.verified ? "verdict good" : "verdict mixed" }, [
        el("strong", {}, s.result.verified ? "Flashed and verified" : "Flashed, unverified"),
        el("p", {}, s.result.message)
      ])]);
    }
  });
}

// src/app/engine_model.ts
class EngineModel {
  catalogue;
  docs;
  store = new Store({
    entry: null,
    html: "",
    loading: false,
    error: null
  });
  cache = new Map;
  constructor(catalogue, docs) {
    this.catalogue = catalogue;
    this.docs = docs;
  }
  async show(name) {
    const entry = this.catalogue.get(name);
    if (!entry) {
      this.store.set({ entry: null, html: "", loading: false, error: `No engine called "${name}".` });
      return;
    }
    const cached = this.cache.get(name);
    if (cached !== undefined) {
      this.store.set({ entry, html: cached, loading: false, error: null });
      return;
    }
    this.store.set({ entry, html: "", loading: true, error: null });
    try {
      const html = await this.docs.fetchPage(entry.doc.page);
      this.cache.set(name, html);
      if (this.store.get().entry?.doc.name !== name)
        return;
      this.store.set({ html, loading: false });
    } catch (e) {
      if (this.store.get().entry?.doc.name !== name)
        return;
      this.store.set({ loading: false, error: e.message });
    }
  }
}

// src/platform/docs.ts
var httpDocs = {
  async fetchPage(path) {
    const res = await fetch(path);
    if (!res.ok)
      throw new Error(`cannot load ${path}: HTTP ${res.status}`);
    return res.text();
  }
};

// src/ui/lightbox.ts
function createLightbox() {
  const img = el("img", { alt: "" });
  const caption = el("p", { class: "muted note" });
  let actual = false;
  const frame = el("div", { class: "lightbox-frame" }, img);
  const setZoom = (on) => {
    actual = on;
    frame.classList.toggle("actual", actual);
    zoom.textContent = actual ? "Fit to window" : "Actual size";
    if (actual)
      frame.scrollLeft = (frame.scrollWidth - frame.clientWidth) / 2;
  };
  const zoom = el("button", { type: "button", onclick: () => setZoom(!actual) }, "Actual size");
  const pdfLink = el("a", { class: "pdf-link", download: "", hidden: true }, "Download PDF");
  const close = el("button", { class: "primary", type: "button", onclick: () => dialog.close() }, "Close");
  const dialog = el("dialog", { class: "lightbox", "aria-label": "Diagram viewer" }, el("div", { class: "lightbox-bar" }, caption, el("span", { class: "lightbox-actions" }, pdfLink, zoom, close)), frame);
  dialog.addEventListener("click", (e) => {
    if (e.target === dialog)
      dialog.close();
  });
  document.body.append(dialog);
  return {
    open(src, text, pdf) {
      pdfLink.hidden = !pdf;
      if (pdf)
        pdfLink.href = pdf;
      img.src = src;
      img.alt = text;
      caption.textContent = text;
      setZoom(false);
      if (typeof dialog.showModal === "function")
        dialog.showModal();
      else
        dialog.setAttribute("open", "");
      frame.scrollTop = 0;
    }
  };
}

// src/ui/engine_view.ts
function formatLine(entry) {
  const bank = entry.bank;
  if (!bank)
    return "Reads nothing from the card.";
  return `${folderLabel(bank.dirs)} - ${bank.fmt.describe}`;
}
function mountEngine(root, ctx) {
  const model = new EngineModel(ctx.engines, httpDocs);
  const lightbox = createLightbox();
  const meta = el("p", { class: "muted note engine-meta" });
  const summary = el("div", { class: "callout engine-format" });
  const doc = el("div", { class: "engine-doc" });
  const actions = el("div", { class: "controls" });
  const renderActions = (entry) => {
    clear(actions);
    if (!entry)
      return;
    const name = entry.doc.name;
    actions.append(el("button", { type: "button", class: "primary", onclick: () => ctx.go("flash") }, `Flash ${name}`));
    if (entry.bank) {
      actions.append(el("button", { type: "button", onclick: () => ctx.go("convert") }, "Convert audio for it"), el("button", { type: "button", onclick: () => ctx.go("reference") }, "Its card layout"));
    }
    actions.append(el("button", { type: "button", onclick: () => ctx.go("engines") }, "All engines"));
  };
  model.store.subscribe((s) => {
    if (s.error) {
      renderActions(null);
      clear(meta);
      clear(summary).append(s.error);
      clear(doc);
      return;
    }
    if (!s.entry)
      return;
    const { doc: info, bank } = s.entry;
    renderActions(s.entry);
    append(clear(meta), [!info.released && "Not in the released set."]);
    append(clear(summary), [
      el("strong", {}, bank ? "On the card: " : "No card needed: "),
      formatLine(s.entry),
      bank && el("span", { class: "muted" }, "  Full format on the "),
      bank && el("a", { href: "#reference" }, "Card reference"),
      bank && "."
    ]);
    clear(doc);
    if (s.loading) {
      doc.append(el("p", { class: "muted" }, "Loading the documentation..."));
      return;
    }
    if (!info.page) {
      doc.append(el("p", { class: "muted" }, "This entry is part of the card layout rather than an " + "engine, so it has no manual."));
      return;
    }
    doc.innerHTML = s.html;
  });
  doc.addEventListener("click", (e) => {
    const link = e.target?.closest?.("figure a");
    if (!link || !link.querySelector("img"))
      return;
    e.preventDefault();
    const figure = link.closest("figure");
    const caption = figure?.querySelector("figcaption");
    lightbox.open(link.getAttribute("href") ?? "", caption?.firstChild?.textContent?.replace(/ - open full size\s*$/, "").trim() ?? "", figure?.querySelector("a.pdf-link")?.getAttribute("href") ?? null);
  });
  ctx.engineFocus.subscribe(({ engine }) => {
    if (engine)
      model.show(engine);
  });
  root.append(meta, summary, actions, doc);
}

// src/ui/home_view.ts
function mountHome(root, ctx) {
  const engines = ctx.engines.entries.filter((e) => e.doc.page);
  const released = engines.filter((e) => e.doc.released).length;
  const cardReaders = engines.filter((e) => e.bank).length;
  const stats = root.querySelector("#home-stats");
  if (stats) {
    stats.textContent = `${engines.length} engines in the tree, ${released} of them in the released set. ` + `${cardReaders} read the SD card; the rest need no card at all. ` + `${ctx.layout.banks.length} card layouts, names up to ${ctx.layout.scan.max_name} ` + `characters, files from ${ctx.layout.scan.min_bytes / 1024} KB.`;
  }
  const count = root.querySelector("#home-engine-count");
  if (count)
    count.textContent = String(engines.length);
  for (const node of root.querySelectorAll("[data-view]")) {
    const view = node.getAttribute("data-view");
    if (view)
      node.addEventListener("click", () => ctx.go(view));
  }
}

// src/ui/engines_view.ts
var MAX_CHARS = 190;
function plainText(md) {
  return md.replace(/!\[[^\]]*\]\([^)]*\)/g, "").replace(/\[([^\]]*)\]\([^)]*\)/g, "$1").replace(/`([^`]*)`/g, "$1").replace(/\*\*([^*]*)\*\*/g, "$1").replace(/(?<![*\w])\*([^*]+)\*(?!\w)/g, "$1").replace(/(?<![_\w])_([^_]+)_(?!\w)/g, "$1").replace(/\s+/g, " ").trim();
}
function describe(entry, max = MAX_CHARS) {
  const summary = entry.doc.summary?.trim();
  if (summary)
    return plainText(summary);
  const body = plainText(entry.doc.body ?? "");
  if (!body)
    return "";
  if (body.length <= max)
    return body;
  const window2 = body.slice(0, max + 1);
  const sentence = [...window2.matchAll(/\.\s+(?=[A-Z])/g)].pop();
  if (sentence && sentence.index > max * 0.4)
    return window2.slice(0, sentence.index + 1);
  const cut = window2.lastIndexOf(" ");
  return `${window2.slice(0, cut > 0 ? cut : max).replace(/[,;:\s]+$/, "")}...`;
}
function mountEngines(root, ctx) {
  const entries = ctx.engines.entries.filter((e) => e.doc.page);
  clear(mountPoint(root)).append(el("div", { class: "engine-grid" }, entries.map((e) => {
    const name = e.doc.name;
    return el("a", {
      class: "engine-card",
      href: `#engine/${name}`,
      onclick: (ev) => {
        const m = ev;
        if (m.metaKey || m.ctrlKey || m.shiftKey)
          return;
        if (m.button != null && m.button !== 0)
          return;
        ev.preventDefault();
        ctx.goEngine(name);
      }
    }, el("span", { class: "engine-card-title" }, e.doc.title || name), e.bank ? el("span", { class: "engine-card-tag" }, "reads the card") : el("span", { class: "engine-card-tag muted" }, "no card needed"), el("span", { class: "engine-card-desc" }, describe(e)));
  })));
}

// src/ui/route.ts
function parseHash(hash) {
  const raw = hash.replace(/^#/, "");
  if (!raw)
    return { view: "", engine: null };
  const [head, ...rest] = raw.split("/");
  if (head === "engine") {
    const engine = rest.join("/").trim();
    return engine ? { view: "engine", engine } : { view: "engines", engine: null };
  }
  return { view: head, engine: null };
}

// src/ui/theme.ts
var THEMES = [
  {
    id: "light",
    label: "Light",
    note: "White paper, system font. The one for reading the manuals."
  },
  {
    id: "dark",
    label: "Dark",
    note: "The same page on a dark ground. For a dim room."
  }
];
var DEFAULT_THEME = THEMES[0].id;
var STORAGE_KEY = "sk-card-theme";
var THEME_ATTR = "data-theme";
function currentTheme() {
  try {
    const saved = localStorage.getItem(STORAGE_KEY);
    return THEMES.some((t) => t.id === saved) ? saved : DEFAULT_THEME;
  } catch {
    return DEFAULT_THEME;
  }
}
function applyTheme(id) {
  const theme = THEMES.find((t) => t.id === id) ?? THEMES[0];
  document.documentElement.setAttribute(THEME_ATTR, theme.id);
  try {
    localStorage.setItem(STORAGE_KEY, theme.id);
  } catch {}
}

// src/ui/main.ts
var VIEWS = {
  home: { mount: mountHome, label: "Overview" },
  engines: { mount: mountEngines, label: "Engines" },
  build: { mount: mountBuild, label: "Build a card", menu: "card" },
  convert: { mount: mountConvert, label: "Convert audio", menu: "card" },
  verify: { mount: mountVerify, label: "Verify a card", menu: "card" },
  reference: { mount: mountReference, label: "Card reference", menu: "card" },
  flash: { mount: mountFlash, label: "Flash firmware", menu: "device" },
  terminal: { mount: mountTerminal, label: "Terminal", menu: "device" },
  engine: { mount: mountEngine, label: "Engine" }
};
var DEFAULT_VIEW = "home";
var ENGINE_PANEL = "engine";
async function main() {
  let ctx;
  try {
    const [layoutData, engineData, patches] = await Promise.all([
      fetch("./card_layout.json").then((r) => {
        if (!r.ok)
          throw new Error(`cannot load ./card_layout.json: HTTP ${r.status}`);
        return r.json();
      }),
      fetch("./engines.json").then((r) => {
        if (!r.ok)
          throw new Error(`cannot load ./engines.json: HTTP ${r.status}`);
        return r.json();
      }),
      fetch("./patches.json").then((r) => r.ok ? r.json() : {})
    ]);
    const layout = makeLayout(layoutData);
    ctx = {
      layout,
      engines: makeCatalogue(engineData, layout),
      patches,
      engineFocus: new Store({ engine: null }),
      go: () => {},
      goEngine: () => {}
    };
  } catch (e) {
    showError($("#panels"), new Error(`${e.message}

This page is generated: run \`make web-data\` and serve web/ over http ` + "(file:// will not work - the browser blocks the fetch)."));
    return;
  }
  const mounted = new Set;
  const pageTitle = $("#page-title");
  const setTitle = (text) => {
    if (pageTitle)
      pageTitle.textContent = text;
    document.title = `${text} - sk-engines`;
  };
  function show(name) {
    if (!VIEWS[name])
      name = DEFAULT_VIEW;
    setTitle(VIEWS[name].label);
    for (const panel of $$("#panels > section"))
      panel.hidden = panel.id !== `panel-${name}`;
    if (!mounted.has(name)) {
      mounted.add(name);
      const root = $(`#panel-${name}`);
      try {
        VIEWS[name].mount(root, ctx);
      } catch (e) {
        showError(root, e);
      }
    }
    if (name === DEFAULT_VIEW) {
      if (location.hash)
        history.replaceState(null, "", location.pathname + location.search);
    } else if (parseHash(location.hash).view !== name) {
      history.replaceState(null, "", `#${name}`);
    }
    $(`#panel-${name}`)?.focus?.();
  }
  function showEngine(name) {
    show(ENGINE_PANEL);
    setTitle(name);
    ctx.engineFocus.set({ engine: name });
    if (location.hash !== `#engine/${name}`)
      history.replaceState(null, "", `#engine/${name}`);
  }
  ctx.go = show;
  ctx.goEngine = showEngine;
  window.addEventListener("hashchange", () => {
    const route2 = parseHash(location.hash);
    if (route2.engine)
      showEngine(route2.engine);
    else
      show(route2.view);
  });
  wireAboutMenu();
  $("#home-link")?.addEventListener("click", () => {
    document.activeElement?.blur?.();
    show(DEFAULT_VIEW);
  });
  $("#engines-link")?.addEventListener("click", () => {
    document.activeElement?.blur?.();
    show("engines");
  });
  buildEngineMenu(ctx, showEngine);
  buildActionMenu("#card-menu", "card", show);
  buildActionMenu("#device-menu", "device", show);
  buildThemeMenu();
  const route = parseHash(location.hash);
  if (route.engine)
    showEngine(route.engine);
  else
    show(route.view || DEFAULT_VIEW);
}
function wireAboutMenu() {
  const dialog = $("#about");
  const open = $("#about-open");
  const close = $("#about-close");
  if (!dialog || !open || !close)
    return;
  open.addEventListener("click", () => {
    open.blur();
    if (typeof dialog.showModal === "function")
      dialog.showModal();
    else
      dialog.setAttribute("open", "");
  });
  close.addEventListener("click", () => dialog.close());
}
function buildEngineMenu(ctx, onPick) {
  const host = $("#engines-menu");
  if (!host)
    return;
  host.append(el("ul", { role: "menu" }, ctx.engines.entries.map((e) => el("li", { role: "menu-item" }, el("button", {
    type: "button",
    onclick: () => {
      document.activeElement?.blur?.();
      onPick(e.doc.name);
    }
  }, e.doc.name, e.bank ? "" : el("span", { class: "muted" }, "  (no card)"))))));
}
function buildActionMenu(sel, group, go) {
  const host = $(sel);
  if (!host)
    return;
  const items = Object.entries(VIEWS).filter(([, v]) => v.menu === group);
  host.append(el("ul", { role: "menu" }, items.map(([id, v]) => el("li", { role: "menu-item" }, el("button", {
    type: "button",
    onclick: () => {
      document.activeElement?.blur?.();
      go(id);
    }
  }, v.label)))));
}
function buildThemeMenu() {
  const host = $("#theme-menu");
  if (!host)
    return;
  const render = () => {
    const active = currentTheme();
    host.querySelector("[role=menu]")?.remove();
    host.append(el("ul", { role: "menu" }, THEMES.map((t) => el("li", { role: "menu-item" }, el("button", {
      type: "button",
      title: t.note,
      onclick: () => {
        document.activeElement?.blur?.();
        applyTheme(t.id);
        render();
      }
    }, `${t.id === active ? "• " : "   "}${t.label}`)))));
  };
  render();
}
if ("serviceWorker" in navigator && location.protocol === "https:") {
  navigator.serviceWorker.register("./sw.js").catch(() => {});
}
main();

//# debugId=65428D650D8A5BA764756E2164756E21
//# sourceMappingURL=app.js.map
