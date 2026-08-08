// flash.test.ts - the DFU download sequence and the image guard, against a scripted device.
//
// This is the one part of the app whose failure mode is a device that has to be recovered rather than
// a page that has to be reloaded, and it is also the part hardest to exercise by hand: getting real
// hardware into dfuERROR on purpose, or finding a bootloader that stalls UPLOAD, is not something a
// browser pass can do reliably. So the fake device below is not a convenience - it is the only place
// the error paths get run at all.
//
// What is NOT covered here, and no test in this file should be read as covering: that a real STM32H750
// in the spotykach bootloader accepts this exact sequence. That is a bench check, and it is written up
// in docs/dev/web-frontend-checks.md.

import { test, ok, eq, throws } from './harness.ts';
import {
  DfuError, flash, reset, setAddress,
  STATE_DFU_IDLE, STATE_DFU_DNBUSY, STATE_DFU_ERROR, STATE_DFU_DNLOAD_IDLE,
} from '../src/core/dfu.ts';
import { APP_ADDRESS, MAX_APP_BYTES, inspectImage, readBanner } from '../src/core/image.ts';
import { assertTarget, ERASE_SIZE, FlashModel } from '../src/app/flash_model.ts';
import type { DfuDevice, DfuStatus } from '../src/core/ports.ts';

const noSleep = async () => {};

/**
 * A device that records what it was told and answers however the test wants.
 *
 * It models the DfuSe address pointer for real - block-0 SET_ADDRESS moves `ptr`, a data download
 * lands at `ptr + (block-2)*xferSize`, and UPLOAD reads back from the same map. That is not
 * decoration: the bug this file failed to catch the first time was writing every chunk to the same
 * address, and a fake that keys stored data by BLOCK NUMBER cannot see that - it happily records
 * blocks 2,3,4 as distinct while a real device overwrites one address three times.
 */
class FakeDfu implements DfuDevice {
  log: Array<{ op: string; block?: number; bytes?: number[]; length?: number }> = [];
  /** Byte-addressed device memory, sparse. What the device actually holds. */
  memory = new Map<number, number>();
  /** The DfuSe address pointer, moved by block-0 SET_ADDRESS. */
  ptr = 0;
  /** Addresses handed to an erase command. */
  erased: number[] = [];
  state = STATE_DFU_IDLE;
  status = 0;
  /** Set to make the next getStatus report busy once, exercising the poll loop. */
  busyOnce = false;
  /** Set to make upload throw, as a bootloader that does not implement it would. */
  uploadStalls = false;
  /** Fault when this data block is downloaded, to exercise a mid-flight device error. */
  failOnBlock: number | null = null;
  /** Contents the device claims to hold, for verify tests. Overrides real memory. */
  contents: Uint8Array | null = null;
  /**
   * Answer every UPLOAD with this fixed junk instead of memory - what the spotykach bootloader
   * actually does. It acknowledges the request and returns an uninitialised buffer, so it neither
   * stalls (which would be honest) nor tells the truth.
   */
  uploadGarbage: Uint8Array | null = null;
  closed = false;

