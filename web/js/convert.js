// convert.js - put your own audio on the card, in the format the target engine actually reads.
//
// This is the part that is genuinely BETTER in a browser rather than merely also-available. The
// decoder-backend apparatus in scripts/sk_card.py - the cysox probe, the ffmpeg fallback, the sox
// fallback, the per-file `find_format` check - exists because desktop audio decoding is inconsistent
// across machines: libsox's format support is a build-time property, mp3/flac/ogg are commonly absent,
// and the user's problem is often that they have no working decoder at all. In a browser that entire
// apparatus collapses to one call that behaves identically everywhere:
//
//     BaseAudioContext.decodeAudioData  ->  mp3, flac, wav, ogg, m4a
//
// Better still, decodeAudioData resamples to the host context's rate as part of decoding, so
// constructing the OfflineAudioContext AT THE TARGET RATE makes the resample free rather than a second
// pass.
//
// Caveat worth stating rather than hiding: the browser's resampler is not bit-identical to libsox's or
// ffmpeg's. None of the three agree with each other today, so this is not a regression - but it does
// mean the web app cannot reproduce a specific card byte-for-byte, and the UI says so.

import { writeWav, writeRaw, padToBytes, F32, INT16 } from './wav.js';
import { formatTarget } from './layout.js';

/**
 * Decode a file to interleaved float samples at the requested rate and channel count.
 *
 * @param {ArrayBuffer} data  the encoded file. NOTE: decodeAudioData detaches this buffer; pass a copy
 *   if the caller still needs it.
 * @returns {Promise<{samples: Float32Array, rate: number, channels: number, sourceRate: number,
 *   sourceChannels: number}>}
 */
export async function decodeTo(data, rate, channels) {
  if (typeof OfflineAudioContext === 'undefined') {
    throw new Error('this browser has no Web Audio API, so it cannot decode audio');
  }
  // Decoding into a context at the TARGET rate does the sample-rate conversion for us - the spec has
  // decodeAudioData resample to the host context's rate.
  const decodeCtx = new OfflineAudioContext(1, 1, rate);
  const decoded = await decodeCtx.decodeAudioData(data);

  let buf = decoded;
  if (decoded.numberOfChannels !== channels) {
    // Channel conversion only; the rate already matches, so this render is a straight mix-down (or
    // duplication) using the standard "speakers" rules - the same (L+R)/2 downmix the CLI's decoders do.
    const mixCtx = new OfflineAudioContext(channels, decoded.length, rate);
    const src = mixCtx.createBufferSource();
    src.buffer = decoded;
    src.connect(mixCtx.destination);
    src.start();
    buf = await mixCtx.startRendering();
  }

  const n = buf.length;
  const out = new Float32Array(n * channels);
  for (let c = 0; c < channels; c++) {
    const chan = buf.getChannelData(c);
    for (let i = 0; i < n; i++) out[i * channels + c] = chan[i];
  }
  return {
    samples: out,
    rate,
    channels,
    sourceRate: decoded.sampleRate,
    sourceChannels: decoded.numberOfChannels,
  };
}

/**
 * Encode already-decoded samples for a bank, applying the same two adjustments the CLI applies:
 * a RAM cap for the engines that load rather than stream, and the scan floor for the browsed banks.
 *
 * @returns {{bytes: Uint8Array, notes: string[]}}
 */
export function encodeForBank(layout, bank, samples, rate) {
  const notes = [];
  const channels = bank.fmt.channels ?? 1;
  const encoding = bank.fmt.encodings[0];

  let out = samples;
  if (bank.max_seconds) {
    const cap = Math.floor(bank.max_seconds * rate * channels);
    if (out.length > cap) {
      notes.push(`trimmed to ${bank.max_seconds.toFixed(0)} s (${bank.engine} loads into RAM)`);
      out = out.subarray(0, cap);
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

/**
 * Convert one input file for one bank, end to end.
 *
 * @param {import('./layout.js').Layout} layout
 * @param {object} bank
 * @param {{name: string, data: ArrayBuffer}} input
 * @param {{index: number, deck?: string, bank?: number, tape?: string, rate?: number}} opts
 * @returns {Promise<{path: string, bytes: Uint8Array, notes: string[], sourceRate: number,
 *   sourceChannels: number}>}
 */
export async function convertOne(layout, bank, input, opts) {
  if (!bank.target) {
    throw new Error(`the ${bank.engine} engine reads text patches, not audio (${bank.fmt.describe})`);
  }
  const rate = bank.fmt.rate ?? opts.rate ?? 48000;
  const channels = bank.fmt.channels ?? 1;
  const { samples, sourceRate, sourceChannels } = await decodeTo(input.data, rate, channels);
  const { bytes, notes } = encodeForBank(layout, bank, samples, rate);
  const path = formatTarget(bank.target, opts.index, {
    deck: opts.deck ?? 'a',
    bank: opts.bank ?? 0,
    tape: opts.tape ?? 'B',
  });
  return { path, bytes, notes, sourceRate, sourceChannels };
}

/** The encoding a bank's converted files are written in - for display. */
export function targetSummary(bank, rate) {
  const enc = bank.fmt.encodings[0] === F32 ? '32-bit float' : '16-bit PCM';
  const container = bank.fmt.container === 'raw' ? 'headerless .raw' : '.wav';
  const ch = bank.fmt.channels === 2 ? 'stereo' : 'mono';
  return `${container}, ${enc}, ${ch}, ${bank.fmt.rate ?? rate} Hz`;
}
