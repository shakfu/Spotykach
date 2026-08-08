// download.ts - hand bytes to the user as a file, and the one compressor the ZIP writer wants.
//
// Both are trivial and both are browser-only, which is exactly why they are here rather than in core:
// `core/zip.ts` is arithmetic and stays testable, and takes the compressor as an argument.

import type { Deflate, Downloader } from '../core/ports.ts';

/** Hand bytes to the user as a download. */
export function saveBytes(bytes: Uint8Array, filename: string, mime = 'application/octet-stream'): void {
  const blob = new Blob([bytes as unknown as BlobPart], { type: mime });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 10_000);
}

export const downloader: Downloader = { save: saveBytes };

/**
 * Raw DEFLATE via `CompressionStream` (Chrome 103+, Firefox 113+, Safari 16.4+).
 *
 * Returns null where the API is missing or the engine rejects 'deflate-raw', which the ZIP writer
 * answers by storing the entry instead: a bigger archive, still a valid one.
 */
export const deflateRaw: Deflate = async (bytes) => {
  if (typeof CompressionStream === 'undefined') return null;
  try {
    const stream = new Blob([bytes as unknown as BlobPart]).stream()
      .pipeThrough(new CompressionStream('deflate-raw'));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  } catch {
    return null; // 'deflate-raw' unsupported on this engine; store instead
  }
};