  async download(block: number, data: Uint8Array): Promise<void> {
    this.log.push({ op: 'download', block, bytes: [...data] });
    if (block === 0 && data.length === 5) {
      const addr = data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24);
      if (data[0] === 0x21) this.ptr = addr >>> 0;
      if (data[0] === 0x41) this.erased.push(addr >>> 0);
    } else if (block >= 2) {
      const base = this.ptr + (block - 2) * this.transferSize();
      for (let i = 0; i < data.length; i++) this.memory.set(base + i, data[i]);
    }
    if (this.failOnBlock !== null && block === this.failOnBlock) {
      this.state = STATE_DFU_ERROR;
      return;
    }
    if (this.busyOnce) this.state = STATE_DFU_DNBUSY;
  }

  /** What the device holds at an address, as the caller would read it back. */
  read(addr: number, len: number): Uint8Array {
    const out = new Uint8Array(len);
    for (let i = 0; i < len; i++) out[i] = this.memory.get(addr + i) ?? 0xff;
    return out;
  }
  async upload(block: number, length: number): Promise<Uint8Array> {
    this.log.push({ op: 'upload', block, length });
    if (this.uploadStalls) throw new Error('upload stalled (stall)');
    if (this.uploadGarbage) {
      const out = new Uint8Array(length);
      for (let i = 0; i < length; i++) out[i] = this.uploadGarbage[i % this.uploadGarbage.length];
      return out;
    }
    // `contents` overrides device memory, so a test can stage a device holding the WRONG bytes.
    if (this.contents) {
      const off = (block - 2) * length;
      return this.contents.subarray(off, Math.min(off + length, this.contents.length));
    }
    return this.read(this.ptr + (block - 2) * length, length);
  }
  async getStatus(): Promise<DfuStatus> {
    this.log.push({ op: 'getStatus' });
    if (this.state === STATE_DFU_DNBUSY && this.busyOnce) {
      this.busyOnce = false;
      return { status: this.status, state: STATE_DFU_DNBUSY, pollTimeout: 1 };
    }
    if (this.state === STATE_DFU_DNBUSY) this.state = STATE_DFU_DNLOAD_IDLE;
    return { status: this.status, state: this.state, pollTimeout: 0 };
  }
  async clearStatus(): Promise<void> {
    this.log.push({ op: 'clearStatus' });
    this.state = STATE_DFU_IDLE;
    this.status = 0;
  }
  async abort(): Promise<void> {
    this.log.push({ op: 'abort' });
    this.state = STATE_DFU_IDLE;
  }
  async close(): Promise<void> {
    this.closed = true;
  }
  transferSize(): number { return 1024; }
  info(): string { return 'FakeDFU 0483:df11'; }
}

/** A minimal Cortex-M image: SP, reset vector, then filler, with an optional banner. */
function makeImage(resetVector: number, size = 2048, banner?: string): ArrayBuffer {
  const buf = new ArrayBuffer(size);
  const view = new DataView(buf);
  view.setUint32(0, 0x20020000, true); // plausible DTCM stack pointer
  view.setUint32(4, resetVector, true);
  if (banner) {
    const bytes = new Uint8Array(buf);
    for (let i = 0; i < banner.length; i++) bytes[512 + i] = banner.charCodeAt(i);
  }
  return buf;
}

// --- image identification --------------------------------------------------------------------------

test('a SRAM app image is recognised and flashable', () => {
  const info = inspectImage(makeImage(0x24000985, 4096, 'spotykach 0.6.1 engine=delay'));
  eq(info.kind, 'sram-app');
  eq(info.engine, 'delay');
  eq(info.version, '0.6.1');
  ok(info.flashable, 'a released engine image must be flashable');
  eq(info.problems, []);
});

test('a QSPI app image is recognised and flashable', () => {
  const info = inspectImage(makeImage(0x90041a15, 4096, 'spotykach 0.6.1 engine=chuck'));
  eq(info.kind, 'qspi-app');
  eq(info.engine, 'chuck');
  ok(info.flashable, 'chuck/csound/mosc run from QSPI and are still apps');
});

test('a BOOTLOADER image is refused', () => {
  // The whole reason core/image.ts exists. bootloader-spotykach-v2.bin sits in the repo root next to
  // everything else, its reset vector is in internal flash, and writing it to the app address is a
  // valid DFU transaction that produces a device which boots to nothing. This is a slip, not a stretch.
  const info = inspectImage(makeImage(0x08001a51, 4096));
  eq(info.kind, 'bootloader');
  ok(!info.flashable, 'a bootloader must never be flashable from this page');
  ok(/bootloader/i.test(info.problems[0]), 'and must say so in the first problem');
});

test('a file that is not firmware at all is refused', () => {
  const info = inspectImage(makeImage(0x41414141, 4096));
  eq(info.kind, 'unknown');
  ok(!info.flashable, 'an unrecognised reset vector is not flashable');
});

