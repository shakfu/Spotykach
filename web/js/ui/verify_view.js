// verify_view.js - "why does my card not work?"
//
// The highest-value screen in the app, and the one that needs the least from the browser: it reads
// filenames and headers, so it works everywhere, with no audio decoding and no writable handle. It is
// also the screen that answers the question the device itself cannot - the hardware's only feedback is
// an LED, and every way of getting a card wrong fails silently.

import { el, $, clear, humanBytes, dropTarget, showError } from './dom.js';
import { verifyCard, summarize } from '../verify.js';
import * as source from '../cardsource.js';

/** Distinct folder layouts: every bank except the platform's own config entry. */
const engineBanks = (layout) => layout.banks.filter((b) => b.kind !== 'config').length;

/** Distinct audio formats in play - the count that makes a card hard to hand-build. */
const audioFormats = (layout) =>
  new Set(layout.banks.filter((b) => b.fmt.container !== 'text').map((b) => b.fmt.describe)).size;

export function mountVerify(root, ctx) {
  const results = el('div', { class: 'results' });
  const status = el('div', { class: 'status' });
  const drop = el('div', { class: 'dropzone' },
    el('p', {}, 'Drop the card folder here'),
    el('p', { class: 'muted' }, 'or pick it below. Nothing is uploaded - the check runs in this tab.'));

  const pickBtn = el('button', {
    class: 'primary',
    onclick: () => run(() => source.pickDirectory('read')),
  }, 'Choose card folder');

  // The <input webkitdirectory> path is the fallback that works in Safari and Firefox, where
  // showDirectoryPicker does not exist. Hidden behind a button so it looks like the other control.
  const fileInput = el('input', {
    type: 'file',
    webkitdirectory: '',
    multiple: true,
    class: 'hidden',
    onchange: (e) => e.target.files.length && run(() => source.fromFileList(e.target.files)),
  });
  const browseBtn = el('button', { onclick: () => fileInput.click() }, 'Browse for folder');

  dropTarget(drop, (dt) => run(() => source.fromDataTransfer(dt)));

  async function run(getCard) {
    clear(results);
    status.textContent = 'Reading the card...';
    try {
      const card = await getCard();
      ctx.setCard(card);
      status.textContent = `Checking ${card.files.length} files...`;
      const findings = await verifyCard(ctx.layout, card);
      render(card, findings);
    } catch (e) {
      status.textContent = '';
      if (e.name === 'AbortError') return; // the user dismissed the picker
      showError(results, e);
    }
  }

  function render(card, findings) {
    const { errors, warns, ok } = summarize(findings);
    const bytes = card.files.reduce((n, f) => n + f.size, 0);
    status.textContent = `${card.files.length} files, ${humanBytes(bytes)}`
      + `${card.handle ? ' - this card can be edited in place' : ''}`;

    clear(results);
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

    for (const [group, label, cls] of [[errors, 'WILL NOT WORK', 'error'], [warns, 'Worth checking', 'warn']]) {
      if (!group.length) continue;
      results.append(el('h3', {}, `${label} (${group.length})`));
      for (const f of group) {
        results.append(el('div', { class: `finding ${cls}` },
          el('div', { class: 'path' }, f.path),
          el('div', { class: 'problem' }, f.problem),
          f.fix && el('div', { class: 'fix' }, f.fix)));
      }
    }
  }

  root.append(
    el('p', { class: 'lead' },
      // Counts derived from the layout, not written down: the "eight folder layouts" figure in the docs
      // silently became nine the moment softcut was added, and prose does not have a test.
      `Engines read this card using ${engineBanks(ctx.layout)} folder layouts and `
      + `${audioFormats(ctx.layout)} incompatible audio formats. The firmware converts nothing: a file `
      + 'in the wrong format is not rejected, it is read as raw bytes and plays as noise, and a '
      + `filename over ${ctx.layout.scan.max_name} characters is skipped by the directory scan with no `
      + 'error shown. This finds all of it.'),
    el('div', { class: 'controls' }, pickBtn, browseBtn, fileInput),
    drop,
    status,
    results,
  );

  if (!source.hasFileSystemAccess()) {
    pickBtn.disabled = true;
    pickBtn.title = 'This browser has no File System Access API';
    $('.controls', root).append(el('span', { class: 'muted note' },
      'In-place card access needs Chrome or Edge; dropping a folder works here.'));
  }
}
