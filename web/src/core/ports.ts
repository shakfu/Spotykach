// ports.ts - what the core needs from the outside world, stated as interfaces.
//
// This file is the reason `src/core/` contains no browser API and `src/app/` (the view-models) can be
// tested with no DOM at all. Everything the app does that only a browser can do - decode an mp3, open
// a serial port, write to a directory, hand the user a download, compress a ZIP entry - enters through
// one of these. `src/platform/` implements them against the real APIs; the tests implement them with
// twenty-line fakes.
//
// The rule that keeps this honest: nothing in core or app may import from platform. A test asserts it,
// because the failure is silent - one convenient `document.` reference and the model is no longer
// testable without a browser, and nobody notices until the shim needs another twenty lines.

import type { Card, CardFile, WriteResult } from './types.ts';

/** Decoded audio, interleaved, at the rate and channel count that was asked for. */
export interface DecodedAudio {
  samples: Float32Array;
  rate: number;
  channels: number;
  /** What the file actually was, before conversion - shown to the user, not used in encoding. */
  sourceRate: number;
  sourceChannels: number;
}

/**
 * mp3/flac/wav/ogg/m4a in, interleaved floats out.
 *
 * The whole reason the web front-end exists: on the desktop this is a decoder-backend problem (cysox,
 * then ffmpeg, then sox, with a per-file format probe because libsox's support is a build-time
 * property). In a browser it is one call that behaves the same everywhere.
 */
export interface AudioDecoder {
  decode(data: ArrayBuffer, rate: number, channels: number): Promise<DecodedAudio>;
}

/** Where a card comes from, and whether this browser can write back to it. */
export interface CardAccess {
  /** File System Access - in-place editing. False in Safari and Firefox, where the zip path is used. */
  hasDirectAccess(): boolean;
  pickDirectory(mode: 'read' | 'readwrite'): Promise<Card>;
  writeInto(handle: unknown, files: CardFile[], dirs?: string[]): Promise<WriteResult>;
}

/** Hand bytes to the user as a file. */
export interface Downloader {
  save(bytes: Uint8Array, filename: string, mime: string): void;
}

/**
 * Raw DEFLATE, injected rather than imported.
 *
 * `CompressionStream('deflate-raw')` is a browser API, and the ZIP writer is otherwise pure - two fixed
 * headers and a CRC. Passing the compressor in keeps `core/zip.ts` testable and lets it degrade to
 * stored entries (a bigger but perfectly valid archive) wherever the API is missing.
 *
 * Returns null when compression is unavailable or did not help.
 */
export type Deflate = (bytes: Uint8Array) => Promise<Uint8Array | null>;

/** A byte pipe to the device. Anything line-oriented satisfies it, including a scripted fake. */
export interface Transport {
  write(text: string): Promise<void>;
  onLine(cb: (line: string) => void): void;
  /** Called once if the port goes away by itself - unplugged, or reset into the bootloader. */
  onClose(cb: (reason: string) => void): void;
  close(): Promise<void>;
  /** Human-readable identification of the open port, e.g. `USB 0x0483:0x5740`. */
  info(): string;
}

export interface SerialPorts {
  supported(): boolean;
  /** `filtered` narrows the chooser to the Daisy's vendor id; false lists every port. */
  request(opts?: { filtered?: boolean }): Promise<Transport>;
}

/** setInterval/clearInterval, injected so the CPU poll can be driven synchronously in tests. */
export interface Clock {
  every(ms: number, fn: () => void): () => void;
}

/**
 * Fetches a generated documentation fragment.
 *
 * A port because the engine pages are the one thing this app loads lazily - 184 KB of rendered docs
 * has no business in the initial payload - and "what happens when that fetch fails" is a state the
 * page has to render properly rather than a thing to find out about in a browser.
 */
export interface DocSource {
  fetchPage(path: string): Promise<string>;
}
