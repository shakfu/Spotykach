// types.ts - the shapes card_layout.json declares, and the shapes this app derives from them.
//
// Nothing here is a rule. `scripts/card_layout.py` is the single source of truth for the layout, and
// `make web-data` exports it; this file only gives its JSON a name so the rest of the code can be
// checked against it. If a field appears here that the export does not produce, the types are lying -
// which is why `makeLayout` still validates the schema number at runtime rather than trusting the cast.

export type Encoding = 'f32' | 'int16';

export interface FormatSpec {
  container: 'wav' | 'raw' | 'text';
  encodings: Encoding[];
  /** null where the bank accepts either channel count (no bank does today, but the export allows it). */
  channels: number | null;
  /** null where the bank has no fixed rate - only `bard`, which is why Convert shows a rate control. */
  rate: number | null;
  note: string;
  describe: string;
}

export interface Bank {
  engine: string;
  kind: 'slots' | 'scanned' | 'config';
  scanned: boolean;
  dirs: string[];
  fmt: FormatSpec;
  slots: string[];
  max_files: number | null;
  max_seconds: number | null;
  sidecars: string[];
  source: string;
  blurb: string;
  extras: Record<string, string>;
  /** The convert target template, `''` for banks that take no audio. See `formatTarget`. */
  target: string;
  /** Every engine that reads this folder set, not just the one that owns it. */
  readers: string[];
}

export interface ScanRules {
  max_name: number;
  min_bytes: number;
  extensions: string[];
  skip_dot: boolean;
}

export interface RootReadme {
  bare: string;
  [variant: string]: string;
}

export interface LayoutData {
  schema: number;
  generated_by: string;
  scan: ScanRules;
  encodings: Record<Encoding, { bits: number; wav_format: number; label: string }>;
  banks: Bank[];
  all_dirs: string[];
  granular_tapes: string[];
  default_config: string;
  config_properties: Record<string, [number, number]>;
  source_extensions: string[];
  sidecar_names: string[];
  skip_dirs: string[];
  readmes: Record<string, string>;
  root_readme: RootReadme;
}

// --- the card, as this app sees it ----------------------------------------------------------------

/**
 * One file on a card, read lazily.
 *
 * Deliberately abstract: satisfiable from a File System Access handle, a dropped folder, a plain
 * `<input webkitdirectory>`, or a test fixture - and `verify` needs to know about none of them.
 */
export interface CardEntry {
  /** card-relative POSIX path, e.g. `tapes/tape_a_1.wav` */
  path: string;
  size: number;
  /** the file, or its first `max` bytes */
  read(max?: number): Promise<Uint8Array>;
}

export interface Card {
  files: CardEntry[];
  dirs: Set<string>;
  /**
   * The writable directory handle, where the browser gave us one. Opaque to core - only
   * `platform/cardsource` knows what it is - but its presence is what "this card can be edited in
   * place" means, so the models do need to see whether it is there.
   */
  handle: unknown | null;
}

export interface CardFile {
  path: string;
  bytes: Uint8Array;
}

export interface Finding {
  /** `error` = will not work; `warn` = works, but probably not what was meant. */
  level: 'error' | 'warn';
  path: string;
  problem: string;
  fix: string;
}

export interface WriteResult {
  written: string[];
  failed: Array<{ path: string; error: string }>;
}
