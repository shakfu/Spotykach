// build_view.ts - make a correct card, either in place or as a zip.
//
// The zip is the primary path, not the consolation prize: File System Access is Chromium-only, and for
// a music-hardware audience skewing Mac, "does not work in Safari" is a real cost. Download-and-unpack
// works everywhere and produces the same bytes.

import { BuildModel } from '../app/build_model.ts';
import { folderLabel } from '../core/layout.ts';
import { cardAccess } from '../platform/cardsource.ts';
import { deflateRaw, downloader } from '../platform/download.ts';
import { aside, clear, el, finding } from './dom.ts';
import type { ViewContext } from './context.ts';

export function mountBuild(root: HTMLElement, ctx: ViewContext): void {
  const model = new BuildModel(ctx.layout, ctx.patches, {
    access: cardAccess, downloader, deflate: deflateRaw,
  });

  const status = el('div', { class: 'status' });
  const out = el('div', { class: 'results' });

  const inPlace = el('button', { class: 'primary', onclick: () => model.writeInPlace() },
    'Write onto a card');
  if (!model.canWriteInPlace()) {
    inPlace.disabled = true;
    inPlace.title = 'This browser has no File System Access API - use the zip';
  }

  model.store.subscribe((s) => {
    status.textContent = s.status;
    clear(out);
    if (s.error) {
      out.append(finding('error', '', s.error));
      return;
    }
    if (s.verdict) {
      out.append(el('div', { class: `verdict ${s.verdict.kind}` },
        el('strong', {}, s.verdict.title),
        el('p', {}, s.verdict.detail)));
    }
    for (const f of s.failures) out.append(finding('error', f.path, f.error));
  });

  const b = model.built();
  const folders = el('table', { class: 'layout' },
    el('thead', {}, el('tr', {}, el('th', {}, 'Folder'), el('th', {}, 'Engine'), el('th', {}, 'Format'))),
    el('tbody', {}, ctx.layout.banks.map((bank) => el('tr', {},
      el('td', { class: 'mono' }, folderLabel(bank.dirs)),
      // Readers, not the owning engine alone: SK/{B,G,P,R,T,Y} is the platform's shared tape store,
      // so listing only "granular" there makes graincloud's loops look like they belong nowhere.
      el('td', {}, bank.readers.join(', ')),
      el('td', { class: 'muted' }, bank.fmt.describe)))));

  root.append(
    el('p', { class: 'lead' }, 'Makes an empty card the firmware can read. Format it FAT32 first.'),
    el('div', { class: 'controls' },
      el('button', { class: 'primary', onclick: () => model.downloadZip() },
        'Download a starter card (.zip)'),
      inPlace),
    status,
    el('p', { class: 'muted note' },
      'Unpack it so the folders sit at the card\'s root. Then add audio on ',
      el('a', { href: '#convert' }, 'Convert'),
      ', and if anything misbehaves later, point ',
      el('a', { href: '#verify' }, 'Verify'),
      ' at the card.'),
    out,
    aside(`What it creates - ${b.files.length} files, ${b.dirs.length} folders`,
      el('p', {}, 'Every folder the firmware looks for, a README in each one restating that folder\'s '
        + 'rules, the default SK/config.txt, radio/rate.txt, bard/BARD.CFG, and the example chuck and '
        + 'csound patches. Byte for byte the card ',
      el('code', {}, 'sk_card.py init --no-demo'),
      ' builds, and it passes Verify with nothing to report.'),
      folders),
    aside('Want demo audio too?',
      el('p', {},
        'The released ',
        el('code', {}, 'sk-card-<version>.zip'),
        ' is a complete card with synthesized audio for every engine, and it is checksummed. This page '
        + 'builds the skeleton only rather than regenerating that content, so what you download from '
        + 'the release is what everyone else has. ',
        el('a', {
          href: 'https://github.com/shakfu/sk-engines/releases/latest',
          target: '_blank',
          rel: 'noreferrer',
        }, 'Get it from the latest release'),
        '.')),
  );
}
