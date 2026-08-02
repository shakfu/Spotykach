// build.ts - generate a complete, correct card.
//
// Almost nothing here is logic: every byte of text this emits comes out of card_layout.json, which
// carries the rendered per-folder READMEs, the root README and the default config alongside the table
// itself. That is on purpose - the wording is a pure function of the layout, so shipping it as data
// means the browser writes files byte-identical to `sk_card.py init` without owning a line of it.
//
// The one thing NOT generated here is demo audio. scripts/sk_card.py synthesizes tones, sweeps, noise
// beds and formant babble so a fresh card makes sound on every engine, but that same card is already a
// checksummed release artifact (`sk-card-<version>.zip`, built by `make sdcard` and shipped by
// `make gh-release`). Regenerating it in JS would be a second synthesis implementation whose output
// nobody could compare against the published checksum, so the app links the artifact instead.

import type { Layout } from './layout.ts';
import type { CardFile } from './types.ts';

const ascii = (s: string): Uint8Array => new TextEncoder().encode(s);

export interface BuiltCard {
  dirs: string[];
  files: CardFile[];
}

/**
 * The full file list for a fresh card: folder READMEs, the platform config, per-bank sidecars
 * (radio/rate.txt, bard/BARD.CFG), the bundled chuck/csound example patches, and the root README.
 *
 * Directories are returned separately because an empty folder is meaningful here - the device scans
 * `radio/0` .. `radio/15` whether or not the user has filled them, and a card missing those folders
 * looks broken in a way `verify` reports.
 *
 * @param patches bundled example patches, `{"chuck/0.ck": "...", ...}`
 */
export function buildCard(layout: Layout, patches: Record<string, string> = {}): BuiltCard {
  const files: CardFile[] = [];
  const add = (path: string, text: string) => files.push({ path, bytes: ascii(text) });

  for (const bank of layout.banks) {
    for (const d of bank.dirs) add(`${d}/README.TXT`, layout.readmes[d]);
  }
  add('SK/config.txt', layout.defaultConfig);
  for (const bank of layout.banks) {
    for (const [path, content] of Object.entries(bank.extras)) add(path, content);
  }
  // The example patches are exactly what these two banks want; they are bundled as data (patches.json)
  // because a static page cannot read examples/ out of the repo the way `sk_card.py init` does.
  for (const [path, text] of Object.entries(patches)) add(path, text);

  add('README.TXT', layout.rootReadme.bare);

  files.sort((a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0));
  return { dirs: [...layout.allDirs], files };
}

/**
 * Only the files a card is MISSING, given what is already there. Used when writing in place, so
 * pointing the builder at a card with content on it tops up the skeleton instead of overwriting a
 * README the user edited or a config they tuned.
 */
export function missingFrom(built: BuiltCard, existingPaths: Iterable<string>): BuiltCard {
  const have = new Set([...existingPaths].map((p) => p.toUpperCase()));
  return {
    dirs: built.dirs,
    files: built.files.filter((f) => !have.has(f.path.toUpperCase())),
  };
}
