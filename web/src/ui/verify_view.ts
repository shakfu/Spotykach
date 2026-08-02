// verify_view.ts - "why does my card not work?"
//
// The highest-value screen in the app, and the one that needs the least from the browser: it reads
// filenames and headers, so it works everywhere, with no audio decoding and no writable handle. It is
// also the screen that answers the question the device itself cannot - the hardware's only feedback is
// an LED, and every way of getting a card wrong fails silently.

import { VerifyModel } from '../app/verify_model.ts';
import type { Layout } from '../core/layout.ts';
import * as source from '../platform/cardsource.ts';
import { aside, clear, dropTarget, el, finding, humanBytes } from './dom.ts';
import type { ViewContext } from './context.ts';

/** Distinct folder layouts: every bank except the platform's own config entry. */
const engineBanks = (layout: Layout): number => layout.banks.filter((b) => b.kind !== 'config').length;

/** Distinct audio formats in play - the count that makes a card hard to hand-build. */
const audioFormats = (layout: Layout): number =>
  new Set(layout.banks.filter((b) => b.fmt.container !== 'text').map((b) => b.fmt.describe)).size;

export function mountVerify(root: HTMLElement, ctx: ViewContext): void {
  const model = new VerifyModel(ctx.layout);

  const results = el('div', { class: 'results' });
  const status = el('div', { class: 'status' });
  const drop = el('div', { class: 'dropzone' },
    el('p', {}, 'Drop the card folder here'),
    el('p', { class: 'muted' }, 'or pick it below. Nothing is uploaded - the check runs in this tab.'));

  const pickBtn = el('button', {
    class: 'primary',
    onclick: () => model.run(() => source.pickDirectory('read')),
  }, 'Choose card folder');

  // The <input webkitdirectory> path is the fallback that works in Safari and Firefox, where
  // showDirectoryPicker does not exist. Hidden behind a button so it looks like the other control.
  const fileInput = el('input', {
    type: 'file',
    webkitdirectory: '',
    multiple: true,
    class: 'hidden',
    onchange: (e: Event) => {
      const files = (e.target as HTMLInputElement).files;
      if (files?.length) void model.run(async () => source.fromFileList(files));
    },
  });
  const browseBtn = el('button', { onclick: () => fileInput.click() }, 'Browse for folder');

  dropTarget(drop, (dt) => model.run(() => source.fromDataTransfer(dt)));

  model.store.subscribe((s) => {
    clear(results);
    if (s.busy || !s.checked) {
      status.textContent = s.status;
      if (s.error) results.append(finding('error', '', s.error));
      return;
    }
    status.textContent = `${s.fileCount} files, ${humanBytes(s.totalBytes)}`
      + `${s.editable ? ' - this card can be edited in place' : ''}`;

    const { errors, warns, ok } = s.summary!;
    if (ok && !warns.length) {
      results.append(el('div', { class: 'verdict good' },
        el('strong', {}, 'No problems found.'),
        el('p', {}, 'Every file present is in a format the firmware accepts.')));
      return;
    }
    results.append(el('div', { class: `verdict ${errors.length ? 'bad' : 'mixed'}` },
      el('strong', {}, `${errors.length} error${errors.length === 1 ? '' : 's'}, `
        + `${warns.length} warning${warns.length === 1 ? '' : 's'}`),
      el('p', {}, errors.length
        ? 'Anything under WILL NOT WORK is silently ignored or misread by the device.'
        : 'Nothing is broken, but these are probably not what you meant.')));

    for (const [group, label, cls] of [
      [errors, 'WILL NOT WORK', 'error'] as const,
      [warns, 'Worth checking', 'warn'] as const,
    ]) {
      if (!group.length) continue;
      results.append(el('h3', {}, `${label} (${group.length})`));
      for (const f of group) results.append(finding(cls, f.path, f.problem, f.fix));
    }
  });

  const controls = el('div', { class: 'controls' }, pickBtn, browseBtn, fileInput);

  root.append(
    el('p', { class: 'lead' }, 'Checks a card and explains anything that will not work.'),
    controls,
    drop,
    status,
    results,
    aside('Why a bad card gives no error on the device',
      el('p', {},
        // Counts derived from the layout, not written down: the "eight folder layouts" figure in the
        // docs silently became nine the moment softcut was added, and prose does not have a test.
        `Engines read this card using ${engineBanks(ctx.layout)} folder layouts and `
        + `${audioFormats(ctx.layout)} incompatible audio formats, and the firmware converts nothing. A `
        + 'file in the wrong format is not rejected, it is read as raw bytes and plays as noise; a '
        + `filename over ${ctx.layout.scan.max_name} characters is skipped by the directory scan with `
        + 'no error shown. The hardware\'s only feedback is an LED, so every one of these fails '
        + 'silently. This finds all of it.')),
  );

  if (!source.hasFileSystemAccess()) {
    pickBtn.disabled = true;
    pickBtn.title = 'This browser has no File System Access API';
    controls.append(el('span', { class: 'muted note' },
      'In-place card access needs Chrome or Edge; dropping a folder works here.'));
  }
}
