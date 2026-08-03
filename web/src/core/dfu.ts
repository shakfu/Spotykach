// dfu.ts - the DFU 1.1 + DFuSe download sequence, with no USB API in it.
//
// This is the same conversation `dfu-util -a 0 -s 0x90040000:leave -D image.bin -d ,0483:df11` has,
// written against the `DfuDevice` port in ports.ts so it can be driven by a scripted fake. That
// matters more here than anywhere else in this codebase: this is the one piece of the app whose
// failure mode is a device that has to be recovered rather than a page that has to be reloaded, and
// it is the piece hardest to exercise by hand. Every state transition below is covered by a test
// against a fake device, including the ones that only happen when something goes wrong.
//
// DFuSe is ST's extension to DFU 1.1. Plain DFU downloads a blob to wherever the device decides;
// DFuSe adds a command channel on block 0 so the host can set an address pointer and erase pages
// first, which is what makes "write this to 0x90040000" expressible at all.
//
//   block 0, [0x21, addr32le]  set address pointer
//   block 0, [0x41, addr32le]  erase the page containing addr
//   block N>=2, data           write xferSize bytes at ptr + (N-2)*xferSize
//   zero-length download       manifest (commit), then the device leaves DFU
//
// Every one of those is followed by GETSTATUS, and the device answers with a poll timeout it wants
// honoured before the next request. Skipping that wait is the classic way to get a flasher that works
// on one machine and corrupts on another - an erase of a 64 KB QSPI sector genuinely takes hundreds
// of milliseconds, and the device is entitled to NAK until it is done.

import type { DfuDevice, DfuStatus } from './ports.ts';

// DFU class requests (USB DFU 1.1, table 3.2).
export const DFU_DETACH = 0;
export const DFU_DNLOAD = 1;
export const DFU_UPLOAD = 2;
export const DFU_GETSTATUS = 3;
export const DFU_CLRSTATUS = 4;
export const DFU_GETSTATE = 5;
export const DFU_ABORT = 6;

// Device states (DFU 1.1, table 6.1). Named as the spec names them so the wire log reads like the doc.
export const STATE_DFU_IDLE = 2;
export const STATE_DFU_DNLOAD_SYNC = 3;
export const STATE_DFU_DNBUSY = 4;
export const STATE_DFU_DNLOAD_IDLE = 5;
export const STATE_DFU_MANIFEST_SYNC = 6;
export const STATE_DFU_MANIFEST = 7;
export const STATE_DFU_MANIFEST_WAIT_RESET = 8;
export const STATE_DFU_ERROR = 10;

// DFuSe block-0 commands.
const CMD_SET_ADDRESS = 0x21;
const CMD_ERASE = 0x41;

/** Status codes worth naming; the rest are reported by number. */
const STATUS_TEXT: Record<number, string> = {
  0x00: 'OK',
  0x01: 'file rejected by the device',
  0x02: 'file failed its target verification',
  0x03: 'write failed - the address may be out of range',
  0x04: 'erase failed',
  0x05: 'erase check failed',
  0x06: 'programming failed',
  0x07: 'the device is write-protected',
  0x08: 'address out of range',
  0x09: 'the download ended early',
  0x0a: 'the firmware is corrupt',
  0x0b: 'vendor-specific error',
  0x0c: 'unexpected USB reset',
  0x0d: 'power-on reset detected',
  0x0e: 'unknown error',
  0x0f: 'the device stalled an unexpected request',
};

export function statusText(status: number): string {
  return STATUS_TEXT[status] ?? `device error 0x${status.toString(16).padStart(2, '0')}`;
}

export class DfuError extends Error {
  constructor(message: string, readonly status?: DfuStatus) {
    super(message);
    this.name = 'DfuError';
  }
}

export interface Progress {
  /** 'erase' then 'write' then 'verify'; each reports its own 0..1. */
  phase: 'erase' | 'write' | 'verify' | 'manifest';
  done: number;
  total: number;
}

