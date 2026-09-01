// verify.ts - "why does my card not work?", in the browser.
//
// A port of `verify_card` and its helpers in scripts/sk_card.py. The RULES are not ported - they come
// from card_layout.json; only the walk is code, exactly as docs/dev/web-frontend.md scopes it.
//
// Parity with the Python is the whole point, so this mirrors it including the parts that are arguably
// too strict (see the note on radio .wav files in checkAudioFormat). If a check should change, change
// scripts/sk_card.py first and follow it here - the two are meant to give a user the same answer, and
// a well-meant local improvement is just a second dialect of the rules.
//
// The input is deliberately abstract: entries with a path, a size, and a lazy reader. That is
// satisfiable from a File System Access directory handle, a drag-and-dropped folder, or a plain
// <input webkitdirectory>, and by fixtures in the tests - none of which this file needs to know about.

import type { Layout } from './layout.ts';
import type { Bank, Card, CardEntry, Finding } from './types.ts';
import { parseWav, WavError, WavNeedMore, type WavInfo } from './wav.ts';

/**
 * How much of a file to pull before parsing its header. A WAV header past 64 KB is pathological, and
 * the re-read below covers the case anyway; this keeps a multi-gigabyte card off the heap.
 */
const HEADER_PREFIX = 64 * 1024;

const finding = (level: Finding['level'], path: string, problem: string, fix = ''): Finding =>
  ({ level, path, problem, fix });

const suffixOf = (name: string): string => {
  const i = name.lastIndexOf('.');
  return i <= 0 ? '' : name.slice(i); // i === 0 is a leading-dot name, which has no suffix
};
const stemOf = (name: string): string => {
  const i = name.lastIndexOf('.');
  return i <= 0 ? name : name.slice(0, i);
};
const dirOf = (path: string): string => {
  const i = path.lastIndexOf('/');
  return i < 0 ? '' : path.slice(0, i);
};
const baseOf = (path: string): string => path.slice(path.lastIndexOf('/') + 1);

/**
 * An 8.3-safe name derived from the original, for the rename hint.
 *
 * Naive truncation produces junk like `THE .WAV` (trailing space, no information), so strip to
 * alphanumerics first and fall back to a generic stem when nothing usable survives.
 */
function shortNameSuggestion(name: string): string {
  const stem = [...stemOf(name)].filter((c) => /[0-9A-Za-z]/.test(c)).join('').toUpperCase().slice(0, 8);
  return `${stem || 'TRACK01'}${suffixOf(name).toUpperCase()}`;
}

/**
 * Rules from StreamDeck::scan_bank. Each of these makes a perfectly-encoded file INVISIBLE, and none
 * produces any feedback on the device - which is why they are checked first and hard.
 *
 * @returns whether the file is visible enough to be worth format-checking
 */
function checkScanVisibility(layout: Layout, entry: CardEntry, bank: Bank, out: Finding[]): boolean {
  const name = baseOf(entry.path);
  const rel = entry.path;
  let ok = true;
  if (name.startsWith('.')) {
    out.push(finding('warn', rel, 'name starts with a dot, so the scan skips it',
      'This is usually a macOS metadata stub (._NAME or .DS_Store). Delete it; on macOS use '
      + '`dot_clean` on the card before ejecting.'));
    return false;
  }
  const max = layout.scan.max_name;
  if (name.length > max) {
    ok = false;
    out.push(finding('error', rel,
      `filename is ${name.length} characters; the scan skips anything over ${max}, so this file is `
      + 'INVISIBLE to the device',
      `Rename to ${max} characters or fewer including the extension (e.g. ${shortNameSuggestion(name)}). `
      + 'For a whole library, scripts/prepare_audiobooks.py does the renaming and records the real '
      + 'titles in BOOKS.TXT.'));
  }
  const ext = suffixOf(name);
  if (!name.includes('.') || !layout.scan.extensions.includes(ext.toLowerCase().replace(/^\./, ''))) {
    ok = false;
    if (layout.isSourceExt(ext)) {
      out.push(finding('error', rel,
        `${ext} is a compressed/unsupported source format - the firmware has no decoder, and the scan `
        + 'only indexes .raw/.wav',
        `Convert it on the Convert tab, or: sk_card.py convert --engine ${bank.engine} CARD ${name}`));
    } else {
      out.push(finding('error', rel, `extension ${ext || '(none)'} is not indexed by the scan`,
        `Use .raw or .wav (${bank.accepts.describe}).`));
    }
  }
  const floor = layout.scan.min_bytes;
  if (entry.size < floor) {
    ok = false;
    out.push(finding('error', rel,
      `file is ${(entry.size / 1024).toFixed(1)} KB; the scan skips anything under ${floor / 1024} KB, `
      + 'so this file is INVISIBLE to the device',
      'Make the clip longer (the floor exists to drop macOS metadata stubs, and catches genuinely '
      + 'short clips too).'));
  }
  return ok;
}

