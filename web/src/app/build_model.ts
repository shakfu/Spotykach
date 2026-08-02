// build_model.ts - "make me a card that works", with no DOM in sight.

import { buildCard, missingFrom, type BuiltCard } from '../core/build.ts';
import type { Layout } from '../core/layout.ts';
import type { CardAccess, Deflate, Downloader } from '../core/ports.ts';
import { makeZip, ZIP_MIME } from '../core/zip.ts';
import { Store } from './store.ts';

export interface Verdict {
  kind: 'good' | 'mixed';
  title: string;
  detail: string;
}

export interface BuildState {
  status: string;
  verdict: Verdict | null;
  failures: Array<{ path: string; error: string }>;
  error: string | null;
  busy: boolean;
}

export interface BuildDeps {
  access: CardAccess;
  downloader: Downloader;
  deflate?: Deflate;
}

const INITIAL: BuildState = {
  status: '', verdict: null, failures: [], error: null, busy: false,
};

export class BuildModel {
  readonly store = new Store<BuildState>({ ...INITIAL });

  constructor(
    private readonly layout: Layout,
    private readonly patches: Record<string, string>,
    private readonly deps: BuildDeps,
  ) {}

  /** What a fresh card contains. Recomputed rather than cached - it is a few hundred microseconds. */
  built(): BuiltCard {
    return buildCard(this.layout, this.patches);
  }

  canWriteInPlace(): boolean {
    return this.deps.access.hasDirectAccess();
  }

  async downloadZip(): Promise<void> {
    this.store.set({ ...INITIAL, busy: true, status: 'Packing...' });
    try {
      const b = this.built();
      const bytes = await makeZip(b.files, b.dirs, this.deps.deflate);
      this.deps.downloader.save(bytes, 'sk-card-starter.zip', ZIP_MIME);
      this.store.set({
        busy: false,
        status: `${b.files.length} files, ${b.dirs.length} folders, ${bytes.length} bytes`,
        verdict: {
          kind: 'good',
          title: 'Downloaded.',
          detail: 'Unpack it onto a FAT32-formatted card so the folders sit at the card\'s root - the '
            + 'card should contain SK, tapes, radio and the rest directly, not a folder containing them.',
        },
      });
    } catch (e) {
      this.store.set({ busy: false, status: '', error: (e as Error).message });
    }
  }

  /**
   * Top up rather than overwrite: pointing this at a card that already has content must not silently
   * replace a config the user tuned or a README they annotated.
   *
   * @returns false if the user dismissed the picker, which is not an error and must not look like one
   */
  async writeInPlace(): Promise<boolean> {
    this.store.set({ ...INITIAL, busy: true });
    try {
      const card = await this.deps.access.pickDirectory('readwrite');
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
          kind: failed.length ? 'mixed' : 'good',
          title: failed.length ? 'Finished with problems.' : 'Card is ready.',
          detail: skipped
            ? `${skipped} files were already there and were left untouched.`
            : 'Every folder, config and README is in place.',
        },
      });
      return true;
    } catch (e) {
      const err = e as Error;
      this.store.set({ busy: false, status: '' });
      if (err.name === 'AbortError') return false;
      this.store.set({ error: err.message });
      return false;
    }
  }
}