export interface FlashOptions {
  address: number;
  /** Bytes per DNLOAD. Comes from the interface's wTransferSize; 1024 is what Daisy advertises. */
  transferSize: number;
  /** Sector size to erase with. QSPI on Daisy Seed erases in 64 KB blocks. */
  eraseSize: number;
  onProgress?: (p: Progress) => void;
  /** Read the image back and compare. Skipped automatically if the device refuses UPLOAD. */
  verify?: boolean;
  signal?: { aborted: boolean };
}

/** Sleep that a fake clock can skip. Injected rather than imported so tests do not wait in real time. */
export type Sleep = (ms: number) => Promise<void>;

/**
 * Poll GETSTATUS until the device stops saying "busy", honouring the timeout it asks for.
 *
 * The device returns bwPollTimeout with every status; the spec says wait at least that long before
 * asking again. `cap` stops a device that reports an absurd timeout from hanging the tab forever.
 */
async function settle(dev: DfuDevice, sleep: Sleep, cap = 5000): Promise<DfuStatus> {
  for (let i = 0; i < 1000; i++) {
    const st = await dev.getStatus();
    if (st.state === STATE_DFU_ERROR) {
      throw new DfuError(statusText(st.status), st);
    }
    if (st.state !== STATE_DFU_DNBUSY && st.state !== STATE_DFU_MANIFEST) return st;
    await sleep(Math.min(st.pollTimeout, cap));
  }
  throw new DfuError('the device never finished the last operation');
}

/**
 * Put the device back in dfuIDLE regardless of where it was left.
 *
 * Worth doing before every flash rather than assuming: a previous attempt that failed halfway leaves
 * the device in dfuERROR, where it stalls every subsequent request, and the symptom - "flashing works
 * once per unplug" - is a genuinely confusing one to debug from the outside.
 */
export async function reset(dev: DfuDevice, sleep: Sleep): Promise<void> {
  const st = await dev.getStatus();
  if (st.state === STATE_DFU_ERROR) {
    await dev.clearStatus();
  } else if (st.state !== STATE_DFU_IDLE) {
    await dev.abort();
  }
  const after = await settle(dev, sleep);
  if (after.state !== STATE_DFU_IDLE) {
    throw new DfuError(`the device will not return to idle (state ${after.state})`);
  }
}

function le32(cmd: number, addr: number): Uint8Array {
  const b = new Uint8Array(5);
  b[0] = cmd;
  b[1] = addr & 0xff;
  b[2] = (addr >>> 8) & 0xff;
  b[3] = (addr >>> 16) & 0xff;
  b[4] = (addr >>> 24) & 0xff;
  return b;
}

/**
 * DFuSe: point the device at an address. Every erase and write is relative to this.
 *
 * `abortAfter` is not a style choice - it is the difference between the two call sites, and both
 * follow devanlai/webdfu because that is the sequence proven against this bootloader:
 *
 *   - the DOWNLOAD loop sets the address and immediately downloads block 2, staying in
 *     dfuDNLOAD_IDLE. No abort.
 *   - the UPLOAD path sets the address, aborts back to dfuIDLE, and only then reads.
 *
 * Aborting in the download loop as well was part of the first version, which produced a device whose
 * first byte did not match what was sent.
 */
export async function setAddress(
  dev: DfuDevice, sleep: Sleep, addr: number, abortAfter = false,
): Promise<void> {
  await dev.download(0, le32(CMD_SET_ADDRESS, addr));
  await settle(dev, sleep);
  if (abortAfter) {
    await dev.abort();
    await settle(dev, sleep);
  }
}

/** DFuSe: erase one sector. The address is absolute, not an offset from the pointer. */
export async function erasePage(dev: DfuDevice, sleep: Sleep, addr: number): Promise<void> {
  await dev.download(0, le32(CMD_ERASE, addr));
  await settle(dev, sleep);
}

