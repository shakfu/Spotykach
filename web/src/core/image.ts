// image.ts - what a firmware .bin actually is, decided from its bytes.
//
// This file is the reason flashing from a browser is defensible at all. The README's objection to
// in-browser DFU was never the USB protocol - it was that "a half-written image is the worst failure
// in the system", and a generic flasher (the Daisy Web Programmer included) cannot tell a spotykach
// app from a bootloader from a holiday photo. It writes what you give it.
//
// A spotykach .bin is self-identifying twice over, and both checks are pure byte inspection with no
// device attached:
//
//   1. THE BANNER. `src/version.cpp` links a literal `spotykach <version> engine=<name>` into every
//      image - the same string the release notes tell you to confirm with `arm-none-eabi-strings`.
//      So the page can name the engine and version it is about to write, rather than showing a
//      filename and hoping.
//
//   2. THE RESET VECTOR. Bytes 4..8 of an ARM Cortex-M image are the reset handler's address, and on
//      this hardware the region it points into says which kind of image this is:
//        0x24......  AXI SRAM   - a BOOT_SRAM app (most engines)
//        0x90......  QSPI       - a BOOT_QSPI app (chuck, csound, mosc)
//        0x08......  internal   - the BOOTLOADER, which must never be written to the app address
//      That last case is the one worth having a file for. Writing `bootloader-spotykach-v2.bin` to
//      0x90040000 is a perfectly valid DFU transaction that produces a device which boots to nothing,
//      and it is an easy mistake - it sits in the repo root next to everything else.
//
// Nothing here talks to a device. It answers "should this be flashed, and as what" so that the answer
// is settled before any USB endpoint is opened.

/** Where the bootloader expects an application. Mirrors QSPI_ADDRESS in lib/libDaisy/core/Makefile. */
export const APP_ADDRESS = 0x90040000;

/** Daisy Seed's QSPI part is 8 MB; the app region is everything above APP_ADDRESS. */
export const QSPI_END = 0x90800000;
export const MAX_APP_BYTES = QSPI_END - APP_ADDRESS;

/** The smallest thing that could be a Cortex-M image: a vector table needs at least this. */
const MIN_BYTES = 512;

export type ImageKind =
  | 'sram-app' // BOOT_SRAM: copied to AXI SRAM and run there
  | 'qspi-app' // BOOT_QSPI: executed in place from QSPI
  | 'bootloader' // runs from internal flash - NOT an app
  | 'unknown'; // no recognisable reset vector

export interface ImageInfo {
  kind: ImageKind;
  bytes: number;
  /** Initial stack pointer, bytes 0..4. */
  stackPointer: number;
  /** Reset handler address, bytes 4..8 - the field that decides `kind`. */
  resetVector: number;
  /** From the linked banner, when present. */
  version: string | null;
  engine: string | null;
  /** False when this must not be flashed to APP_ADDRESS; `problems` says why. */
  flashable: boolean;
  /** Hard reasons it cannot be flashed. Non-empty means the button stays disabled. */
  problems: string[];
  /** Reasons to look twice. Flashing proceeds, but the page says these out loud first. */
  warnings: string[];
}

/**
 * The banner, if this image carries one.
 *
 * Scanned as latin1 over the whole buffer rather than at a fixed offset: the string lands wherever the
 * linker puts .rodata, which differs per engine and per build. 2 MB of csound is a few milliseconds to
 * walk once, and it happens on file selection, not in a loop.
 */
export function readBanner(bytes: Uint8Array): { version: string; engine: string } | null {
  let text = '';
  // Chunked: String.fromCharCode.apply blows the argument limit somewhere around 100k on a 2.3 MB
  // csound image, and a per-byte loop over that is measurably slower than this.
  for (let i = 0; i < bytes.length; i += 0x8000) {
    text += String.fromCharCode(...bytes.subarray(i, Math.min(i + 0x8000, bytes.length)));
  }
  const m = /spotykach (\S+) engine=([a-z0-9_]+)/.exec(text);
  return m ? { version: m[1], engine: m[2] } : null;
}

