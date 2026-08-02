// cardsource.test.ts - the drop path, against a drag data store that behaves like a real one.
//
// This is the one part of `platform/` worth testing here, and the reason is a bug it actually had.
// A `DataTransfer` is only valid for the duration of the drop event's task: once that task ends the
// browser invalidates the store, and every `DataTransferItem` in it starts returning null. Code that
// awaits anything and *then* reads `dt.items` therefore works in every test written against a plain
// object, and fails in a browser - silently, with the dropzone reporting zero files.
//
// The fake below models that invalidation, which is what makes these tests worth more than the shape
// assertions they look like.

import { suite, test, ok, eq } from './harness.ts';
import { fromDataTransfer, fromFileList } from '../src/platform/cardsource.ts';

suite('cardsource');

/** A File stand-in: the code only ever reads name/size and slices it. */
function fakeFile(name: string, size = 64): File {
  const bytes = new Uint8Array(size);
  return {
    name,
    size,
    webkitRelativePath: '',
    slice: () => ({ arrayBuffer: async () => bytes.buffer }),
    arrayBuffer: async () => bytes.buffer,
  } as unknown as File;
}

/** A directory entry for the legacy `webkitGetAsEntry` tree walk. */
function dirEntry(name: string, children: Array<ReturnType<typeof fileEntry> | unknown>): unknown {
  let handed = false;
  return {
    name,
    isFile: false,
    isDirectory: true,
    createReader: () => ({
      readEntries(cb: (list: unknown[]) => void) {
        // readEntries returns at most 100 at a time and signals the end with an empty list.
        cb(handed ? [] : children);
        handed = true;
      },
    }),
  };
}

function fileEntry(name: string): unknown {
  return {
    name,
    isFile: true,
    isDirectory: false,
    file: (cb: (f: File) => void) => cb(fakeFile(name)),
  };
}

/**
 * A DataTransfer that INVALIDATES itself after the current task, as the browser does.
 *
 * `items` empties, every item stops answering, and `files` empties too. That last one is the
 * pessimistic reading of the spec - the drag data store is disabled wholesale - and it is the one to
 * model, because code that only works when a browser is lenient about it is code that works until it
 * meets a browser that is not.
 */
function expiringTransfer(opts: {
  entries?: unknown[];
  files?: File[];
  handle?: { kind: string } | null;
}): DataTransfer {
  const entries = opts.entries ?? [];
  const files = opts.files ?? [];
  let live = true;
  queueMicrotask(() => { live = false; });

  const items = entries.map((entry, i) => ({
    kind: 'file',
    webkitGetAsEntry: () => (live ? entry : null),
    ...(opts.handle !== undefined
      ? { getAsFileSystemHandle: async () => (i === 0 ? opts.handle : null) }
      : {}),
  }));

  return {
    get items() {
      return live ? items : [];
    },
    get files() {
      return live ? files : [];
    },
  } as unknown as DataTransfer;
}

test('a dropped folder is read even though the drag store expires', async () => {
  const dt = expiringTransfer({
    entries: [dirEntry('CARD', [fileEntry('README.TXT'), fileEntry('1.WAV')])],
  });
  const card = await fromDataTransfer(dt);
  // The dropped folder IS the card root, so its own name is not part of any path.
  eq(card.files.map((f) => f.path).sort(), ['1.WAV', 'README.TXT']);
});

test('a single loose file survives the store expiring - the bug this file exists for', async () => {
  // The old order was: await getAsFileSystemHandle(), THEN read webkitGetAsEntry(). By the second
  // read the store is gone, the entry list is empty, and the drop yields nothing at all.
  const dt = expiringTransfer({
    entries: [fileEntry('CLIP01.WAV')],
    files: [fakeFile('CLIP01.WAV')],
    handle: { kind: 'file' }, // Chromium answers with a FILE handle, so the directory path is skipped
  });
  const card = await fromDataTransfer(dt);
  eq(card.files.map((f) => f.path), ['CLIP01.WAV']);
});

test('several dropped files all arrive', async () => {
  const dt = expiringTransfer({
    entries: [fileEntry('a.wav'), fileEntry('b.wav')],
    files: [fakeFile('a.wav'), fakeFile('b.wav')],
  });
  const card = await fromDataTransfer(dt);
  eq(card.files.map((f) => f.path).sort(), ['a.wav', 'b.wav']);
});

test('a dropped directory handle takes the writable path', async () => {
  // Chromium hands back a FileSystemDirectoryHandle, which is what unlocks in-place editing from a
  // drag rather than only from the picker.
  const handle = {
    kind: 'directory',
    name: 'CARD',
    async *entries() {
      yield ['README.TXT', { kind: 'file', getFile: async () => fakeFile('README.TXT') }];
    },
  };
  const dt = expiringTransfer({ entries: [dirEntry('CARD', [])], handle: handle as never });
  const card = await fromDataTransfer(dt);
  eq(card.files.map((f) => f.path), ['README.TXT']);
  ok(card.handle, 'the writable handle is kept - this is what "can be edited in place" means');
});

test('a folder picked with <input webkitdirectory> is made card-relative', async () => {
  // webkitRelativePath is "<pickedFolder>/tapes/x.wav", so the first segment is stripped. If it were
  // not, every path would resolve to the wrong bank and the findings would be nonsense.
  const withPath = (name: string, rel: string): File => {
    const f = fakeFile(name);
    Object.defineProperty(f, 'webkitRelativePath', { value: rel });
    return f;
  };
  const card = fromFileList([
    withPath('tape_a_1.wav', 'MYCARD/tapes/tape_a_1.wav'),
    withPath('config.txt', 'MYCARD/SK/config.txt'),
  ]);
  eq(card.files.map((f) => f.path).sort(), ['SK/config.txt', 'tapes/tape_a_1.wav']);
  ok(card.dirs.has('tapes') && card.dirs.has('SK'), 'and the parent folders are recorded');
  eq(card.handle, null, 'this path is read-only - there is no handle to write back through');
});