/** Read enough of a file to parse its header, growing to the whole file if the walk needs it. */
async function parseHeader(entry: CardEntry): Promise<WavInfo> {
  const prefix = await entry.read(Math.min(entry.size, HEADER_PREFIX));
  try {
    return parseWav(prefix, entry.size);
  } catch (e) {
    if (!(e instanceof WavNeedMore)) throw e;
    return parseWav(await entry.read(), entry.size);
  }
}

/**
 * Does the file's actual encoding match what this bank's engine will read it as?
 *
 * Note one inherited strictness, deliberately kept: a `.wav` dropped in a radio folder is checked
 * against the headerless RAW spec's fixed 48 kHz, even though the scan would read the rate out of its
 * header and play it correctly. That is what scripts/sk_card.py does, and the two front-ends
 * disagreeing would be worse than either being strict.
 */
async function checkAudioFormat(
  layout: Layout, entry: CardEntry, bank: Bank, out: Finding[],
): Promise<void> {
  const rel = entry.path;
  const acc = bank.accepts;
  const ext = suffixOf(baseOf(rel)).toLowerCase();
  if (acc.containers.includes('raw') && ext === '.raw') {
    if (entry.size % 2) {
      out.push(finding('warn', rel, 'odd byte count for a 16-bit format (last frame is partial)',
        'Harmless - the firmware floors to a whole frame - but usually means the file was truncated '
        + 'or is not actually int16.'));
    }
    return; // headerless: nothing else is checkable without guessing
  }

  let info: WavInfo;
  try {
    info = await parseHeader(entry);
  } catch (e) {
    if (!(e instanceof WavError)) throw e;
    out.push(finding('error', rel, `the firmware's WAV parser would reject this file: ${e.message}`,
      `Re-encode it: sk_card.py convert --engine ${bank.engine} CARD ${baseOf(rel)}`));
    return;
  }

  const problems: string[] = [];
  if (!info.encoding || !acc.encodings.includes(info.encoding)) {
    problems.push(`the firmware cannot decode ${info.describe().split(',')[0]}`);
  }
  if (info.channels > acc.max_channels) {
    problems.push(`${info.channels} channels is past the ${acc.max_channels}-channel downmix bound`);
  }
  if (acc.rate != null && info.rate !== acc.rate) {
    problems.push(`${info.rate} Hz, and nothing on this path resamples`);
  } else if (info.rate < layout.data.rate_bounds.min || info.rate > layout.data.rate_bounds.max) {
    problems.push(`${info.rate} Hz is outside the ${layout.data.rate_bounds.min}..`
      + `${layout.data.rate_bounds.max} Hz the resampler takes`);
  }
  if (problems.length) {
    out.push(finding('error', rel,
      `this will not load (${problems.join('; ')})`,
      `Accepts: ${acc.describe}. Fix it on the Convert tab, or: sk_card.py convert --engine `
      + `${bank.engine} CARD ${baseOf(rel)}`));
    return;
  }

  if (bank.max_seconds && info.seconds > bank.max_seconds * 1.02) {
    out.push(finding('warn', rel,
      `${info.seconds.toFixed(0)} s exceeds the ~${bank.max_seconds.toFixed(0)} s this engine holds in RAM`,
      'It will load truncated. Trim it, or use the tape engine, which streams.'));
  }
}

/** Slot banks open exact filenames. A near-miss is never opened and the slot reads as empty. */
function checkSlotName(layout: Layout, entry: CardEntry, bank: Bank, out: Finding[]): boolean {
  const rel = entry.path;
  const name = baseOf(rel);
  const names = new Map(bank.slots.map((s) => [s.toLowerCase(), s]));
  if (!names.has(name.toLowerCase())) {
    const ext = suffixOf(name);
    if (layout.isSourceExt(ext)) {
      // The commonest newcomer mistake: copy the source file across and expect the device to cope.
      // Say what it actually needs rather than listing slot names at them.
      out.push(finding('error', rel,
        `${ext} is a compressed/unsupported source format - the firmware has no decoder and never `
        + 'opens this file',
        `Convert it on the Convert tab, or: sk_card.py convert --engine ${bank.engine} CARD ${name}`));
    } else {
      out.push(finding('warn', rel, 'not one of this engine\'s slot filenames, so it is never opened',
        `Expected one of: ${bank.slots.slice(0, 6).join(', ')}${bank.slots.length > 6 ? ' ...' : ''}`));
    }
    return false;
  }
  const canonical = names.get(name.toLowerCase())!;
  const isUpper = canonical !== canonical.toLowerCase() && canonical === canonical.toUpperCase();
  if (name !== canonical && isUpper) {
    out.push(finding('warn', rel, `name is ${name}, documented as ${canonical}`,
      'FAT is case-insensitive so this generally still opens, but match the documented case to be safe.'));
  }
  return true;
}

/**
 * SK/config.txt is a property name on one line and its value on the NEXT line - not key=value, which
 * is the natural thing to write by hand and silently parses as nothing.
 */