/** Which memory the reset handler lives in, which is what distinguishes an app from the bootloader. */
function classify(resetVector: number): ImageKind {
  const region = resetVector >>> 24;
  if (region === 0x24) return 'sram-app';
  if (region === 0x90) return 'qspi-app';
  if (region === 0x08) return 'bootloader';
  return 'unknown';
}

/**
 * Decide everything about a candidate image, without a device.
 *
 * Returns rather than throws: every one of these is something the page has to render as a sentence,
 * and an exception would just be converted back into one at the call site.
 */
export function inspectImage(buf: ArrayBuffer): ImageInfo {
  const bytes = new Uint8Array(buf);
  const problems: string[] = [];
  const warnings: string[] = [];

  if (bytes.length < MIN_BYTES) {
    return {
      kind: 'unknown', bytes: bytes.length, stackPointer: 0, resetVector: 0,
      version: null, engine: null, flashable: false,
      problems: [`only ${bytes.length} bytes - too small to be a firmware image`],
      warnings: [],
    };
  }

  const view = new DataView(buf);
  const stackPointer = view.getUint32(0, true);
  const resetVector = view.getUint32(4, true);
  const kind = classify(resetVector);
  const banner = readBanner(bytes);

  // The one that matters. Flashing the bootloader to the app address is a valid transaction with a
  // useless result, and the file is sitting in the repo root - this is a slip, not a stretch.
  if (kind === 'bootloader') {
    problems.push(
      'this is a bootloader image (its reset vector is in internal flash at ' +
      `0x${resetVector.toString(16)}), not an engine. Installing a bootloader is a separate, ` +
      'device-level procedure and is deliberately not done from this page.',
    );
  }

  if (kind === 'unknown') {
    problems.push(
      `the reset vector (0x${resetVector.toString(16)}) points nowhere this hardware runs code from. ` +
      'This is probably not a Daisy firmware image at all.',
    );
  }

  if (bytes.length > MAX_APP_BYTES) {
    problems.push(
      `${(bytes.length / 1024).toFixed(0)} KB does not fit the ` +
      `${(MAX_APP_BYTES / 1024 / 1024).toFixed(0)} MB QSPI app region`,
    );
  }

  // A plausible initial SP lives in DTCM (0x2000....) or AXI SRAM (0x24......). Wrong here does not
  // prove the image is bad - it is a weaker signal than the reset vector - so it warns rather than
  // blocks, and the flash still requires a deliberate click.
  const spRegion = stackPointer >>> 24;
  if (spRegion !== 0x20 && spRegion !== 0x24) {
    warnings.push(
      `the initial stack pointer (0x${stackPointer.toString(16)}) is not in DTCM or AXI SRAM, ` +
      'which is unusual for a Daisy image',
    );
  }

  // No banner is not fatal - somebody may legitimately be flashing a third-party or hand-built image -
  // but it does mean the page cannot tell the user what they are about to install, and saying so is
  // more useful than staying quiet about it.
  if (!banner) {
    warnings.push(
      'no spotykach version banner in this image, so its engine and version cannot be confirmed. ' +
      'A released binary always carries one.',
    );
  }

  return {
    kind,
    bytes: bytes.length,
    stackPointer,
    resetVector,
    version: banner?.version ?? null,
    engine: banner?.engine ?? null,
    flashable: problems.length === 0,
    problems,
    warnings,
  };
}

/** `sk-delay-0.6.1.bin` -> a sentence naming what it is. Used for the confirmation line. */
export function describeImage(info: ImageInfo): string {
  const size = info.bytes < 1024 * 1024
    ? `${(info.bytes / 1024).toFixed(0)} KB`
    : `${(info.bytes / 1024 / 1024).toFixed(2)} MB`;
  if (info.engine && info.version) {
    const where = info.kind === 'qspi-app' ? 'runs from QSPI' : 'runs from SRAM';
    return `${info.engine} ${info.version} - ${size}, ${where}`;
  }
  return `unidentified image - ${size}`;
}
