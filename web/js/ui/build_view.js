// build_view.js - make a correct card, either in place or as a zip.
//
// The zip is the primary path, not the consolation prize: File System Access is Chromium-only, and for
// a music-hardware audience skewing Mac, "does not work in Safari" is a real cost. Download-and-unpack
// works everywhere and produces the same bytes.

import { el, clear, humanBytes, showError } from './dom.js';
import { buildCard, missingFrom } from '../build.js';
import { makeZip, saveBlob } from '../zip.js';
import * as source from '../cardsource.js';

/**
 * A readable label for a bank's folders, matching how docs/sd-card.md writes them.
 *
 * `SK/B .. SK/Y` reads as a range of two folders rather than a set of six, and it makes granular's
 * relationship to the `SK` platform row - at the other end of the table - impossible to see. Collapsing
 * to the shared parent says it in one glance: `SK/{B,G,P,R,T,Y}`, `radio/{0..15}`.
 */
export function folderLabel(dirs) {
  if (dirs.length === 1) return dirs[0];
  const cut = dirs[0].lastIndexOf('/');
  const parent = cut < 0 ? '' : dirs[0].slice(0, cut);
  const leaves = dirs.map((d) => d.slice(d.lastIndexOf('/') + 1));
  // Bail out if they do not actually share one parent - better a clumsy label than a wrong one.
  if (!dirs.every((d) => d.slice(0, d.lastIndexOf('/')) === parent)) return dirs.join(', ');
  const nums = leaves.map(Number);
  const contiguous = nums.every(Number.isInteger)
    && nums.every((n, i) => i === 0 || n === nums[i - 1] + 1);
  const set = contiguous ? `${nums[0]}..${nums[nums.length - 1]}` : leaves.join(',');
  return parent ? `${parent}/{${set}}` : `{${set}}`;
}

export function mountBuild(root, ctx) {
  const status = el('div', { class: 'status' });
  const out = el('div', { class: 'results' });

  const built = () => buildCard(ctx.layout, ctx.patches);

  async function downloadZip() {
    clear(out);
    status.textContent = 'Packing...';
    try {
      const b = built();
      const blob = await makeZip(b.files, b.dirs);
      saveBlob(blob, 'sk-card-starter.zip');
      status.textContent = `${b.files.length} files, ${b.dirs.length} folders, ${humanBytes(blob.size)}`;
      out.append(el('div', { class: 'verdict good' },
        el('strong', {}, 'Downloaded.'),
        el('p', {}, 'Unpack it onto a FAT32-formatted card so the folders sit at the card\'s root - '
          + 'the card should contain SK, tapes, radio and the rest directly, not a folder containing them.')));
    } catch (e) {
      status.textContent = '';
      showError(out, e);
    }
  }

  async function writeInPlace() {
    clear(out);
    try {
      const card = await source.pickDirectory('readwrite');
      const b = built();
      // Top up rather than overwrite: pointing this at a card that already has content must not
      // silently replace a config the user tuned or a README they annotated.
      const todo = missingFrom(b, card.files.map((f) => f.path));
      status.textContent = `Writing ${todo.files.length} missing files...`;
      const { written, failed } = await source.writeInto(card.handle, todo.files, todo.dirs);
      const skipped = b.files.length - todo.files.length;
      status.textContent = `${written.length} written, ${skipped} already present, ${failed.length} failed`;
      out.append(el('div', { class: `verdict ${failed.length ? 'mixed' : 'good'}` },
        el('strong', {}, failed.length ? 'Finished with problems.' : 'Card is ready.'),
        el('p', {}, skipped
          ? `${skipped} files were already there and were left untouched.`
          : 'Every folder, config and README is in place.')));
      for (const f of failed) {
        out.append(el('div', { class: 'finding error' },
          el('div', { class: 'path' }, f.path), el('div', { class: 'problem' }, f.error)));
      }
    } catch (e) {
      if (e.name === 'AbortError') return;
      status.textContent = '';
      showError(out, e);
    }
  }

  const inPlace = el('button', { class: 'primary', onclick: writeInPlace }, 'Write onto a card');
  if (!source.hasFileSystemAccess()) {
    inPlace.disabled = true;
    inPlace.title = 'This browser has no File System Access API - use the zip';
  }

  const b = built();
  const folders = el('table', { class: 'layout' },
    el('thead', {}, el('tr', {}, el('th', {}, 'Folder'), el('th', {}, 'Engine'), el('th', {}, 'Format'))),
    el('tbody', {}, ctx.layout.banks.map((bank) => el('tr', {},
      el('td', { class: 'mono' }, folderLabel(bank.dirs)),
      // Readers, not the owning engine alone: SK/{B,G,P,R,T,Y} is the platform's shared tape store,
      // so listing only "granular" there makes graincloud's loops look like they belong nowhere.
      el('td', {}, bank.readers.join(', ')),
      el('td', { class: 'muted' }, bank.fmt.describe)))));

  root.append(
    el('p', { class: 'lead' },
      'Start here with a new card. This builds a complete, valid, minimal card: every folder the '
      + 'firmware looks for, a README in each one restating that folder\'s rules, the default '
      + 'SK/config.txt, radio/rate.txt, bard/BARD.CFG, and the example chuck and csound patches. It '
      + 'is byte for byte the card `sk_card.py init --no-demo` builds, and it passes the Verify tab '
      + 'with nothing to report.'),
    el('ol', { class: 'steps' },
      el('li', {}, 'Format the card as FAT32 (up to 32 GB). Browsers cannot do this part.'),
      el('li', {}, 'Download the zip below and unpack it so the folders sit at the card\'s root - '
        + 'or, in Chrome and Edge, write straight onto the card.'),
      el('li', {}, 'Add your own audio on the Convert tab. It is converted to the format each engine '
        + 'actually reads.'),
      el('li', {}, 'If anything ever misbehaves, point the ',
        el('a', { href: '#verify' }, 'Verify tab'),
        ' at the card - it explains every rule the card breaks, and how to fix it.')),
    el('div', { class: 'controls' },
      el('button', { class: 'primary', onclick: downloadZip }, 'Download a starter card (.zip)'),
      inPlace),
    status,
    el('div', { class: 'callout' },
      el('strong', {}, 'Want demo audio too? '),
      'The released ',
      el('code', {}, 'sk-card-<version>.zip'),
      ' is a complete card with synthesized audio for every engine, and it is checksummed. This page '
      + 'builds the skeleton only rather than regenerating that content, so what you download from the '
      + 'release is what everyone else has. ',
      el('a', {
        href: 'https://github.com/shakfu/sk-engines/releases/latest',
        target: '_blank',
        rel: 'noreferrer',
      }, 'Get it from the latest release'),
      '.'),
    out,
    el('h3', {}, `What it creates (${b.files.length} files, ${b.dirs.length} folders)`),
    folders,
  );
}