test('an image too small to hold a vector table is refused', () => {
  const info = inspectImage(new ArrayBuffer(16));
  ok(!info.flashable, 'sixteen bytes is not a firmware image');
  ok(/too small/.test(info.problems[0]));
});

test('an image larger than the QSPI app region is refused', () => {
  const info = inspectImage(makeImage(0x24000985, MAX_APP_BYTES + 1024));
  ok(!info.flashable, 'it does not fit and the device would silently truncate it');
  ok(info.problems.some((p) => /does not fit/.test(p)));
});

test('a valid image with no banner flashes, but warns that it cannot be identified', () => {
  const info = inspectImage(makeImage(0x24000985, 4096));
  ok(info.flashable, 'a hand-built image is still a legitimate thing to flash');
  eq(info.engine, null);
  ok(info.warnings.some((w) => /banner/.test(w)), 'but the page must say it cannot name it');
});

test('the banner is found wherever the linker put it', () => {
  // Scanned rather than read at a fixed offset: .rodata lands differently per engine and per build.
  const big = new Uint8Array(200_000);
  const s = 'spotykach 0.6.1-2-gb7d494d engine=graincloud';
  for (let i = 0; i < s.length; i++) big[187_000 + i] = s.charCodeAt(i);
  const found = readBanner(big);
  eq(found?.engine, 'graincloud');
  eq(found?.version, '0.6.1-2-gb7d494d');
});

// --- the address guard -----------------------------------------------------------------------------

test('the app refuses to write anything but the application region', () => {
  // The most valuable test here. Everything else is about doing the write correctly; this is about
  // never doing the one write that cannot be undone from a browser.
  assertTarget(APP_ADDRESS); // does not throw
  throws(() => assertTarget(0x08000000), 'refusing to write',
    'internal flash - where the bootloader lives - must be refused');
  throws(() => assertTarget(0x90000000), 'refusing to write',
    'even elsewhere in QSPI is refused; there is exactly one legal target');
  throws(() => assertTarget(APP_ADDRESS + 4), 'refusing to write');
});

// --- the protocol ----------------------------------------------------------------------------------

test('every chunk is preceded by its own SET_ADDRESS and written to block 2', async () => {
  // THE REGRESSION. The first version set the address once and incremented the block number
  // (2, 3, 4...), which is a legal reading of DfuSe and does not work on this bootloader: flashing a
  // real Daisy produced a device whose very first byte did not match what was sent. devanlai/webdfu -
  // what the Daisy Web Programmer runs against the same hardware - re-issues SET_ADDRESS per chunk and
  // always downloads to block 2, and that is what this pins.
  const dev = new FakeDfu();
  const image = new Uint8Array(2048);
  for (let i = 0; i < image.length; i++) image[i] = i & 0xff;
  await flash(dev, image, {
    address: APP_ADDRESS, transferSize: 1024, eraseSize: ERASE_SIZE,
  }, noSleep);

  const dataBlocks = dev.log.filter((l) => l.op === 'download' && (l.block ?? 0) >= 2);
  eq(dataBlocks.length, 2, 'two chunks for 2048 bytes at 1024 apiece');
  ok(dataBlocks.every((d) => d.block === 2), 'every data chunk goes to block 2, never 3');

  const setAddrs = dev.log
    .filter((l) => l.op === 'download' && l.block === 0 && l.bytes?.[0] === 0x21)
    .map((l) => l.bytes!.slice(1))
    .map((b) => (b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)) >>> 0);
  ok(setAddrs.includes(APP_ADDRESS), 'the first chunk is addressed at the app base');
  ok(setAddrs.includes(APP_ADDRESS + 1024), 'and the second one chunk further in');

  // The point of all of it: the device ends up holding the image, at the right address.
  eq([...dev.read(APP_ADDRESS, 2048)], [...image], 'the device holds exactly the image');
});