function aborted(opts: FlashOptions): boolean {
  return opts.signal?.aborted === true;
}

/** Index of the first differing byte, or -1. */
function firstDifference(want: Uint8Array, got: Uint8Array): number {
  for (let i = 0; i < want.length; i++) if (got[i] !== want[i]) return i;
  return -1;
}

/** Eight bytes from `at`, as hex - enough to recognise a vector table, a blank window or zeros. */
function hex(b: Uint8Array, at: number): string {
  return [...b.subarray(at, at + 8)].map((n) => n.toString(16).padStart(2, '0')).join(' ');
}

/**
 * Name the two shapes that mean "this is not really a failed write".
 *
 * A whole block of 0xFF is an erased window and a whole block of 0x00 is almost always an UPLOAD that
 * is not implemented - a bootloader is allowed to answer the request without reading anything. Both
 * are worth saying out loud, because the fix for "the flash did not take" and the fix for "this device
 * cannot read back" are nothing alike.
 */
function blankNote(got: Uint8Array): string {
  if (got.length === 0) return ' (the device returned no data at all)';
  if (got.every((b) => b === 0xff)) {
    return ' - the whole block read as 0xFF (erased), so either nothing was written or this ' +
      'bootloader does not read QSPI back';
  }
  if (got.every((b) => b === 0x00)) {
    return ' - the whole block read as zero, which usually means UPLOAD is not implemented rather ' +
      'than that the write failed';
  }
  return '';
}

export interface FlashResult {
  /** True only if the image was read back and matched. */
  verified: boolean;
  /** Why verification did not happen, when it did not. Shown to the user verbatim. */
  note?: string;
}

/**
 * Does this device's UPLOAD actually report memory?
 *
 * Run immediately after the erase, which is the one moment the correct answer is known: the region has
 * just been erased, so a device that reads it truthfully must return 0xFF. Anything else means UPLOAD
 * is answering without reading - and the spotykach bootloader does exactly that, returning an
 * uninitialised buffer (`bc 87 17 85 ...`) rather than stalling the request like a device that simply
 * does not implement it.
 *
 * That distinction is why this probe exists rather than a post-hoc guess. Comparing after the write
 * cannot tell "the write failed" from "the read is meaningless", and getting it wrong means either
 * reporting a good flash as broken - which this did, twice, on real hardware - or the reverse.
 */
async function uploadIsTrustworthy(
  dev: DfuDevice, sleep: Sleep, address: number, transferSize: number,
): Promise<boolean> {
  try {
    await reset(dev, sleep);
    await setAddress(dev, sleep, address, true);
    const probe = await dev.upload(2, transferSize);
    return probe.length > 0 && probe.every((b) => b === 0xff);
  } catch {
    return false; // a stalled UPLOAD is the honest kind of "no"
  } finally {
    await reset(dev, sleep).catch(() => {});
  }
}

/**
 * Erase, write and optionally verify an image at an absolute address.
 *
 * The address is the caller's to choose and the caller's to justify - `flash_model.ts` is what refuses
 * anything but the app region. Keeping that policy out of here means this file stays a faithful
 * implementation of the protocol rather than a half-policy that has to be argued with later.
 */
