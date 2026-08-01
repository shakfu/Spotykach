// zip.js - a minimal ZIP writer, so the card builder works in browsers without File System Access.
//
// This is the graceful-degradation path from docs/dev/web-frontend.md: WebSerial, WebUSB and the File
// System Access API are Chromium-only, but "drop files in, get a zip out" works everywhere, so the
// builder is designed around the zip and treats direct card access as the enhancement. That makes the
// app useful in Safari and Firefox rather than showing them a browser-upgrade notice.
//
// No dependency, because the alternative is bundling a library to write a container format whose
// entire specification-relevant surface here is two fixed-layout headers and a CRC. Deflate comes from
// CompressionStream('deflate-raw') where the browser has it (Chrome 103+, Firefox 113+, Safari 16.4+)
// and falls back to stored/uncompressed entries where it does not - a bigger download, still a valid
// archive.
//
// Entries are stamped with the DOS epoch (1980-01-01) rather than the current time, matching
// `sk_card.py dist`: a card zip whose bytes change on every build cannot be checksummed meaningfully,
// and reproducibility is cheap to keep.

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[i] = c >>> 0;
  }
  return t;
})();

export function crc32(bytes) {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

const DOS_DATE = ((1980 - 1980) << 9) | (1 << 5) | 1; // 1980-01-01
const DOS_TIME = 0;

const METHOD_STORE = 0;
const METHOD_DEFLATE = 8;

async function deflateRaw(bytes) {
  if (typeof CompressionStream === 'undefined') return null;
  try {
    const stream = new Blob([bytes]).stream().pipeThrough(new CompressionStream('deflate-raw'));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  } catch {
    return null; // 'deflate-raw' unsupported on this engine; store instead
  }
}

function u32(view, off, v) {
  view.setUint32(off, v >>> 0, true);
}

/**
 * Build a ZIP archive.
 *
 * @param {Array<{path: string, bytes: Uint8Array}>} files
 * @param {string[]} [dirs] empty directories to record, so a card's unused `radio/7` folders survive
 *   the round trip through the archive
 * @returns {Promise<Blob>}
 */
export async function makeZip(files, dirs = []) {
  const entries = [];
  for (const d of dirs) entries.push({ name: d.replace(/\/?$/, '/'), bytes: new Uint8Array(0), dir: true });
  for (const f of files) entries.push({ name: f.path, bytes: f.bytes, dir: false });

  const chunks = [];
  const central = [];
  let offset = 0;
  const enc = new TextEncoder();

  for (const e of entries) {
    const name = enc.encode(e.name);
    const crc = crc32(e.bytes);
    let method = METHOD_STORE;
    let body = e.bytes;
    if (!e.dir && e.bytes.length > 0) {
      const packed = await deflateRaw(e.bytes);
      // Only take the compressed form if it actually helped; tiny text files often inflate.
      if (packed && packed.length < e.bytes.length) {
        method = METHOD_DEFLATE;
        body = packed;
      }
    }

    const local = new Uint8Array(30 + name.length);
    const lv = new DataView(local.buffer);
    u32(lv, 0, 0x04034b50);
    lv.setUint16(4, 20, true); // version needed
    lv.setUint16(6, 0, true); // flags
    lv.setUint16(8, method, true);
    lv.setUint16(10, DOS_TIME, true);
    lv.setUint16(12, DOS_DATE, true);
    u32(lv, 14, crc);
    u32(lv, 18, body.length);
    u32(lv, 22, e.bytes.length);
    lv.setUint16(26, name.length, true);
    lv.setUint16(28, 0, true); // extra length
    local.set(name, 30);

    chunks.push(local, body);
    central.push({ name, crc, method, comp: body.length, raw: e.bytes.length, offset, dir: e.dir });
    offset += local.length + body.length;
  }

  const centralStart = offset;
  for (const c of central) {
    const rec = new Uint8Array(46 + c.name.length);
    const cv = new DataView(rec.buffer);
    u32(cv, 0, 0x02014b50);
    cv.setUint16(4, 20, true); // version made by
    cv.setUint16(6, 20, true); // version needed
    cv.setUint16(8, 0, true);
    cv.setUint16(10, c.method, true);
    cv.setUint16(12, DOS_TIME, true);
    cv.setUint16(14, DOS_DATE, true);
    u32(cv, 16, c.crc);
    u32(cv, 20, c.comp);
    u32(cv, 24, c.raw);
    cv.setUint16(28, c.name.length, true);
    cv.setUint16(30, 0, true); // extra
    cv.setUint16(32, 0, true); // comment
    cv.setUint16(34, 0, true); // disk
    cv.setUint16(36, 0, true); // internal attrs
    // External attrs: unix mode in the high 16 bits, plus the MS-DOS directory bit, so unzip recreates
    // empty folders with sane permissions instead of 000.
    u32(cv, 38, c.dir ? (0o040755 << 16) | 0x10 : 0o100644 << 16);
    u32(cv, 42, c.offset);
    rec.set(c.name, 46);
    chunks.push(rec);
    offset += rec.length;
  }

  const end = new Uint8Array(22);
  const ev = new DataView(end.buffer);
  u32(ev, 0, 0x06054b50);
  ev.setUint16(8, central.length, true);
  ev.setUint16(10, central.length, true);
  u32(ev, 12, offset - centralStart);
  u32(ev, 16, centralStart);
  chunks.push(end);

  return new Blob(chunks, { type: 'application/zip' });
}

/** Hand a Blob to the user as a download. */
export function saveBlob(blob, filename) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 10_000);
}