test('the erase and the manifest bracket the write', async () => {
  const dev = new FakeDfu();
  await flash(dev, new Uint8Array(1024).fill(9), {
    address: APP_ADDRESS, transferSize: 1024, eraseSize: ERASE_SIZE,
  }, noSleep);
  eq(dev.erased, [APP_ADDRESS], 'the sector holding the image was erased first');
  const downloads = dev.log.filter((l) => l.op === 'download');
  const last = downloads[downloads.length - 1];
  eq(last.block, 0, 'the manifest is a block-0 download');
  eq(last.bytes, [], 'of zero length');
});

test('a partial final block is written at its real length, not padded', async () => {
  // Padding would write past the image. Harmless in an erased sector, but it makes the read-back
  // comparison meaningless, and "we wrote more than the file" is not a property to carry silently.
  const dev = new FakeDfu();
  await flash(dev, new Uint8Array(1536).fill(1), {
    address: APP_ADDRESS, transferSize: 1024, eraseSize: ERASE_SIZE,
  }, noSleep);
  const sizes = dev.log.filter((l) => l.op === 'download' && l.block === 2).map((l) => l.bytes!.length);
  eq(sizes, [1024, 512], 'the tail block is 512 bytes, not a padded 1024');
  eq(dev.read(APP_ADDRESS + 1536, 1)[0], 0xff, 'and nothing was written past the image');
});

test('only the sectors the image covers are erased', async () => {
  // Not a mass erase. There is nothing else in QSPI today, but "erases more than it was asked to" is
  // not a property to bake into a flasher that might later share the chip.
  const dev = new FakeDfu();
  await flash(dev, new Uint8Array(ERASE_SIZE + 1).fill(7), {
    address: APP_ADDRESS, transferSize: 1024, eraseSize: ERASE_SIZE,
  }, noSleep);
  eq(dev.erased, [APP_ADDRESS, APP_ADDRESS + ERASE_SIZE],
    'one sector plus one byte spills into a second sector, and no further');
});

test('the poll timeout is honoured rather than assumed away', async () => {
  // An erase of a 64 KB QSPI sector genuinely takes hundreds of milliseconds and the device is
  // entitled to say "not yet". Ignoring bwPollTimeout is the classic flasher bug that works on one
  // machine and corrupts on another.
  const dev = new FakeDfu();
  dev.busyOnce = true;
  const waits: number[] = [];
  await flash(dev, new Uint8Array(1024), {
    address: APP_ADDRESS, transferSize: 1024, eraseSize: ERASE_SIZE,
  }, async (ms) => { waits.push(ms); });
  ok(waits.length > 0, 'the host waited when the device said dfuDNBUSY');
});

test('a device error aborts the flash and reports what the device said', async () => {
  const dev = new FakeDfu();
  // Faults on the first data block, not up front: a device already in dfuERROR is CLEARED by reset(),
  // which is the behaviour the next test pins. This is the mid-flight failure.
  dev.failOnBlock = 2; // the first data chunk; every chunk is block 2 now
  dev.status = 0x03; // write failed
  let caught: unknown = null;
  try {
    await flash(dev, new Uint8Array(1024), {
      address: APP_ADDRESS, transferSize: 1024, eraseSize: ERASE_SIZE,
    }, noSleep);
  } catch (e) { caught = e; }
  ok(caught instanceof DfuError, 'a device fault is a DfuError, not a bare throw');
  ok(/write failed/.test((caught as Error).message), 'and carries the decoded status');
});

test('a device left in dfuERROR is cleared rather than being given up on', async () => {
  // The symptom this prevents: "flashing works exactly once per unplug". A failed attempt leaves the
  // device in dfuERROR, where it stalls everything until CLRSTATUS.
  const dev = new FakeDfu();
  dev.state = STATE_DFU_ERROR;
  dev.status = 0;
  await reset(dev, noSleep);
  ok(dev.log.some((l) => l.op === 'clearStatus'), 'CLRSTATUS was issued');
  eq(dev.state, STATE_DFU_IDLE);
});

