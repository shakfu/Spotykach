// convert_view.ts - drop audio in, get files the firmware can actually read.
//
// The screen that justifies the whole app. On the desktop this needs a decoder the user probably does
// not have: scripts/sk_card.py probes cysox, then ffmpeg, then sox, and the per-file `find_format`
// check exists because libsox's format support is a build-time property and mp3 is commonly missing.
// Here it is decodeAudioData, which every browser has, for mp3/flac/wav/ogg/m4a alike.

import { ConvertModel, type InputFile } from '../app/convert_model.ts';
import { cardAccess } from '../platform/cardsource.ts';
import { browserDecoder } from '../platform/audio.ts';
import { deflateRaw, downloader } from '../platform/download.ts';
import { aside, clear, dropTarget, el, finding, humanBytes } from './dom.ts';
import type { ViewContext } from './context.ts';

/** A browser File, reduced to what the model needs. */
const inputFor = (f: File): InputFile => ({
  name: f.name,
  size: f.size,
  bytes: () => f.arrayBuffer(),
});

export function mountConvert(root: HTMLElement, ctx: ViewContext): void {
  const model = new ConvertModel(ctx.layout, {
    decoder: browserDecoder, access: cardAccess, downloader, deflate: deflateRaw,
  });

  const status = el('div', { class: 'status' });
  const out = el('div', { class: 'results' });
  const fileList = el('ul', { class: 'filelist' });
  const targetNote = el('div', { class: 'muted note' });

  const field = (label: string, control: HTMLElement): HTMLLabelElement =>
    el('label', { class: 'field' }, el('span', {}, label), control);

  const engineSel = el('select', { onchange: () => model.setEngine(engineSel.value) },
    model.banks().map((b) => el('option', { value: b.engine }, b.engine)));
  const deckSel = el('select', { onchange: () => model.setField('deck', deckSel.value) },
    [el('option', {}, 'a'), el('option', {}, 'b')]);
  const bankSel = el('select', { onchange: () => model.setField('bank', Number(bankSel.value)) },
    Array.from({ length: 16 }, (_, i) => el('option', {}, String(i))));
  const tapeSel = el('select', { onchange: () => model.setField('tape', tapeSel.value) },
    ctx.layout.granularTapes.map((t) => el('option', {}, t)));
  const slotInput = el('input', {
    type: 'number', min: '0', max: '48', value: '1', class: 'slot',
    oninput: () => model.setField('slot', Number(slotInput.value) || 0),
  });
  const rateInput = el('input', {
    type: 'number', min: '3000', max: '96000', value: '48000', class: 'slot',
    oninput: () => model.setField('rate', Number(rateInput.value) || 48000),
  });

  const deckField = field('Deck', deckSel);
  const bankField = field('Bank / shelf', bankSel);
  const tapeField = field('Tape', tapeSel);
  const slotField = field('First slot', slotInput);
  const rateField = field('Sample rate', rateInput);

  const convertBtn = el('button', { class: 'primary', disabled: true, onclick: () => model.convert() },
    'Convert');
  const zipBtn = el('button', { disabled: true, onclick: () => model.downloadZip() },
    'Download as .zip');
  const saveBtn = el('button', { disabled: true, onclick: () => model.saveToCard() },
    'Save onto the card');

  const drop = el('div', { class: 'dropzone' },
    el('p', {}, 'Drop audio files here'),
    el('p', { class: 'muted' }, 'mp3, flac, wav, ogg, m4a - decoded by the browser, nothing uploaded'));
  dropTarget(drop, (dt) => model.addFiles([...dt.files].map(inputFor)));

  const picker = el('input', {
    type: 'file',
    multiple: true,
    accept: 'audio/*',
    class: 'hidden',
    onchange: (e: Event) => {
      const files = (e.target as HTMLInputElement).files;
      if (files) model.addFiles([...files].map(inputFor));
    },
  });

  model.store.subscribe((s) => {
    const fields = model.fields();
    deckField.hidden = !fields.deck;
    bankField.hidden = !fields.bank;
    tapeField.hidden = !fields.tape;
    rateField.hidden = !fields.rate;
    if (rateInput.value !== String(s.rate)) rateInput.value = String(s.rate);
    targetNote.textContent = model.summary();
    status.textContent = s.status;

    clear(fileList);
    s.files.forEach((f, i) => {
      fileList.append(el('li', {},
        el('span', { class: 'name' }, f.name),
        el('span', { class: 'muted' }, humanBytes(f.size)),
        el('button', { class: 'link', onclick: () => model.removeFile(i) }, 'remove')));
    });

    convertBtn.disabled = !model.canConvert();
    zipBtn.disabled = s.results.length === 0;
    saveBtn.disabled = !model.canSaveToCard();

    clear(out);
    if (s.error) {
      out.append(finding('error', '', `Could not decode: ${s.error}`,
        'The browser decodes mp3, flac, wav, ogg and m4a. A DRM-protected or unusual file may need '
        + 'converting with ffmpeg first.'));
    }
    for (const r of s.results) {
      out.append(finding('ok', `${r.path}`,
        `${humanBytes(r.bytes.length)}, from ${r.sourceRate} Hz `
        + `${r.sourceChannels === 2 ? 'stereo' : 'mono'}`,
        r.notes.length ? r.notes.join('; ') : undefined));
    }
  });

  root.append(
    el('p', { class: 'lead' },
      'Converts your audio to exactly what the target engine reads. mp3, flac, wav, ogg, m4a.'),
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
    aside('On resampling, and why this is not the CLI',
      el('p', {},
        'The browser\'s resampler is not bit-identical to libsox\'s or ffmpeg\'s. None of the three '
        + 'agree with each other today, so this is not a regression - but it does mean this page cannot '
        + 'reproduce a particular card byte for byte. For a 50x pstretch source, where artefacts have a '
        + 'long time to become audible, converting with ffmpeg is worth comparing against.'),
      el('p', {},
        'The upside is the reason this tab exists: decoding happens in the browser\'s own audio engine, '
        + 'so there is no install and no format-support lottery. The CLI needs ffmpeg, or cysox plus a '
        + 'libsox built with the right handlers.')),
  );
}
