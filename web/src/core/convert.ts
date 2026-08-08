// convert.ts - encode decoded audio into what the target engine actually reads.
//
// Decoding is NOT here: it is the one part that needs a browser (`decodeAudioData`), so it enters
// through the `AudioDecoder` port and lives in `platform/audio.ts`. What is left is arithmetic - the
// RAM cap, the scan floor, the container choice - and it is pure, which is what lets the convert
// view-model be tested end to end against a fake decoder with no browser in sight.
//
// Caveat worth stating rather than hiding, and the UI states it: the browser's resampler is not
// bit-identical to libsox's or ffmpeg's. None of the three agree with each other today, so this is not
// a regression - but it does mean the web app cannot reproduce a specific card byte for byte.

import { formatTarget, type Layout, type TargetVars } from './layout.ts';
import type { AudioDecoder } from './ports.ts';
import type { Bank } from './types.ts';
import { F32, INT16, padToBytes, writeRaw, writeWav, type Samples } from './wav.ts';

export interface Encoded {
  bytes: Uint8Array;
  notes: string[];
}

/**
 * Encode already-decoded samples for a bank, applying the same two adjustments the CLI applies:
 * a RAM cap for the engines that load rather than stream, and the scan floor for the browsed banks.
 */
export function encodeForBank(
  layout: Layout, bank: Bank, samples: Samples, rate: number,
): Encoded {
  const notes: string[] = [];
  const channels = bank.fmt.channels ?? 1;
  const encoding = bank.fmt.encodings[0];

  let out: Samples = samples;
  if (bank.max_seconds) {
    const cap = Math.floor(bank.max_seconds * rate * channels);
    if (out.length > cap) {
      notes.push(`trimmed to ${bank.max_seconds.toFixed(0)} s (${bank.engine} loads into RAM)`);
      // slice, not subarray: `Samples` includes a plain array, and the copy is irrelevant next to the
      // decode that produced these samples in the first place.
      out = out.slice(0, cap);
    }
  }
  if (bank.scanned) {
    const bps = encoding === INT16 ? 2 : 4;
    const before = out.length;
    out = padToBytes(out, layout.scan.min_bytes, bps);
    if (out.length > before) {
      notes.push(`looped up to ${layout.scan.min_bytes / 1024} KB - shorter files are skipped by the `
        + 'directory scan');
    }
  }

  const bytes = bank.fmt.container === 'raw'
    ? writeRaw(out)
    : writeWav(out, rate, channels, encoding);
  return { bytes, notes };
}

export interface ConvertInput {
  name: string;
  /** NOTE: decodeAudioData detaches this buffer; pass a copy if the caller still needs it. */
  data: ArrayBuffer;
}

export interface ConvertOptions extends TargetVars {
  index: number;
  rate?: number;
}

export interface ConvertResult {
  path: string;
  bytes: Uint8Array;
  notes: string[];
  sourceRate: number;
  sourceChannels: number;
}

/** Convert one input file for one bank, end to end, using whatever decoder was supplied. */
export async function convertOne(
  layout: Layout, bank: Bank, decoder: AudioDecoder, input: ConvertInput, opts: ConvertOptions,
): Promise<ConvertResult> {
  if (!bank.target) {
    throw new Error(`the ${bank.engine} engine reads text patches, not audio (${bank.fmt.describe})`);
  }
  const rate = bank.fmt.rate ?? opts.rate ?? 48000;
  const channels = bank.fmt.channels ?? 1;
  const { samples, sourceRate, sourceChannels } = await decoder.decode(input.data, rate, channels);
  const { bytes, notes } = encodeForBank(layout, bank, samples, rate);
  const path = formatTarget(bank.target, opts.index, {
    deck: opts.deck ?? 'a',
    bank: opts.bank ?? 0,
    tape: opts.tape ?? 'B',
  });
  return { path, bytes, notes, sourceRate, sourceChannels };
}

/** The encoding a bank's converted files are written in - for display. */
export function targetSummary(bank: Bank, rate: number): string {
  const enc = bank.fmt.encodings[0] === F32 ? '32-bit float' : '16-bit PCM';
  const container = bank.fmt.container === 'raw' ? 'headerless .raw' : '.wav';
  const ch = bank.fmt.channels === 2 ? 'stereo' : 'mono';
  return `${container}, ${enc}, ${ch}, ${bank.fmt.rate ?? rate} Hz`;
}