test('the download loop does not abort after SET_ADDRESS, but the read-back does', async () => {
  // The two call sites differ, and both follow devanlai/webdfu because that is what works on this
  // bootloader: downloads stay in dfuDNLOAD_IDLE, reads abort to dfuIDLE first. The first version
  // aborted in both, and flashing real hardware with it did not take.
  const plain = new FakeDfu();
  await setAddress(plain, noSleep, APP_ADDRESS);
  ok(!plain.log.some((l) => l.op === 'abort'), 'the download form issues no abort');

  const aborting = new FakeDfu();
  await setAddress(aborting, noSleep, APP_ADDRESS, true);
  ok(aborting.log.some((l) => l.op === 'abort'), 'the read-back form does');

  // And the write loop must use the first form.
  const dev = new FakeDfu();
  await flash(dev, new Uint8Array(1024).fill(4), {
    address: APP_ADDRESS, transferSize: 1024, eraseSize: ERASE_SIZE,
  }, noSleep);
  const firstData = dev.log.findIndex((l) => l.op === 'download' && l.block === 2);
  const lastSetAddr = dev.log.map((l, i) => ({ l, i }))
    .filter((x) => x.l.op === 'download' && x.l.block === 0 && x.l.bytes?.[0] === 0x21)
    .map((x) => x.i).pop()!;
  ok(!dev.log.slice(lastSetAddr, firstData).some((l) => l.op === 'abort'),
    'no abort between the last SET_ADDRESS and the chunk it addresses');
});

test('verify reads the image back and passes when the bytes match', async () => {
  const dev = new FakeDfu();
  const image = new Uint8Array(2048);
  for (let i = 0; i < image.length; i++) image[i] = i & 0xff;
  // No staged `contents`: the read-back comes from what the write actually put in device memory, so
  // this passes only if the download loop addressed every chunk correctly.
  const res = await flash(dev, image, {
    address: APP_ADDRESS, transferSize: 1024, eraseSize: ERASE_SIZE, verify: true,
  }, noSleep);
  ok(res.verified, 'a device that reads back truthfully gets a verified result');
  eq([...dev.read(APP_ADDRESS, 2048)], [...image], 'and the bytes are right');
});

test('a device whose UPLOAD returns junk is detected BEFORE the write, not after', async () => {
  // THE REGRESSION, and it cost two failed flashes of real hardware. The spotykach bootloader answers
  // UPLOAD with an uninitialised buffer - `bc 87 17 85 ...` - rather than stalling it or reading QSPI.
  // Comparing after the write cannot tell that from a failed write, so it reported a GOOD flash as
  // "the flash did not take", twice.
  //
  // The probe runs immediately after the erase, which is the one moment the right answer is known: an
  // erased region must read as 0xFF. Anything else means UPLOAD is not reporting memory.
  const dev = new FakeDfu();
  dev.uploadGarbage = new Uint8Array([0xbc, 0x87, 0x17, 0x85, 0x1e, 0x28, 0x8d, 0x04]);
  const image = new Uint8Array(2048).fill(0x5a);

  const res = await flash(dev, image, {
    address: APP_ADDRESS, transferSize: 1024, eraseSize: ERASE_SIZE, verify: true,
  }, noSleep);

  ok(!res.verified, 'it must not claim verification it could not do');
  ok(/does not report memory/.test(res.note ?? ''), 'and must say why, in words a user can act on');
  // The write still has to have happened, correctly. This is the half that was being thrown away.
  eq([...dev.read(APP_ADDRESS, 2048)], [...image], 'the image was written regardless');
});

test('a device that stalls UPLOAD is unverified, not failed', async () => {
  // UPLOAD is optional in DFU 1.1. Stalling it is the honest "no" and must not fail the flash.
  const dev = new FakeDfu();
  dev.uploadStalls = true;
  const res = await flash(dev, new Uint8Array(1024).fill(3), {
    address: APP_ADDRESS, transferSize: 1024, eraseSize: ERASE_SIZE, verify: true,
  }, noSleep);
  ok(!res.verified);
  eq([...dev.read(APP_ADDRESS, 1024)], [...new Uint8Array(1024).fill(3)], 'and the write stands');
});