export async function flash(
  dev: DfuDevice,
  image: Uint8Array,
  opts: FlashOptions,
  sleep: Sleep,
): Promise<FlashResult> {
  const { address, transferSize, eraseSize } = opts;
  const report = opts.onProgress ?? (() => {});

  await reset(dev, sleep);

  // --- erase ---------------------------------------------------------------------------------------
  // Only the sectors the image actually covers. A mass erase would be one request instead of dozens,
  // and would also wipe whatever else lives in QSPI - on this hardware that is nothing today, but
  // "erase more than asked" is not a property to bake into a flasher.
  const firstSector = Math.floor(address / eraseSize) * eraseSize;
  const lastByte = address + image.length - 1;
  const sectors: number[] = [];
  for (let a = firstSector; a <= lastByte; a += eraseSize) sectors.push(a);

  for (let i = 0; i < sectors.length; i++) {
    if (aborted(opts)) throw new DfuError('cancelled');
    await erasePage(dev, sleep, sectors[i]);
    report({ phase: 'erase', done: i + 1, total: sectors.length });
  }

  // --- can this device be verified at all? ---------------------------------------------------------
  // Asked here, between the erase and the write, because this is the only moment the truth is known.
  let canVerify = false;
  let note: string | undefined;
  if (opts.verify) {
    canVerify = await uploadIsTrustworthy(dev, sleep, address, transferSize);
    if (!canVerify) {
      note = 'this bootloader does not report memory through DFU UPLOAD, so the image could not be ' +
        'read back. Confirm the device boots.';
    }
  }

  // --- write ---------------------------------------------------------------------------------------
  // SET_ADDRESS before EVERY chunk, always downloading to block 2 - not one SET_ADDRESS followed by
  // block 2, 3, 4... Both are legal readings of DfuSe (block N writes ptr + (N-2)*xferSize), and only
  // the first works on this bootloader. The second was tried on a real Daisy and produced a device
  // whose first byte did not match what was sent; this is the sequence devanlai/webdfu uses, which is
  // what the Daisy Web Programmer runs against the same hardware.
  //
  // The cost is one extra command per chunk - for csound at 2.3 MB that is ~2300 extra round trips -
  // and it is not negotiable for correctness.
  const blocks = Math.ceil(image.length / transferSize);
  for (let i = 0; i < blocks; i++) {
    if (aborted(opts)) throw new DfuError('cancelled');
    const chunk = image.subarray(i * transferSize, Math.min((i + 1) * transferSize, image.length));
    await setAddress(dev, sleep, address + i * transferSize);
    await dev.download(2, chunk);
    await settle(dev, sleep);
    report({ phase: 'write', done: i + 1, total: blocks });
  }

  // --- verify --------------------------------------------------------------------------------------
  // Only when the probe above said the device answers truthfully. Reading back is the only thing that
  // turns "the writes were acknowledged" into "the bytes are there", but a comparison against a
  // meaningless answer is worse than no comparison: it fails good flashes, which is what happened on
  // real hardware before the probe existed.
  if (opts.verify && canVerify) {
    await reset(dev, sleep);
    await setAddress(dev, sleep, address, true);
    for (let i = 0; i < blocks; i++) {
      if (aborted(opts)) throw new DfuError('cancelled');
      const want = image.subarray(i * transferSize, Math.min((i + 1) * transferSize, image.length));
      const got = await dev.upload(i + 2, transferSize);
      if (got.length < want.length) {
        throw new DfuError(`read back ${got.length} bytes where ${want.length} were written`);
      }
      const at = firstDifference(want, got);
      if (at >= 0) {
        throw new DfuError(
          `read-back mismatch at 0x${(address + i * transferSize + at).toString(16)}: ` +
          `expected ${hex(want, at)}, device returned ${hex(got, at)}${blankNote(got)}`,
        );
      }
      report({ phase: 'verify', done: i + 1, total: blocks });
    }
  }

  // --- manifest ------------------------------------------------------------------------------------
  // A zero-length download commits. The device then resets itself, which on WebUSB surfaces as the
  // transfer failing or the device vanishing mid-request - so a throw here is expected, not an error,
  // and is swallowed. This is the `:leave` in the dfu-util invocation, and the same benign
  // `Error 74 / get_status` the release notes warn about on the command line.
  report({ phase: 'manifest', done: 0, total: 1 });
  try {
    await dev.download(0, new Uint8Array(0));
    await settle(dev, sleep);
  } catch {
    /* the device left DFU mid-answer, which is exactly what was asked of it */
  }
  report({ phase: 'manifest', done: 1, total: 1 });
  return { verified: opts.verify === true && canVerify, note };
}
