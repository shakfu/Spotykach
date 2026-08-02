// cardsource.ts - where the card comes from, and where output goes.
//
// Three ways in, in descending order of capability, because only the first is Chromium-only:
//
//   1. File System Access (`showDirectoryPicker`, or a dropped FileSystemDirectoryHandle) - reads the
//      card AND writes back to it in place. Chrome/Edge.
//   2. A dropped folder, or <input webkitdirectory> - reads everything, writes nothing. Works in
//      Safari and Firefox too, and pairs with the zip download.
//   3. Loose dropped files - enough for Convert, not enough for Verify.
//
// The rest of the app sees one shape regardless: a `Card` of `{path, size, read()}` entries, declared
// in core. Designing for the read-only path first is what keeps the tool useful outside Chrome instead
// of showing most of a music-hardware audience a browser-upgrade notice.

import type { CardAccess } from '../core/ports.ts';
import type { Card, CardEntry, CardFile, WriteResult } from '../core/types.ts';

// The File System Access API is not in every TS DOM lib yet, and only this file needs its shape.
interface FileSystemHandleLike {
  kind: 'file' | 'directory';
  name: string;
}
interface FileHandleLike extends FileSystemHandleLike {
  kind: 'file';
  getFile(): Promise<File>;
}
interface DirHandleLike extends FileSystemHandleLike {
  kind: 'directory';
  entries(): AsyncIterableIterator<[string, FileHandleLike | DirHandleLike]>;
  getDirectoryHandle(name: string, opts?: { create?: boolean }): Promise<DirHandleLike>;
  getFileHandle(name: string, opts?: { create?: boolean }): Promise<FileHandleLike & {
    createWritable(): Promise<{ write(data: Uint8Array): Promise<void>; close(): Promise<void> }>;
  }>;
}

type PickerWindow = Window & {
  showDirectoryPicker?: (opts?: { mode?: string; id?: string }) => Promise<DirHandleLike>;
};

export const hasFileSystemAccess = (): boolean =>
  typeof window !== 'undefined' && 'showDirectoryPicker' in window;

/** Lazily read a File, optionally only its first `max` bytes. */
function entryFor(path: string, file: File): CardEntry {
  return {
    path,
    size: file.size,
    async read(max?: number) {
      const blob = max != null && max < file.size ? file.slice(0, max) : file;
      return new Uint8Array(await blob.arrayBuffer());
    },
  };
}

