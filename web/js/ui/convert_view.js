// convert_view.js - drop audio in, get files the firmware can actually read.
//
// The screen that justifies the whole app. On the desktop this needs a decoder the user probably does
// not have: scripts/sk_card.py probes cysox, then ffmpeg, then sox, and the per-file `find_format`
// check exists because libsox's format support is a build-time property and mp3 is commonly missing.
// Here it is decodeAudioData, which every browser has, for mp3/flac/wav/ogg/m4a alike.

import { el, clear, humanBytes, dropTarget, showError } from './dom.js';
import { convertOne, targetSummary } from '../convert.js';
import { makeZip, saveBlob } from '../zip.js';
import * as source from '../cardsource.js';

export function mountConvert(root, ctx) {
  const banks = ctx.layout.audioBanks();
  let files = [];
  let results = [];

  const status = el('div', { class: 'status' });
  const out = el('div', { class: 'results' });
  const fileList = el('ul', { class: 'filelist' });

  const engineSel = el('select', { onchange: refreshOptions },
    banks.map((b) => el('option', { value: b.engine }, b.engine)));
  const deckSel = el('select', {}, [el('option', {}, 'a'), el('option', {}, 'b')]);
  const bankSel = el('select', {}, Array.from({ length: 16 }, (_, i) => el('option', {}, String(i))));
  const tapeSel = el('select', {}, ctx.layout.granularTapes.map((t) => el('option', {}, t)));
  const slotInput = el('input', { type: 'number', min: '0', max: '48', value: '1', class: 'slot' });
  const rateInput = el('input', { type: 'number', min: '3000', max: '96000', value: '48000', class: 'slot' });

  const deckField = field('Deck', deckSel);
  const bankField = field('Bank / shelf', bankSel);
  const tapeField = field('Tape', tapeSel);
  const slotField = field('First slot', slotInput);
  const rateField = field('Sample rate', rateInput);
  const targetNote = el('div', { class: 'muted note' });

  function field(label, control) {
    return el('label', { class: 'field' }, el('span', {}, label), control);
  }

  const bank = () => ctx.layout.bank(engineSel.value);

  function refreshOptions() {
    const b = bank();
    const uses = (t) => b.target.includes(t);
    deckField.hidden = !uses('{deck}');
    bankField.hidden = !uses('{bank}');
    tapeField.hidden = !uses('{tape}');
    // Only the bard bank has no fixed rate; everywhere else the firmware demands one exact value and
    // offering a control would only invite getting it wrong.
    rateField.hidden = b.fmt.rate != null;
    if (b.engine === 'bard') rateInput.value = '24000';
    slotInput.value = b.engine === 'radio' || b.engine === 'bard' ? '1' : '1';
    targetNote.textContent = `Writes ${targetSummary(b, Number(rateInput.value))}`
      + (b.max_seconds ? ` - trimmed to ${b.max_seconds} s, because this engine loads into RAM` : '')
      + (b.scanned ? `, looped up to ${ctx.layout.scan.min_bytes / 1024} KB if shorter` : '');
    renderFiles();
  }

  function renderFiles() {
    clear(fileList);
    const b = bank();
    files.forEach((f, i) => {
      fileList.append(el('li', {},
        el('span', { class: 'name' }, f.name),
        el('span', { class: 'muted' }, humanBytes(f.size)),
        el('button', {
          class: 'link',
          onclick: () => {
            files.splice(i, 1);
            renderFiles();
          },
        }, 'remove')));
    });
    convertBtn.disabled = !files.length;
    zipBtn.disabled = true;
    saveBtn.disabled = true;
    status.textContent = files.length
      ? `${files.length} file(s) -> ${b.engine}, starting at slot ${slotInput.value}`
      : '';
  }

  function addFiles(list) {
    for (const f of list) files.push(f);
    renderFiles();
  }

  const drop = el('div', { class: 'dropzone' },
    el('p', {}, 'Drop audio files here'),
    el('p', { class: 'muted' }, 'mp3, flac, wav, ogg, m4a - decoded by the browser, nothing uploaded'));
  dropTarget(drop, (dt) => addFiles([...dt.files]));

  const picker = el('input', {
    type: 'file',
    multiple: true,
    accept: 'audio/*',
    class: 'hidden',
    onchange: (e) => addFiles([...e.target.files]),
  });

  async function convert() {
    clear(out);
    results = [];
    const b = bank();
    const start = Number(slotInput.value) || 0;
    status.textContent = 'Decoding...';
    try {
      for (let i = 0; i < files.length; i++) {
        const f = files[i];
        status.textContent = `Decoding ${f.name} (${i + 1}/${files.length})...`;
        const r = await convertOne(ctx.layout, b, { name: f.name, data: await f.arrayBuffer() }, {
          index: start + i,
          deck: deckSel.value,
          bank: Number(bankSel.value),
          tape: tapeSel.value,
          rate: Number(rateInput.value),
        });
        results.push(r);
        out.append(el('div', { class: 'finding ok' },
          el('div', { class: 'path' }, `${f.name}  ->  ${r.path}`),
          el('div', { class: 'problem' },
            `${humanBytes(r.bytes.length)}, from ${r.sourceRate} Hz `
            + `${r.sourceChannels === 2 ? 'stereo' : 'mono'}`),
          r.notes.length && el('div', { class: 'fix' }, r.notes.join('; '))));
      }
      status.textContent = `${results.length} file(s) converted`;
      zipBtn.disabled = false;
      saveBtn.disabled = !source.hasFileSystemAccess();
    } catch (e) {
      status.textContent = '';
      out.append(el('div', { class: 'finding error' },
        el('div', { class: 'problem' }, `Could not decode: ${e.message}`),
        el('div', { class: 'fix' }, 'The browser decodes mp3, flac, wav, ogg and m4a. A DRM-protected '
          + 'or unusual file may need converting with ffmpeg first.')));
    }
  }

  async function downloadZip() {
    const blob = await makeZip(results.map((r) => ({ path: r.path, bytes: r.bytes })));
    saveBlob(blob, `sk-${bank().engine}-files.zip`);
  }

  async function saveToCard() {
    try {
      const card = await source.pickDirectory('readwrite');
      const { written, failed } = await source.writeInto(
        card.handle, results.map((r) => ({ path: r.path, bytes: r.bytes })),
      );
      status.textContent = `${written.length} written to the card, ${failed.length} failed`;
      for (const f of failed) {
        out.append(el('div', { class: 'finding error' },
          el('div', { class: 'path' }, f.path), el('div', { class: 'problem' }, f.error)));
      }
    } catch (e) {
      if (e.name !== 'AbortError') showError(out, e);
    }
  }

  const convertBtn = el('button', { class: 'primary', disabled: true, onclick: convert }, 'Convert');
  const zipBtn = el('button', { disabled: true, onclick: downloadZip }, 'Download as .zip');
  const saveBtn = el('button', { disabled: true, onclick: saveToCard }, 'Save onto the card');

  slotInput.addEventListener('input', renderFiles);
  rateInput.addEventListener('input', refreshOptions);

  root.append(
    el('p', { class: 'lead' },
      'Decodes with the browser\'s own audio engine and re-encodes to exactly what the target engine '
      + 'reads - right container, bit depth, channel count and sample rate. No install, and no '
      + 'format-support lottery.'),
    el('div', { class: 'fields' },
      field('Engine', engineSel), deckField, bankField, tapeField, slotField, rateField),
    targetNote,
    el('div', { class: 'controls' },
      el('button', { onclick: () => picker.click() }, 'Choose audio files'),
      convertBtn, zipBtn, saveBtn, picker),
    drop,
    fileList,
    status,
    out,
    el('div', { class: 'callout' },
      el('strong', {}, 'On resampling: '),
      'the browser\'s resampler is not bit-identical to libsox\'s or ffmpeg\'s. None of the three agree '
      + 'with each other today, so this is not a regression - but it does mean this page cannot '
      + 'reproduce a particular card byte for byte. For a 50x pstretch source, where artefacts have a '
      + 'long time to become audible, converting with ffmpeg is worth comparing against.'),
  );
  refreshOptions();
}
