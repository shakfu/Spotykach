// flash_model.ts - the Flash tab's state, and the only place that decides what may be written.
//
// The split is deliberate and load-bearing. `core/dfu.ts` implements DFU faithfully and will write
// whatever address it is given, because a protocol implementation that argues with its caller is a
// worse protocol implementation. This file is the caller, and it will only ever name one address.
//
// WHY THIS IS DEFENSIBLE AT ALL. `web/README.md` kept flashing out of scope on the grounds that "a
// half-written image is the worst failure in the system". That is true of the general case and not of
// this one, and the difference is worth stating because it is the whole argument:
//
//   - The app lives in QSPI at 0x90040000. The BOOTLOADER lives in internal flash at 0x08000000, and
//     nothing here can address it: APP_ADDRESS is a constant, not a field, and `assertTarget` below
//     rejects everything else before a single USB transfer happens.
//   - So the worst outcome of an interrupted write is a device with a corrupt APP and an intact
//     bootloader - which still enters DFU on a 3-second Reset hold, and can simply be flashed again.
//     That is a retry, not a brick.
//   - The images identify themselves (core/image.ts), so the page refuses a bootloader binary and can
//     name the engine and version it is about to install instead of showing a filename.
//   - The write is read back and compared where the device supports UPLOAD, so "it flashed" is a
//     measurement rather than an inference.
//
// What this does NOT do, and will not: install a bootloader. That is the one operation whose failure
// is unrecoverable without hardware, it is a once-per-device procedure, and `build_release.py` calls
// it "deliberately not documented here". The page says so where somebody would look for it.

import { Store } from './store.ts';
import { flash, DfuError, type Progress } from '../core/dfu.ts';
import { APP_ADDRESS, describeImage, inspectImage, type ImageInfo } from '../core/image.ts';
import type { DfuDevice, UsbDfu } from '../core/ports.ts';

/** QSPI on the Daisy Seed erases in 64 KB blocks. */
export const ERASE_SIZE = 64 * 1024;

export interface FlashState {
  supported: boolean;
  device: string | null;
  /** The chosen file, once inspected. */
  image: ImageInfo | null;
  filename: string | null;
  busy: boolean;
  phase: Progress['phase'] | null;
  /** 0..1 within the current phase. */
  progress: number;
  /** Set when the write completed; says whether the read-back confirmed it. */
  result: { ok: boolean; verified: boolean; message: string } | null;
  error: string | null;
}

const EMPTY: FlashState = {
  supported: false, device: null, image: null, filename: null,
  busy: false, phase: null, progress: 0, result: null, error: null,
};

export interface FlashDeps {
  usb: UsbDfu;
  /** Injected so tests do not spend real seconds inside the poll loop. */
  sleep?: (ms: number) => Promise<void>;
  /** The "you are about to overwrite the firmware" gate. Returns false to stop. */
  confirm?: (question: string) => Promise<boolean> | boolean;
}

/**
 * Refuse any address but the app region.
 *
 * Exported and tested directly. This is a four-line function guarding the one irreversible thing in
 * the app, and the test that calls it with 0x08000000 is the most valuable test in this file.
 */
export function assertTarget(address: number): void {
  if (address !== APP_ADDRESS) {
    throw new DfuError(
      `refusing to write 0x${address.toString(16)}: this page only ever writes the application ` +
      `region at 0x${APP_ADDRESS.toString(16)}. Installing a bootloader is a separate procedure.`,
    );
  }
}

export class FlashModel {
  readonly store = new Store<FlashState>(EMPTY);
  private dev: DfuDevice | null = null;
  private bytes: Uint8Array | null = null;
  private cancel = { aborted: false };

  constructor(private deps: FlashDeps) {
    this.store.set({ supported: deps.usb.supported() });
  }

  private get sleep() {
    return this.deps.sleep ?? ((ms: number) => new Promise<void>((r) => setTimeout(r, ms)));
  }

  /** Inspect a chosen file. Nothing is written and no device is needed; this is pure byte reading. */
  select(filename: string, buf: ArrayBuffer): void {
    const image = inspectImage(buf);
    this.bytes = new Uint8Array(buf);
    this.store.set({ filename, image, result: null, error: null });
  }

  clearSelection(): void {
    this.bytes = null;
    this.store.set({ filename: null, image: null, result: null, error: null });
  }

  async connect(): Promise<void> {
    if (!this.deps.usb.supported()) {
      this.store.set({ error: 'this browser has no WebUSB' });
      return;
    }
    try {
      this.dev = await this.deps.usb.request();
      this.store.set({ device: this.dev.info(), error: null });
    } catch (e) {
      // Cancelling the chooser is a NotFoundError and is not an error the page should shout about.
      const msg = e instanceof Error ? e.message : String(e);
      const cancelled = /No device selected|NotFoundError/i.test(msg);
      this.store.set({ device: null, error: cancelled ? null : msg });
    }
  }

  async disconnect(): Promise<void> {
    await this.dev?.close();
    this.dev = null;
    this.store.set({ device: null, phase: null, progress: 0 });
  }

  /** Stop after the current block. The device is left in a state `reset()` recovers on the next go. */
  abort(): void {
    this.cancel.aborted = true;
  }

  async write(): Promise<void> {
    const s = this.store.get();
    if (!this.dev || !this.bytes || !s.image) return;
    if (!s.image.flashable) {
      this.store.set({ error: s.image.problems[0] });
      return;
    }

    // Before the confirm, not after: an abort raised while the dialog is up must not land on a flag
    // object that write() is about to replace.
    this.cancel = { aborted: false };

    const what = describeImage(s.image);
    const ask = this.deps.confirm;
    if (ask && !(await ask(`Overwrite the firmware on ${s.device} with ${what}?`))) return;

    this.store.set({ busy: true, error: null, result: null, phase: 'erase', progress: 0 });

    try {
      assertTarget(APP_ADDRESS); // the constant, checked against itself - see the note on the function
      const outcome = await flash(
        this.dev,
        this.bytes,
        {
          address: APP_ADDRESS,
          transferSize: this.dev.transferSize(),
          eraseSize: ERASE_SIZE,
          verify: true,
          signal: this.cancel,
          onProgress: (p) => {
            this.store.set({ phase: p.phase, progress: p.total > 0 ? p.done / p.total : 0 });
          },
        },
        this.sleep,
      );

      this.store.set({
        busy: false,
        phase: null,
        progress: 1,
        result: {
          ok: true,
          verified: outcome.verified,
          message: outcome.verified
            ? `${what} written and read back byte for byte. Power-cycle the device to run it.`
            : `${what} written. ${outcome.note ?? 'It could not be read back.'} ` +
              'Power-cycle the device to run it.',
        },
      });
      // The device resets itself out of DFU on manifest, so the handle is stale either way.
      this.dev = null;
      this.store.set({ device: null });
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      this.store.set({
        busy: false,
        phase: null,
        error: /cancelled/.test(msg)
          ? 'Cancelled. The app region is now partly written - flash again before using the device; ' +
            'the bootloader is untouched, so hold Reset for 3 seconds to get back here.'
          : msg,
      });
    }
  }
}