/** Walk a FileSystemDirectoryHandle into the common shape. */
export async function fromDirectoryHandle(handle: DirHandleLike): Promise<Card> {
  const files: CardEntry[] = [];
  const dirs = new Set<string>();
  const walk = async (dir: DirHandleLike, prefix: string, depth: number): Promise<void> => {
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
export async function pickDirectory(mode: 'read' | 'readwrite' = 'read'): Promise<Card> {
  const w = window as PickerWindow;
  if (!w.showDirectoryPicker) {
    throw new Error('This browser has no File System Access API. Drop the card folder onto the page '
      + 'instead, or use Chrome/Edge to edit a card in place.');
  }
  return fromDirectoryHandle(await w.showDirectoryPicker({ mode, id: 'sk-card' }));
}

/**
 * A FileList from `<input type=file webkitdirectory>`. `webkitRelativePath` is
 * "<pickedFolder>/tapes/x.wav", so the first segment is stripped to make paths card-relative.
 */
export function fromFileList(list: ArrayLike<File>): Card {
  const files: CardEntry[] = [];
  const dirs = new Set<string>();
  for (const file of Array.from(list)) {
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

// The legacy drag-and-drop entry API, which is what every non-Chromium browser still offers.
interface FileSystemEntryLike {
  name: string;
  isFile: boolean;
  isDirectory: boolean;
  file(cb: (f: File) => void, err?: () => void): void;
  createReader(): { readEntries(cb: (list: FileSystemEntryLike[]) => void, err?: () => void): void };
}

/**
 * A drop. Prefers `getAsFileSystemHandle` (Chromium) because that yields a writable handle and so
 * unlocks in-place editing from a drag; falls back to the older `webkitGetAsEntry` tree walk, which
 * every browser has and which is read-only.
 *
 * EVERY read of the drag data store happens synchronously at the top, before the first `await`.
 *
 * That is not style. The store is invalidated once the drop event's task ends, so a `DataTransferItem`
 * touched after an await can return null and the drop silently yields nothing - and "silently" is the
 * whole problem: the user sees a dropzone accept their folder and then report zero files. The previous
 * version called `webkitGetAsEntry()` after awaiting `getAsFileSystemHandle()`, which is exactly that
 * bug for a single loose file. Promises started here are awaited below; starting them is synchronous.
 */
export async function fromDataTransfer(dt: DataTransfer): Promise<Card> {
  type ItemWithHandle = DataTransferItem & {
    getAsFileSystemHandle?: () => Promise<FileSystemHandleLike | null>;
  };
  const items = [...dt.items].filter((i) => i.kind === 'file') as ItemWithHandle[];
  const roots = items
    .map((i) => (i.webkitGetAsEntry ? i.webkitGetAsEntry() as unknown as FileSystemEntryLike : null))
    .filter((e): e is FileSystemEntryLike => Boolean(e));
  const plainFiles = [...dt.files];
  const pendingHandle = items.length === 1 && typeof items[0].getAsFileSystemHandle === 'function'
    ? items[0].getAsFileSystemHandle()
    : null;

  if (pendingHandle) {
    const handle = await pendingHandle;
    if (handle && handle.kind === 'directory') return fromDirectoryHandle(handle as DirHandleLike);
  }

  const files: CardEntry[] = [];
  const dirs = new Set<string>();
  const readEntry = (entry: FileSystemEntryLike, prefix: string): Promise<void> =>
    new Promise((resolve) => {
      const path = prefix ? `${prefix}/${entry.name}` : entry.name;
      if (entry.isFile) {
        entry.file((f) => {
          files.push(entryFor(path, f));
          resolve();
        }, () => resolve());
      } else if (entry.isDirectory) {
        dirs.add(path);
        const reader = entry.createReader();
        const batch = (): void => reader.readEntries(async (list) => {
          if (!list.length) return resolve();
          await Promise.all(list.map((e) => readEntry(e, path)));
          batch(); // readEntries returns at most 100 at a time; keep going until it returns none
        }, () => resolve());
        batch();
      } else {
        resolve();
      }
    });

  if (roots.length === 1 && roots[0].isDirectory) {
    // A single dropped folder IS the card root, so its own name is not part of any path.
    const reader = roots[0].createReader();
    await new Promise<void>((resolve) => {
      const batch = (): void => reader.readEntries(async (list) => {
        if (!list.length) return resolve();
        await Promise.all(list.map((e) => readEntry(e, '')));
        batch();
      }, () => resolve());
      batch();
    });
    return { files, dirs, handle: null };
  }
  await Promise.all(roots.map((e) => readEntry(e, '')));
  if (!roots.length) {
    for (const f of plainFiles) files.push(entryFor(f.name, f));
  }
  return { files, dirs, handle: null };
}

/**
 * Write files (and create folders) under a directory handle. Used by Build and Convert when the user
 * picked the card itself rather than taking a zip.
 */
export async function writeInto(
  handle: unknown, files: CardFile[], dirs: string[] = [],
): Promise<WriteResult> {
  const root = handle as DirHandleLike;
  const written: string[] = [];
  const failed: Array<{ path: string; error: string }> = [];

  const dirHandle = async (path: string): Promise<DirHandleLike> => {
    let cur = root;
    for (const part of path.split('/').filter(Boolean)) {
      cur = await cur.getDirectoryHandle(part, { create: true });
    }
    return cur;
  };

  for (const d of dirs) {
    try {
      await dirHandle(d);
    } catch (e) {
      failed.push({ path: d, error: (e as Error).message });
    }
  }
  for (const f of files) {
    try {
      const i = f.path.lastIndexOf('/');
      const dir = i < 0 ? root : await dirHandle(f.path.slice(0, i));
      const fh = await dir.getFileHandle(f.path.slice(i + 1), { create: true });
      const w = await fh.createWritable();
      await w.write(f.bytes);
      await w.close();
      written.push(f.path);
    } catch (e) {
      failed.push({ path: f.path, error: (e as Error).message });
    }
  }
  return { written, failed };
}

/** The `CardAccess` port, as the models consume it. */
export const cardAccess: CardAccess = {
  hasDirectAccess: hasFileSystemAccess,
  pickDirectory,
  writeInto,
};
