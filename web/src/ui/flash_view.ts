// flash_view.ts - the Flash tab.
//
// The tab that `web/README.md` said would never exist, and the reasoning that changed is in
// flash_model.ts: this writes the APP region only, so a failed write costs a re-flash rather than a
// device. The page states that where a nervous person will read it, because "what happens if I unplug
// it halfway" is the actual question standing between somebody and this button.
//
// Two things it insists on before the button lights:
//   - a device in bootloader mode, chosen through the WebUSB picker;
//   - a file whose own bytes say it is a spotykach engine image and not, say, the bootloader.
// Neither is a formality. The second one refuses a real file that really is sitting in the repo root.

import { FlashModel } from '../app/flash_model.ts';
import { APP_ADDRESS, type ImageInfo } from '../core/image.ts';
import { webUsbDfu } from '../platform/usb.ts';
import { append, aside, clear, confirmDestructive, dropTarget, el, finding, humanBytes } from './dom.ts';
import type { ViewContext } from './context.ts';

const PHASE_LABEL: Record<string, string> = {
  erase: 'Erasing',
  write: 'Writing',
  verify: 'Reading back',
  manifest: 'Finishing',
};

/**
 * The chosen image, as findings - the same shape a bad card gets on the Verify tab.
 *
 * Built with dom.ts's `finding()` rather than by hand. That helper nests the badge and the path INSIDE
 * `.problem` and puts the separating spaces in, which is not decoration: assembling the three spans as
 * siblings instead renders `OKsk-bard-...binbard 0.6.1-...` with nothing between them, because
 * `.finding .problem` is what carries the spacing and a sibling span never gets it.
 */
function imageReport(info: ImageInfo, filename: string): HTMLElement {
  const rows: HTMLElement[] = [];

  const headline = info.engine && info.version
    ? `${info.engine} ${info.version}`
    : 'unidentified image';
  rows.push(finding(
    info.flashable ? 'ok' : 'error',
    filename,
    `${headline} - ${humanBytes(info.bytes)}, reset vector 0x${info.resetVector.toString(16)}`,
  ));

  for (const p of info.problems) rows.push(finding('error', '', p));
  for (const w of info.warnings) rows.push(finding('warn', '', w));
  return el('div', {}, rows);
}

