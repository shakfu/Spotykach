// build_view.ts - make a correct card, either in place or as a zip.
//
// The zip is the primary path, not the consolation prize: File System Access is Chromium-only, and for
// a music-hardware audience skewing Mac, "does not work in Safari" is a real cost. Download-and-unpack
// works everywhere and produces the same bytes.

import { BuildModel } from '../app/build_model.ts';
import { folderLabel } from '../core/layout.ts';
import { cardAccess } from '../platform/cardsource.ts';
import { deflateRaw, downloader } from '../platform/download.ts';
import { clear, el, finding } from './dom.ts';
import { fill, mountPoint, slot } from './slots.ts';
import type { ViewContext } from './context.ts';

export function mountBuild(root: HTMLElement, ctx: ViewContext): void {
  const model = new BuildModel(ctx.layout, ctx.patches, {
    access: cardAccess, downloader, deflate: deflateRaw,
  });

  const status = el('div', { class: 'status' });
  const out = el('div', { class: 'results' });

  // Deliberately NOT `primary`: the zip is the path that works in every browser, and this one is the
  // Chromium-only enhancement. Two filled buttons side by side means neither is the main action - and
  // this is the one that is disabled for most visitors, so filled-but-faded reads as the app being
  // broken rather than as a capability their browser lacks.
  const inPlace = el('button', { onclick: () => model.writeInPlace() }, 'Write onto a card');
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

  // The prose is in index.html; what is left here is the controls, the live status and the results.
  mountPoint(root).append(
    el('div', { class: 'controls' },
      el('button', { class: 'primary', onclick: () => model.downloadZip() },
        'Download a starter card (.zip)'),
      inPlace),
    status,
    out,
  );

  // Counts and the folder table are derived from the layout, so they are filled rather than written.
  fill(root, 'files', String(b.files.length));
  fill(root, 'dirs', String(b.dirs.length));
  slot(root, 'folders').append(folders);
}