async function checkConfig(layout: Layout, entry: CardEntry, out: Finding[]): Promise<void> {
  const rel = 'SK/config.txt';
  const text = new TextDecoder('utf-8', { fatal: false }).decode(await entry.read());
  const lines = text.split(/\r\n|\r|\n/).map((l) => l.trim()).filter(Boolean);
  if (lines.some((l) => l.includes('='))) {
    out.push(finding('error', rel,
      'looks like `key=value`, but the parser expects the property name and its value on separate lines',
      'Write:\n    pre_load\n    1'));
    return;
  }
  const known = layout.configProperties;
  for (let i = 0; i + 1 < lines.length; i += 2) {
    const key = lines[i];
    const val = lines[i + 1];
    if (!(key in known)) {
      out.push(finding('warn', rel, `unknown property '${key}'`, `Known: ${Object.keys(known).join(', ')}`));
      continue;
    }
    const [lo, hi] = known[key];
    const digits = val.replace(/^-+/, '');
    const numeric = digits.length > 0 && /^[0-9]+$/.test(digits);
    if (!numeric || !(Number(val) >= lo && Number(val) <= hi)) {
      out.push(finding('error', rel, `${key} = '${val}' is outside ${lo}..${hi}`,
        `Set a value in ${lo}..${hi}.`));
    }
  }
  if (lines.length % 2) {
    out.push(finding('warn', rel, 'odd number of lines - the last property has no value',
      'Every property name needs a value on the following line.'));
  }
}

/** Walk a card and report everything that will not behave as the user expects. */
export async function verifyCard(
  layout: Layout, card: { files: CardEntry[]; dirs?: Iterable<string> } | Card,
): Promise<Finding[]> {
  const out: Finding[] = [];
  // Prune filesystem bookkeeping directories at any depth, as the CLI's os.walk filter does.
  const files = card.files.filter(
    (f) => !f.path.split('/').slice(0, -1).some((c) => layout.isSkippedDir(c)),
  );

  // Directories: whatever the caller reported, plus every parent implied by a file. The distinction
  // matters for the "folder exists but is empty" warning at the end, which is the one check that needs
  // a directory the walk found no files in.
  const dirs = new Set<string>(card.dirs ?? []);
  for (const f of files) {
    let d = dirOf(f.path);
    while (d) {
      dirs.add(d);
      d = dirOf(d);
    }
  }

  const present = layout.allDirs.filter((d) => dirs.has(d));
  if (!present.length) {
    out.push(finding('error', '.', 'no recognised engine folders found here',
      'Is this the card\'s root? Build a fresh one on the Build tab, or: sk_card.py init CARD'));
    return out;
  }

  // Group by directory so the per-folder file cap can be counted, and so findings come out in a
  // stable, readable order rather than filesystem-enumeration order.
  const byDir = new Map<string, CardEntry[]>();
  for (const f of files) {
    const d = dirOf(f.path);
    let list = byDir.get(d);
    if (!list) byDir.set(d, (list = []));
    list.push(f);
  }

  const seenBanks = new Set<string>();
  let configEntry: CardEntry | null = null;
  for (const dir of [...byDir.keys()].sort()) {
    const bank = dir ? layout.bankForPath(dir) : null;
    let counted = 0;
    for (const entry of byDir.get(dir)!.sort((a, b) => (a.path < b.path ? -1 : 1))) {
      const name = baseOf(entry.path);
      if (entry.path === 'SK/config.txt') configEntry = entry;
      if (!bank) {
        if (!entry.path.startsWith('.') && dir === '' && name.toUpperCase() !== 'README.TXT') {
          out.push(finding('warn', entry.path, 'file in the card root belongs to no engine',
            'Harmless, but the device never reads it.'));
        }
        continue;
      }
      seenBanks.add(bank.engine);
      if (layout.isSidecar(name)) continue;
      if (bank.fmt.container === 'text') {
        const slots = new Set(bank.slots.map((s) => s.toLowerCase()));
        if (bank.slots.length && !slots.has(name.toLowerCase())) {
          out.push(finding('warn', entry.path, 'not a slot the engine loads',
            `Expected ${bank.slots.join(', ')}`));
        }
        continue;
      }
      if (bank.scanned) {
        if (name.toUpperCase().endsWith('.TXT')) continue; // bard bookmark sidecars live beside the books
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
      out.push(finding('warn', dir,
        `${counted} playable files but only the first ${bank.max_files} (alphabetically) are indexed`,
        `Move the rest to another ${bank.engine} folder.`));
    }
  }

  if (configEntry) await checkConfig(layout, configEntry, out);

  for (const bank of layout.banks) {
    if (bank.fmt.container === 'text' || seenBanks.has(bank.engine)) continue;
    if (bank.dirs.some((d) => dirs.has(d))) {
      out.push(finding('warn', bank.dirs[0], `no files for the ${bank.engine} engine`,
        `${bank.blurb.split('.')[0]}.`));
    }
  }
  return out;
}

export interface Summary {
  errors: Finding[];
  warns: Finding[];
  ok: boolean;
}

/** Split findings the way the CLI prints them: errors first, then warnings. */
export function summarize(findings: Finding[]): Summary {
  const errors = findings.filter((f) => f.level === 'error');
  const warns = findings.filter((f) => f.level === 'warn');
  return { errors, warns, ok: errors.length === 0 };
}
