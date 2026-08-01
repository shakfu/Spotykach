// cardsource.js - where the card comes from, and where output goes.
//
// Three ways in, in descending order of capability, because only the first is Chromium-only:
//
//   1. File System Access (`showDirectoryPicker`, or a dropped FileSystemDirectoryHandle) - reads the
//      card AND writes back to it in place. Chrome/Edge.
//   2. A dropped folder, or <input webkitdirectory> - reads everything, writes nothing. Works in
//      Safari and Firefox too, and pairs with the zip download.
//   3. Loose dropped files - enough for Convert, not enough for Verify.
//
// The rest of the app sees one shape regardless: a list of `{path, size, read()}`. Designing for the
// read-only path first is what keeps the tool useful outside Chrome instead of showing most of a
// music-hardware audience a browser-upgrade notice.

/** @typedef {import('./verify.js').CardEntry} CardEntry */

export const hasFileSystemAccess = () => typeof window !== 'undefined' && 'showDirectoryPicker' in window;

/** Lazily read a File, optionally only its first `max` bytes. */
function entryFor(path, file) {
  return {
    path,
    size: file.size,
    file,
    async read(max) {
      const blob = max != null && max < file.size ? file.slice(0, max) : file;
      return new Uint8Array(await blob.arrayBuffer());
    },
  };
}

/**
 * Walk a FileSystemDirectoryHandle into the common shape.
 * @returns {Promise<{files: CardEntry[], dirs: Set<string>, handle: FileSystemDirectoryHandle}>}
 */
export async function fromDirectoryHandle(handle) {
  const files = [];
  const dirs = new Set();
  const walk = async (dir, prefix, depth) => {
    // A card is at most three levels deep (SK/B/1.WAV). The bound is a guard against a user picking
    // their home directory by mistake, not a layout rule.
    if (depth > 6) return;
    for await (const [name, child] of dir.entries()) {
      const path = prefix ? `${prefix}/${name}` : name;
      if (child.kind === 'directory') {
        dirs.add(path);
        await walk(child, path, depth + 1);
      } else {
        files.push(entryFor(path, await child.getFile()));
      }
    }
  };
  await walk(handle, '', 0);
  return { files, dirs, handle };
}

/** Prompt for the card's root folder. Must be called from a user gesture. */
export async function pickDirectory(mode = 'read') {
  if (!hasFileSystemAccess()) {
    throw new Error('This browser has no File System Access API. Drop the card folder onto the page '
      + 'instead, or use Chrome/Edge to edit a card in place.');
  }
  return fromDirectoryHandle(await window.showDirectoryPicker({ mode, id: 'sk-card' }));
}

/**
 * A FileList from `<input type=file webkitdirectory>`. `webkitRelativePath` is
 * "<pickedFolder>/tapes/x.wav", so the first segment is stripped to make paths card-relative.
 */
export function fromFileList(list) {
  const files = [];
  const dirs = new Set();
  for (const file of list) {
    const rel = (file.webkitRelativePath || file.name).split('/').slice(1).join('/') || file.name;
    files.push(entryFor(rel, file));
    let d = rel.includes('/') ? rel.slice(0, rel.lastIndexOf('/')) : '';
    while (d) {
      dirs.add(d);
      d = d.includes('/') ? d.slice(0, d.lastIndexOf('/')) : '';
    }
  }
  return { files, dirs, handle: null };
}

/**
 * A drop. Prefers `getAsFileSystemHandle` (Chromium) because that yields a writable handle and so
 * unlocks in-place editing from a drag; falls back to the older `webkitGetAsEntry` tree walk, which
 * every browser has and which is read-only.
 */
export async function fromDataTransfer(dt) {
  const items = [...dt.items].filter((i) => i.kind === 'file');

  if (items.length === 1 && typeof items[0].getAsFileSystemHandle === 'function') {
    const handle = await items[0].getAsFileSystemHandle();
    if (handle && handle.kind === 'directory') return fromDirectoryHandle(handle);
  }

  const files = [];
  const dirs = new Set();
  const readEntry = (entry, prefix) => new Promise((resolve) => {
    const path = prefix ? `${prefix}/${entry.name}` : entry.name;
    if (entry.isFile) {
      entry.file((f) => {
        files.push(entryFor(path, f));
        resolve();
      }, resolve);
    } else if (entry.isDirectory) {
      dirs.add(path);
      const reader = entry.createReader();
      const batch = () => reader.readEntries(async (list) => {
        if (!list.length) return resolve();
        await Promise.all(list.map((e) => readEntry(e, path)));
        batch(); // readEntries returns at most 100 at a time; keep going until it returns none
      }, resolve);
      batch();
    } else {
      resolve();
    }
  });

  const roots = items.map((i) => (i.webkitGetAsEntry ? i.webkitGetAsEntry() : null)).filter(Boolean);
  if (roots.length === 1 && roots[0].isDirectory) {
    // A single dropped folder IS the card root, so its own name is not part of any path.
    const reader = roots[0].createReader();
    await new Promise((resolve) => {
      const batch = () => reader.readEntries(async (list) => {
        if (!list.length) return resolve();
        await Promise.all(list.map((e) => readEntry(e, '')));
        batch();
      }, resolve);
      batch();
    });
    return { files, dirs, handle: null };
  }
  await Promise.all(roots.map((e) => readEntry(e, '')));
  if (!roots.length) {
    for (const f of dt.files) files.push(entryFor(f.name, f));
  }
  return { files, dirs, handle: null };
}

/**
 * Write files (and create folders) under a directory handle. Used by Build and Convert when the user
 * picked the card itself rather than taking a zip.
 *
 * @returns {Promise<{written: string[], failed: Array<{path: string, error: string}>}>}
 */
export async function writeInto(handle, files, dirs = []) {
  const written = [];
  const failed = [];

  const dirHandle = async (path) => {
    let cur = handle;
    for (const part of path.split('/').filter(Boolean)) {
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
      const i = f.path.lastIndexOf('/');
      const dir = i < 0 ? handle : await dirHandle(f.path.slice(0, i));
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