test('a device that reads back truthfully but holds the WRONG bytes still fails', async () => {
  // The probe must not become a way to skip verification altogether. A device that passes the probe
  // (erased region reads 0xFF) and then disagrees with the image has genuinely failed to write.
  const dev = new FakeDfu();
  const image = new Uint8Array(2048).fill(0xaa);
  let caught: unknown = null;
  try {
    await flash(dev, image, {
      address: APP_ADDRESS, transferSize: 1024, eraseSize: ERASE_SIZE, verify: true,
      // Staged after the probe passes: memory reads 0xFF when erased, then lies during the compare.
      onProgress: (p) => { if (p.phase === 'write' && p.done === p.total) dev.contents = new Uint8Array(2048).fill(0xbb); },
    }, noSleep);
  } catch (e) { caught = e; }
  ok(caught instanceof DfuError, 'a real mismatch is still a failure');
  const msg = (caught as Error).message;
  ok(/read-back mismatch at 0x90040000/.test(msg), 'it names the address');
  ok(/expected aa aa/.test(msg) && /returned bb bb/.test(msg), 'and reports what came back');
});

test('cancelling stops the write', async () => {
  const dev = new FakeDfu();
  const signal = { aborted: true };
  let caught: unknown = null;
  try {
    await flash(dev, new Uint8Array(8192), {
      address: APP_ADDRESS, transferSize: 1024, eraseSize: ERASE_SIZE, signal,
    }, noSleep);
  } catch (e) { caught = e; }
  ok(caught instanceof DfuError && /cancelled/.test(caught.message));
});

// --- the model -------------------------------------------------------------------------------------

function fakeUsb(dev: DfuDevice) {
  return { supported: () => true, request: async () => dev };
}

test('the model will not enable a flash without both a device and a good image', async () => {
  const dev = new FakeDfu();
  const model = new FlashModel({ usb: fakeUsb(dev), sleep: noSleep, confirm: () => true });

  model.select('sk-delay.bin', makeImage(0x24000985, 2048, 'spotykach 0.6.1 engine=delay'));
  await model.write(); // no device yet
  eq(dev.log.length, 0, 'nothing was sent without a connected device');

  await model.connect();
  eq(model.store.get().device, 'FakeDFU 0483:df11');
  await model.write();
  ok(dev.log.length > 0, 'with both present, the write proceeds');
  ok(model.store.get().result?.ok, 'and reports success');
});

test('the model refuses to write a bootloader image even when one is chosen', async () => {
  const dev = new FakeDfu();
  const model = new FlashModel({ usb: fakeUsb(dev), sleep: noSleep, confirm: () => true });
  await model.connect();
  model.select('bootloader-spotykach-v2.bin', makeImage(0x08001a51, 2048));
  await model.write();
  eq(dev.log.length, 0, 'not one USB transfer was issued');
  ok(/bootloader/i.test(model.store.get().error ?? ''), 'and the page says why');
});

test('declining the confirmation writes nothing', async () => {
  const dev = new FakeDfu();
  const model = new FlashModel({ usb: fakeUsb(dev), sleep: noSleep, confirm: () => false });
  await model.connect();
  model.select('sk-delay.bin', makeImage(0x24000985, 2048, 'spotykach 0.6.1 engine=delay'));
  await model.write();
  eq(dev.log.length, 0, 'saying no means no bytes leave the browser');
});

test('a cancelled flash tells the reader the device is still recoverable', async () => {
  // The sentence that matters most in the whole tab. Somebody who just cancelled a firmware write
  // needs to know, immediately, that the bootloader was not involved.
  const dev = new FakeDfu();
  const model = new FlashModel({ usb: fakeUsb(dev), sleep: noSleep, confirm: () => true });
  await model.connect();
  model.select('sk-delay.bin', makeImage(0x24000985, 200_000, 'spotykach 0.6.1 engine=delay'));
  const writing = model.write();
  model.abort();
  await writing;
  const err = model.store.get().error ?? '';
  ok(/hold Reset/i.test(err), 'it says how to get back into the bootloader');
  ok(/bootloader is untouched/i.test(err), 'and that the bootloader was never written');
});
