// harness.js - a test runner in eighty lines, because the alternative is a package.json with
// dependencies for a page that deliberately has none.
//
// Runs under `node` or `bun` unmodified: both give us ESM, `node:fs`, and a global `crypto.subtle`,
// which is the entire platform surface the tests need. Nothing here touches a browser, matching the
// note in docs/dev/web-frontend.md that these tests do not need one.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));

/** @type {Array<{name: string, fn: Function, file: string}>} */
export const tests = [];
let currentFile = '?';

export function suite(name) {
  currentFile = name;
}

export function test(name, fn) {
  tests.push({ name, fn, file: currentFile });
}

export class AssertionError extends Error {}

export function ok(cond, msg = 'expected a truthy value') {
  if (!cond) throw new AssertionError(msg);
}

export function eq(actual, expected, msg = '') {
  const a = show(actual);
  const b = show(expected);
  if (a !== b) throw new AssertionError(`${msg}\n  actual:   ${a}\n  expected: ${b}`);
}

export function throws(fn, matcher, msg = 'expected a throw') {
  try {
    fn();
  } catch (e) {
    if (matcher && !(typeof matcher === 'function' ? e instanceof matcher : String(e).includes(matcher))) {
      throw new AssertionError(`${msg}: wrong error ${e}`);
    }
    return e;
  }
  throw new AssertionError(msg);
}

export async function rejects(promise, matcher, msg = 'expected a rejection') {
  try {
    await promise;
  } catch (e) {
    if (matcher && !(typeof matcher === 'function' ? e instanceof matcher : String(e).includes(matcher))) {
      throw new AssertionError(`${msg}: wrong error ${e}`);
    }
    return e;
  }
  throw new AssertionError(msg);
}

/** Stable, readable rendering for comparisons - typed arrays included, which JSON.stringify mangles. */
function show(v) {
  if (v instanceof Uint8Array) return `Uint8Array(${v.length})[${[...v].join(',')}]`;
  if (ArrayBuffer.isView(v)) return `${v.constructor.name}(${v.length})[${[...v].join(',')}]`;
  if (v instanceof Map) return `Map{${[...v.entries()].map(([k, x]) => `${k}:${show(x)}`).join(',')}}`;
  if (v instanceof Set) return `Set{${[...v].join(',')}}`;
  return JSON.stringify(v, (_k, x) => (x instanceof Map ? Object.fromEntries(x) : x));
}

// --- fixture access --------------------------------------------------------------------------

export const fixturePath = (name) => join(HERE, 'fixtures', name);
export const readFixture = (name) => new Uint8Array(readFileSync(fixturePath(name)));
export const readJson = (name) => JSON.parse(readFileSync(fixturePath(name), 'utf8'));
export const readWeb = (rel) => JSON.parse(readFileSync(join(HERE, '..', rel), 'utf8'));

export const b64 = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));

export async function sha256hex(bytes) {
  const d = await crypto.subtle.digest('SHA-256', bytes);
  return [...new Uint8Array(d)].map((x) => x.toString(16).padStart(2, '0')).join('');
}

/**
 * Rebuild a CardEntry list from a fixture case. The fixture stores each file's real byte count plus a
 * prefix, since verify only ever parses headers - the reader zero-fills the rest, which every check
 * treats identically to the original file.
 */
export function entriesFromCase(caseData) {
  return caseData.files.map((f) => {
    const head = b64(f.head);
    return {
      path: f.path,
      size: f.size,
      async read(max) {
        const n = Math.min(max ?? f.size, f.size);
        const out = new Uint8Array(n);
        out.set(head.subarray(0, Math.min(n, head.length)));
        return out;
      },
    };
  });
}

export async function run() {
  let passed = 0;
  const failures = [];
  for (const t of tests) {
    try {
      await t.fn();
      passed++;
    } catch (e) {
      failures.push({ t, e });
    }
  }
  for (const { t, e } of failures) {
    console.error(`FAIL  ${t.file} :: ${t.name}\n  ${e.message}`);
    if (!(e instanceof AssertionError)) console.error(e.stack);
  }
  const total = tests.length;
  console.log(`\n${passed}/${total} passed${failures.length ? `, ${failures.length} FAILED` : ''}`);
  return failures.length ? 1 : 0;
}
