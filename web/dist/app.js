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
function aside(summary, ...children) {
  return el("details", { class: "aside" }, el("summary", {}, summary), ...children);
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
function finding(cls, path, problem, fix) {
  return el("div", { class: `finding ${cls}` }, path && el("div", { class: "path" }, path), el("div", { class: "problem" }, problem), fix && el("div", { class: "fix" }, fix));
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
  if (items.length === 1 && typeof items[0].getAsFileSystemHandle === "function") {
    const handle = await items[0].getAsFileSystemHandle();
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
  const roots = items.map((i) => i.webkitGetAsEntry ? i.webkitGetAsEntry() : null).filter((e) => Boolean(e));
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
    for (const f of Array.from(dt.files))
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

// src/ui/build_view.ts
function mountBuild(root, ctx) {
  const model = new BuildModel(ctx.layout, ctx.patches, {
    access: cardAccess,
    downloader,
    deflate: deflateRaw
  });
  const status = el("div", { class: "status" });
  const out = el("div", { class: "results" });
  const inPlace = el("button", { class: "primary", onclick: () => model.writeInPlace() }, "Write onto a card");
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
  root.append(el("p", { class: "lead" }, "Makes an empty card the firmware can read. Format it FAT32 first."), el("div", { class: "controls" }, el("button", { class: "primary", onclick: () => model.downloadZip() }, "Download a starter card (.zip)"), inPlace), status, el("p", { class: "muted note" }, "Unpack it so the folders sit at the card's root. Then add audio on ", el("a", { href: "#convert" }, "Convert"), ", and if anything misbehaves later, point ", el("a", { href: "#verify" }, "Verify"), " at the card."), out, aside(`What it creates - ${b.files.length} files, ${b.dirs.length} folders`, el("p", {}, "Every folder the firmware looks for, a README in each one restating that folder's " + "rules, the default SK/config.txt, radio/rate.txt, bard/BARD.CFG, and the example chuck and " + "csound patches. Byte for byte the card ", el("code", {}, "sk_card.py init --no-demo"), " builds, and it passes Verify with nothing to report."), folders), aside("Want demo audio too?", el("p", {}, "The released ", el("code", {}, "sk-card-<version>.zip"), " is a complete card with synthesized audio for every engine, and it is checksummed. This page " + "builds the skeleton only rather than regenerating that content, so what you download from " + "the release is what everyone else has. ", el("a", {
    href: "https://github.com/shakfu/sk-engines/releases/latest",
    target: "_blank",
    rel: "noreferrer"
  }, "Get it from the latest release"), ".")));
}

// src/core/wav.ts
var F32 = "f32";
var INT16 = "int16";
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
    if (this.fmt === WAVE_FORMAT_PCM && this.bits === 16)
      return INT16;
    return null;
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
  root.append(el("p", { class: "lead" }, "Converts your audio to exactly what the target engine reads. mp3, flac, wav, ogg, m4a."), el("div", { class: "fields" }, field("Engine", engineSel), deckField, bankField, tapeField, slotField, rateField), targetNote, el("div", { class: "controls" }, el("button", { onclick: () => picker.click() }, "Choose audio files"), convertBtn, zipBtn, saveBtn, picker), drop, fileList, status, out, aside("On resampling, and why this is not the CLI", el("p", {}, "The browser's resampler is not bit-identical to libsox's or ffmpeg's. None of the three " + "agree with each other today, so this is not a regression - but it does mean this page cannot " + "reproduce a particular card byte for byte. For a 50x pstretch source, where artefacts have a " + "long time to become audible, converting with ffmpeg is worth comparing against."), el("p", {}, "The upside is the reason this tab exists: decoding happens in the browser's own audio engine, " + "so there is no install and no format-support lottery. The CLI needs ffmpeg, or cysox plus a " + "libsox built with the right handlers.")));
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
      out.push(finding2("error", rel, `extension ${ext || "(none)"} is not indexed by the scan`, `Use .raw or .wav (${bank.fmt.describe}).`));
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
async function checkAudioFormat(_layout, entry, bank, out) {
  const rel = entry.path;
  const fmt = bank.fmt;
  const ext = suffixOf(baseOf(rel)).toLowerCase();
  if (fmt.container === "raw" && ext === ".raw") {
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
  if (!info.encoding || !fmt.encodings.includes(info.encoding)) {
    problems.push(`encoding is ${info.describe().split(",")[0]}`);
  }
  if (fmt.channels != null && info.channels !== fmt.channels)
    problems.push(`${info.channels} channel(s)`);
  if (fmt.rate != null && info.rate !== fmt.rate)
    problems.push(`${info.rate} Hz`);
  if (problems.length) {
    out.push(finding2("error", rel, `wrong format (${problems.join(", ")}) - the firmware reads the bytes as-is, so this plays as ` + "noise or not at all", `Needs: ${fmt.describe}. Fix it on the Convert tab, or: sk_card.py convert --engine ` + `${bank.engine} CARD ${baseOf(rel)}`));
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
  const drop = el("div", { class: "dropzone" }, el("p", {}, "Drop the card folder here"), el("p", { class: "muted" }, "or pick it below. Nothing is uploaded - the check runs in this tab."));
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
  root.append(el("p", { class: "lead" }, "Checks a card and explains anything that will not work."), controls, drop, status, results, aside("Why a bad card gives no error on the device", el("p", {}, `Engines read this card using ${engineBanks(ctx.layout)} folder layouts and ` + `${audioFormats(ctx.layout)} incompatible audio formats, and the firmware converts nothing. A ` + "file in the wrong format is not rejected, it is read as raw bytes and plays as noise; a " + `filename over ${ctx.layout.scan.max_name} characters is skipped by the directory scan with ` + "no error shown. The hardware's only feedback is an LED, so every one of these fails " + "silently. This finds all of it.")));
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
  entries;
  constructor(layout) {
    this.layout = layout;
    this.entries = layout.banks.map((bank) => ({
      bank,
      haystack: [
        bank.engine,
        bank.dirs.join(" "),
        bank.readers.join(" "),
        bank.fmt.describe,
        bank.blurb,
        bank.slots.join(" "),
        bank.target
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
  setShowSources(on) {
    this.store.set({ showSources: on });
  }
  visible() {
    const { query, pinned } = this.store.get();
    const q = query.trim().toLowerCase();
    return this.entries.filter((e) => pinned ? e.bank.engine === pinned : !q || e.haystack.includes(q)).map((e) => e.bank);
  }
  status() {
    const { query, pinned } = this.store.get();
    const shown = this.visible().length;
    return pinned || query.trim() ? `${shown} of ${this.entries.length} shown` : `${this.entries.length} folder layouts`;
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
function bankSection(layout, bank) {
  const rows = [specRow("Format", bank.fmt.describe)];
  if (bank.slots.length) {
    const row = specRow(`Names (${bank.slots.length})`, slotList(bank.slots), true);
    row.querySelector("td").title = bank.slots.join(", ");
    rows.push(row);
  }
  if (bank.scanned) {
    rows.push(specRow("Scanned", `any name of at most ${layout.scan.max_name} characters ending ` + `${extList(layout.scan.extensions)}, at least ${layout.scan.min_bytes / 1024} KB` + `${bank.max_files ? `, at most ${bank.max_files} per folder` : ""}`));
  }
  if (bank.max_seconds) {
    rows.push(specRow("Length", `about ${seconds(bank.max_seconds)} s at most - this engine loads the ` + "whole file into RAM, so anything longer is trimmed"));
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
  return el("section", { class: "ref-bank", dataset: { engine: bank.engine } }, el("h3", {}, bank.engine, " ", el("span", { class: "mono muted" }, folderLabel(bank.dirs))), bank.blurb && el("p", { class: "muted" }, bank.blurb), el("table", { class: "layout spec" }, el("tbody", {}, rows)), bank.kind === "config" && configTable(layout));
}
function mountReference(root, ctx) {
  const model = new ReferenceModel(ctx.layout);
  const status = el("div", { class: "status" });
  const sections = new Map(ctx.layout.banks.map((b) => [b.engine, bankSection(ctx.layout, b)]));
  const filter = el("input", {
    type: "text",
    class: "filter",
    placeholder: "Filter by engine, folder or format",
    autocomplete: "off",
    oninput: () => model.setQuery(filter.value)
  });
  const chips = el("div", { class: "chips" }, ctx.layout.banks.map((b) => el("button", { class: "link", onclick: () => model.toggleChip(b.engine) }, b.engine)));
  const srcToggle = el("input", {
    type: "checkbox",
    onchange: () => model.setShowSources(srcToggle.checked)
  });
  const banksEl = el("div", { class: "ref-banks" }, [...sections.values()]);
  model.store.subscribe((s) => {
    const visible = new Set(model.visible().map((b) => b.engine));
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
  root.append(el("p", { class: "lead" }, "What each engine expects on the card."), el("div", { class: "controls" }, filter, el("label", { class: "field inline" }, srcToggle, el("span", {}, "firmware sources"))), chips, status, everywhere(model.scan()), banksEl, aside("Where are the other engines?", el("p", {}, "An engine not listed here reads nothing from the card and needs no folder at all - most of the " + "effects are in that group. Everything above is generated from the same table the firmware " + "and the command-line tools read, so it is the same content as ", el("code", {}, "python3 scripts/sk_card.py layout"), ".")));
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
  error: null
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
  write(text, kind = "meta") {
    const lines = [...this.store.get().lines, { text, kind }];
    this.store.set({ lines: lines.slice(-CONSOLE_LIMIT) });
  }
  async connect({ filtered = true } = {}) {
    try {
      const transport = await this.deps.serial.request({ filtered });
      transport.onClose((why) => this.lost(why));
      this.device = new Device(transport, { logSink: (l) => this.write(l, "log") });
      this.store.set({
        connected: true,
        port: transport.info(),
        status: `connected (${transport.info()})`,
        offerAllPorts: false,
        error: null
      });
      this.write("connected", "meta");
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
    if (isDestructive(line) && !this.deps.confirm(`Send "${line}"?`)) {
      this.write(`cancelled: ${line}`, "meta");
      return null;
    }
    if (!quiet)
      this.write(`> ${line}`, "sent");
    try {
      const reply = await this.device.cmd(line);
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
  async refreshDescribe() {
    if (!this.device)
      return;
    try {
      this.store.set({ descriptor: parseDescribe(await this.device.describeLines()) });
    } catch {
      this.store.set({ descriptor: null });
    }
  }
  async refreshUsb() {
    const reply = await this.send("query usb", { quiet: true });
    if (reply == null) {
      this.store.set({ usb: [], usbAvailable: false });
      return;
    }
    this.store.set({ usb: parseUsbDiag(reply), usbAvailable: true });
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
    await this.send("reset cpu");
    this.store.set({ cpuHistory: [] });
  }
}

// src/platform/serial.ts
var DAISY_VID = 1155;
var BAUD = 115200;
var supported = () => typeof navigator !== "undefined" && navigator.serial != null;
async function requestPort({ filtered = true } = {}) {
  if (!supported()) {
    throw new Error("This browser has no WebSerial. Use Chrome, Edge or another Chromium browser.");
  }
  const serial = navigator.serial;
  const port = await serial.requestPort(filtered ? { filters: [{ usbVendorId: DAISY_VID }] } : {});
  await port.open({ baudRate: BAUD });
  try {
    await port.setSignals?.({ dataTerminalReady: true });
  } catch {}
  return new SerialTransport(port);
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
    const i = this.port.getInfo?.() ?? {};
    const hex = (v) => v == null ? "?" : `0x${v.toString(16).padStart(4, "0")}`;
    return `USB ${hex(i.usbVendorId)}:${hex(i.usbProductId)}`;
  }
}
var webSerial = { supported, request: requestPort };

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
      if (/^(config|set param|reset|preset)\b/.test(line))
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
      await model.send(`set param ${p.name} ${deck} ${slider.value}`, { quiet: true });
    });
    return el("div", { class: "row" }, el("label", {}, `${p.name}${p.scope === "deck" ? ` ${deck}` : ""}`), slider, out, el("button", {
      class: "link",
      onclick: async () => {
        const v = await model.send(`get param ${p.name} ${deck}`, { quiet: true });
        if (v != null) {
          slider.value = v;
          out.textContent = Number(v).toPrecision(4);
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
      const grid = el("div", { class: "grid" });
      for (const p of descriptor.params.values()) {
        for (const deck of p.scope === "deck" ? decks : ["A"])
          grid.append(paramRow(p, deck));
      }
      surface.append(el("h4", {}, "Parameters"), grid);
    }
    if (descriptor.configs.size) {
      const grid = el("div", { class: "grid" });
      for (const c of descriptor.configs.values()) {
        const sel = el("select", { onchange: () => model.send(`config ${c.name} A ${sel.value}`) }, [...c.values.entries()].map(([v, lbl]) => el("option", { value: String(v) }, `${v} - ${lbl}`)));
        grid.append(el("div", { class: "row" }, el("label", {}, c.name), sel));
      }
      surface.append(el("h4", {}, "Configs"), grid);
    }
    const actions = el("div", { class: "actions" });
    for (const deck of decks) {
      for (const [label, cmd] of [
        [`gate ${deck}`, `gate ${deck}`],
        [`play ${deck}`, `pad play ${deck}`],
        [`rec ${deck}`, `pad rec ${deck}`],
        [`stop ${deck}`, `pad stop ${deck}`],
        [`clear ${deck}`, `pad clear ${deck}`]
      ]) {
        actions.append(el("button", {
          class: isDestructive(cmd) ? "danger" : "",
          onclick: () => model.send(cmd)
        }, label));
      }
    }
    surface.append(el("h4", {}, "Actions"), actions);
    if (descriptor.queries.size) {
      const list = el("div", { class: "grid" });
      for (const q of descriptor.queries.values()) {
        const value = el("span", { class: "mono value" }, "-");
        list.append(el("div", { class: "row" }, el("label", {}, q.name), el("button", {
          onclick: async () => {
            const r = await model.send(`query ${q.name} ${q.scope === "deck" ? "A" : ""}`.trim(), { quiet: true });
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
  append(root, [
    el("div", { class: "callout warn" }, el("strong", {}, "Released firmware has no terminal. "), "Needs a build you make yourself: ", el("code", {}, "make ENGINE=<engine> TERMINAL=1"), "."),
    el("div", { class: "controls" }, connectBtn, allPortsBtn, status),
    !model.supported() && el("div", { class: "callout" }, el("strong", {}, "This browser has no WebSerial. "), "Talking to hardware needs Chrome or Edge - and unlike the card tabs there is no fallback here, " + "because there is no zip-shaped substitute for a serial port."),
    el("h3", {}, "Console"),
    log,
    input,
    el("h3", {}, "CPU load"),
    cpuPanel,
    el("h3", {}, "Control surface"),
    surface,
    el("h3", {}, "USB bring-up"),
    usbPanel,
    aside("Why released firmware has no terminal, and what it costs", el("p", {}, "scripts/build_release.py never passes TERMINAL=1, so every binary in dist/ lacks the command " + "channel and this tab finds nothing to talk to. Shipping terminal-enabled releases is an open " + "firmware decision: it costs ~19-25 KB of SRAM_EXEC everywhere, and on the QSPI engines " + "(chuck, csound, mosc) it costs USB MIDI, which claims the same OTG core."), el("p", {}, "The control surface above is generated from the device's own `describe` reply - every control " + "is one this build actually advertises, and nothing appears for the enum entries it ignores. " + "Destructive verbs ask before firing."))
  ]);
}

// src/ui/main.ts
var VIEWS = {
  build: mountBuild,
  convert: mountConvert,
  verify: mountVerify,
  reference: mountReference,
  terminal: mountTerminal
};
var DEFAULT_VIEW = Object.keys(VIEWS)[0];
async function main() {
  let ctx;
  try {
    const [layoutData, patches] = await Promise.all([
      fetch("./card_layout.json").then((r) => {
        if (!r.ok)
          throw new Error(`cannot load ./card_layout.json: HTTP ${r.status}`);
        return r.json();
      }),
      fetch("./patches.json").then((r) => r.ok ? r.json() : {})
    ]);
    ctx = { layout: makeLayout(layoutData), patches };
  } catch (e) {
    showError($("#panels"), new Error(`${e.message}

This page is generated: run \`make web-data\` and serve web/ over http ` + "(file:// will not work - the browser blocks the fetch)."));
    return;
  }
  const mounted = new Set;
  function show(name) {
    if (!VIEWS[name])
      name = DEFAULT_VIEW;
    for (const tab of $$("#tabs button")) {
      tab.classList.toggle("active", tab.dataset.view === name);
      tab.setAttribute("aria-selected", String(tab.dataset.view === name));
    }
    for (const panel of $$("#panels > section"))
      panel.hidden = panel.id !== `panel-${name}`;
    if (!mounted.has(name)) {
      mounted.add(name);
      const root = $(`#panel-${name}`);
      try {
        VIEWS[name](root, ctx);
      } catch (e) {
        showError(root, e);
      }
    }
    if (location.hash.slice(1) !== name)
      history.replaceState(null, "", `#${name}`);
  }
  for (const tab of $$("#tabs button")) {
    tab.addEventListener("click", () => show(tab.dataset.view ?? DEFAULT_VIEW));
  }
  window.addEventListener("hashchange", () => show(location.hash.slice(1)));
  $("#banner").append(el("span", { class: "muted" }, `${ctx.layout.banks.length} banks, scan floor ${ctx.layout.scan.min_bytes / 1024} KB, ` + `name limit ${ctx.layout.scan.max_name}`));
  show(location.hash.slice(1) || DEFAULT_VIEW);
}
if ("serviceWorker" in navigator && location.protocol === "https:") {
  navigator.serviceWorker.register("./sw.js").catch(() => {});
}
main();

//# debugId=174E19FE926E743464756E2164756E21
//# sourceMappingURL=app.js.map
