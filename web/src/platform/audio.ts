// audio.ts - the browser's decoder, behind the AudioDecoder port.
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

import type { AudioDecoder, DecodedAudio } from '../core/ports.ts';

/** Decode a file to interleaved float samples at the requested rate and channel count. */
export async function decodeTo(
  data: ArrayBuffer, rate: number, channels: number,
): Promise<DecodedAudio> {
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

export const browserDecoder: AudioDecoder = { decode: decodeTo };
