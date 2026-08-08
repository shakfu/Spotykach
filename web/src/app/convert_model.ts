// convert_model.ts - the convert tab's state and behaviour, decoder injected.
//
// Everything the old view did between "user clicked" and "pixels changed" is here, which is what makes
// the interesting questions testable: does switching to bard expose the rate control and default it to
// 24 kHz? does a decode failure leave the buttons in a sane state? Neither needs a browser now - the
// tests pass a fake decoder that returns a sine wave.

import { convertOne, targetSummary, type ConvertResult } from '../core/convert.ts';
import type { Layout } from '../core/layout.ts';
import type { AudioDecoder, CardAccess, Deflate, Downloader } from '../core/ports.ts';
import type { Bank } from '../core/types.ts';
import { makeZip, ZIP_MIME } from '../core/zip.ts';
import { Store } from './store.ts';

/** An input file, reduced to what conversion needs - so a test can supply one without a File. */
export interface InputFile {
  name: string;
  size: number;
  bytes(): Promise<ArrayBuffer>;
}

export interface ConvertState {
  engine: string;
  deck: string;
  bank: number;
  tape: string;
  slot: number;
  rate: number;
  files: InputFile[];
  results: ConvertResult[];
  status: string;
  error: string | null;
  busy: boolean;
}

export interface ConvertDeps {
  decoder: AudioDecoder;
  access: CardAccess;
  downloader: Downloader;
  deflate?: Deflate;
}

/** Which controls a bank's target template actually uses - the rest are hidden, not disabled. */
export interface FieldVisibility {
  deck: boolean;
  bank: boolean;
  tape: boolean;
  rate: boolean;
}

export class ConvertModel {
  readonly store: Store<ConvertState>;

  constructor(
    private readonly layout: Layout,
    private readonly deps: ConvertDeps,
  ) {
    const first = layout.audioBanks()[0];
    this.store = new Store<ConvertState>({
      engine: first ? first.engine : '',
      deck: 'a',
      bank: 0,
      tape: layout.granularTapes[0] ?? 'B',
      slot: 1,
      rate: 48000,
      files: [],
      results: [],
      status: '',
      error: null,
      busy: false,
    });
    this.setEngine(this.store.get().engine);
  }

  banks(): Bank[] {
    return this.layout.audioBanks();
  }

  bank(): Bank {
    return this.layout.bank(this.store.get().engine)!;
  }

  fields(): FieldVisibility {
    const t = this.bank().target;
    return {
      deck: t.includes('{deck}'),
      bank: t.includes('{bank}'),
      tape: t.includes('{tape}'),
      // Only bard has no fixed rate; everywhere else the firmware demands one exact value and offering
      // a control would only invite getting it wrong.
      rate: this.bank().fmt.rate == null,
    };
  }

  /** What the current settings will produce, in words. */
  summary(): string {
    const b = this.bank();
    return `Writes ${targetSummary(b, this.store.get().rate)}`
      + (b.max_seconds ? ` - trimmed to ${b.max_seconds} s, because this engine loads into RAM` : '')
      + (b.scanned ? `, looped up to ${this.layout.scan.min_bytes / 1024} KB if shorter` : '');
  }

  setEngine(engine: string): void {
    const bank = this.layout.bank(engine);
    if (!bank) return;
    // A bank with no fixed rate needs a default, and 24 kHz is the right one for speech - half the
    // bytes per hour, which for an audiobook shelf is the difference that matters.
    const rate = bank.fmt.rate ?? (bank.engine === 'bard' ? 24000 : 48000);
    this.store.set({ engine, rate, results: [], status: '' });
  }

  setField<K extends keyof ConvertState>(key: K, value: ConvertState[K]): void {
    this.store.set({ [key]: value } as unknown as Partial<ConvertState>);
  }

  addFiles(files: InputFile[]): void {
    const next = [...this.store.get().files, ...files];
    this.store.set({ files: next, results: [], status: this.describeQueue(next) });
  }

  removeFile(index: number): void {
    const next = this.store.get().files.filter((_, i) => i !== index);
    this.store.set({ files: next, results: [], status: this.describeQueue(next) });
  }

  private describeQueue(files: InputFile[]): string {
    return files.length
      ? `${files.length} file(s) -> ${this.store.get().engine}, starting at slot ${this.store.get().slot}`
      : '';
  }

  canConvert(): boolean {
    return this.store.get().files.length > 0 && !this.store.get().busy;
  }

  canSaveToCard(): boolean {
    return this.store.get().results.length > 0 && this.deps.access.hasDirectAccess();
  }

  async convert(): Promise<void> {
    const s = this.store.get();
    const bank = this.bank();
    const results: ConvertResult[] = [];
    this.store.set({ busy: true, results: [], error: null, status: 'Decoding...' });
    try {
      for (let i = 0; i < s.files.length; i++) {
        const f = s.files[i];
        this.store.set({ status: `Decoding ${f.name} (${i + 1}/${s.files.length})...` });
        results.push(await convertOne(this.layout, bank, this.deps.decoder,
          { name: f.name, data: await f.bytes() },
          { index: s.slot + i, deck: s.deck, bank: s.bank, tape: s.tape, rate: s.rate }));
      }
      this.store.set({ busy: false, results, status: `${results.length} file(s) converted` });
    } catch (e) {
      this.store.set({ busy: false, status: '', results: [], error: (e as Error).message });
    }
  }

  async downloadZip(): Promise<void> {
    const results = this.store.get().results;
    const bytes = await makeZip(
      results.map((r) => ({ path: r.path, bytes: r.bytes })), [], this.deps.deflate);
    this.deps.downloader.save(bytes, `sk-${this.store.get().engine}-files.zip`, ZIP_MIME);
  }

  /** @returns false if the user dismissed the picker */
  async saveToCard(): Promise<boolean> {
    try {
      const card = await this.deps.access.pickDirectory('readwrite');
      const files = this.store.get().results.map((r) => ({ path: r.path, bytes: r.bytes }));
      const { written, failed } = await this.deps.access.writeInto(card.handle, files);
      this.store.set({
        status: `${written.length} written to the card, ${failed.length} failed`,
        error: failed.length ? failed.map((f) => `${f.path}: ${f.error}`).join('\n') : null,
      });
      return true;
    } catch (e) {
      const err = e as Error;
      if (err.name === 'AbortError') return false;
      this.store.set({ error: err.message });
      return false;
    }
  }
}