export function mountFlash(root: HTMLElement, _ctx: ViewContext): void {
  const model = new FlashModel({ usb: webUsbDfu, confirm: (q) => confirmDestructive(q) });

  const status = el('div', { class: 'status' });
  const report = el('div');
  const bar = el('div', { class: 'console' });
  const result = el('div');

  const file = el('input', {
    type: 'file',
    accept: '.bin',
    onchange: async (e: Event) => {
      const f = (e.target as HTMLInputElement).files?.[0];
      if (f) model.select(f.name, await f.arrayBuffer());
    },
  });

  const connectBtn = el('button', {
    class: 'primary',
    onclick: () => (model.store.get().device ? model.disconnect() : model.connect()),
  }, 'Connect device');

  const flashBtn = el('button', { class: 'danger', onclick: () => model.write() }, 'Flash');
  const cancelBtn = el('button', { onclick: () => model.abort() }, 'Cancel');

  const drop = el('div', { class: 'dropzone' }, [
    el('p', {}, 'Drop an engine .bin here, or choose one:'),
    file,
  ]);
  dropTarget(drop, async (dt) => {
    const f = dt.files?.[0];
    if (f) model.select(f.name, await f.arrayBuffer());
  });

  append(root, [
    el('div', { class: 'controls' }, [connectBtn, flashBtn, cancelBtn]),
    status,
    drop,
    report,
    bar,
    result,

    aside('Why this is safe to interrupt, and what it will not do',
      el('p', {}, 'This page writes one address and one only: the application region at ' +
        `0x${APP_ADDRESS.toString(16)}, in QSPI. The bootloader lives somewhere else entirely - ` +
        'internal flash at 0x08000000 - and nothing here can address it.'),
      el('p', {}, 'So the worst case is a device with a half-written app and a working bootloader. ' +
        'Hold Reset for about 3 seconds until the pad LEDs breathe white, and flash it again. ' +
        'That is a retry, not a brick.'),
      el('p', {}, 'Installing a bootloader is the operation that genuinely can brick a device, it is ' +
        'done once per unit, and it is not offered here. Use dfu-util for it.'),
      el('p', {}, 'Where the device allows it, the image is read back and compared byte for byte ' +
        'after writing, so a successful flash is measured rather than assumed.')),

    aside('If the device does not appear',
      el('p', {}, 'The device must be in bootloader mode first: hold Reset for about 3 seconds until ' +
        'the pad LEDs breathe white. It then enumerates as 0483:df11.'),
      el('p', {}, 'WebUSB is Chromium-only - Chrome or Edge. Firefox and Safari do not implement it ' +
        'and will not; the command-line path works everywhere:'),
      el('pre', {}, `dfu-util -a 0 -s 0x${APP_ADDRESS.toString(16)}:leave -D sk-<engine>-<version>.bin -d ,0483:df11`),
      el('p', {}, 'On Linux, a udev rule is needed for a non-root browser to claim the interface - ' +
        'the same rule dfu-util needs.')),
  ]);

  model.store.subscribe((s) => {
    // --- what the page can do at all -----------------------------------------------------------
    if (!s.supported) {
      clear(status);
      append(status, [el('span', {}, 'This browser has no WebUSB, so flashing is not possible here. ' +
        'Use Chrome or Edge, or dfu-util (below).')]);
      connectBtn.disabled = true;
      flashBtn.disabled = true;
      cancelBtn.hidden = true;
      return;
    }

    connectBtn.textContent = s.device ? 'Disconnect' : 'Connect device';
    connectBtn.disabled = s.busy;
    file.disabled = s.busy;
    cancelBtn.hidden = !s.busy;

    // The button is the point of the tab and it stays off until every precondition is met, each of
    // which the status line names rather than leaving the reader to guess which one is missing.
    const ready = !!s.device && !!s.image?.flashable && !s.busy;
    flashBtn.disabled = !ready;

    clear(status);
    const bits: string[] = [];
    bits.push(s.device ? `Device: ${s.device}` : 'No device connected.');
    if (!s.image) bits.push('No image chosen.');
    else if (!s.image.flashable) bits.push('This image cannot be flashed - see below.');
    if (s.device && s.image?.flashable && !s.busy) {
      bits.push(`Ready to write 0x${APP_ADDRESS.toString(16)}.`);
    }
    append(status, [el('span', {}, bits.join(' '))]);

    // --- the chosen file -----------------------------------------------------------------------
    clear(report);
    if (s.image && s.filename) append(report, [imageReport(s.image, s.filename)]);

    // --- progress ------------------------------------------------------------------------------
    clear(bar);
    bar.hidden = !s.busy;
    if (s.busy && s.phase) {
      const pct = Math.round(s.progress * 100);
      append(bar, [
        el('div', { class: 'line' }, `${PHASE_LABEL[s.phase] ?? s.phase}... ${pct}%`),
        el('div', { class: 'line muted' }, 'Do not unplug the device. If you do, hold Reset for 3 ' +
          'seconds and flash again - the bootloader is not being written.'),
      ]);
    }

    // --- outcome -------------------------------------------------------------------------------
    clear(result);
    if (s.error) {
      append(result, [el('div', { class: 'verdict bad' }, [
        el('strong', {}, 'Failed'), el('p', {}, s.error),
      ])]);
    } else if (s.result) {
      append(result, [el('div', { class: s.result.verified ? 'verdict good' : 'verdict mixed' }, [
        el('strong', {}, s.result.verified ? 'Flashed and verified' : 'Flashed, unverified'),
        el('p', {}, s.result.message),
      ])]);
    }
  });
}
