// harness.ts - a test runner in a hundred lines, because the alternative is a package.json with
// dependencies for a page that deliberately has none at runtime.
//
// Runs under `bun` unmodified, TypeScript included - bun executes .ts directly, so the tests exercise
// the same source the bundler consumes rather than a compiled copy of it. The whole platform surface
// used here is ESM, `node:fs` and a global `crypto.subtle`.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import type { EngineData, LayoutData } from '../src/core/types.ts';

const HERE = dirname(fileURLToPath(import.meta.url));

export interface TestCase {
  name: string;
  fn: () => unknown | Promise<unknown>;
  file: string;
}

export const tests: TestCase[] = [];
let currentFile = '?';

export function suite(name: string): void {
  currentFile = name;
}

export function test(name: string, fn: () => unknown | Promise<unknown>): void {
  tests.push({ name, fn, file: currentFile });
}

export class AssertionError extends Error {}

export function ok(cond: unknown, msg = 'expected a truthy value'): asserts cond {
  if (!cond) throw new AssertionError(msg);
}

export function eq(actual: unknown, expected: unknown, msg = ''): void {
  const a = show(actual);
  const b = show(expected);
  if (a !== b) throw new AssertionError(`${msg}\n  actual:   ${a}\n  expected: ${b}`);
}

type Matcher = string | (new (...args: never[]) => Error);

function matches(e: unknown, matcher: Matcher): boolean {
  return typeof matcher === 'function' ? e instanceof matcher : String(e).includes(matcher);
}

export function throws(fn: () => unknown, matcher?: Matcher, msg = 'expected a throw'): unknown {
  try {
    fn();
  } catch (e) {
    if (matcher && !matches(e, matcher)) throw new AssertionError(`${msg}: wrong error ${e}`);
    return e;
  }
  throw new AssertionError(msg);
}

export async function rejects(
  promise: Promise<unknown>, matcher?: Matcher, msg = 'expected a rejection',
): Promise<unknown> {
  try {
    await promise;
  } catch (e) {
    if (matcher && !matches(e, matcher)) throw new AssertionError(`${msg}: wrong error ${e}`);
    return e;
  }
  throw new AssertionError(msg);
}

/** Stable, readable rendering for comparisons - typed arrays included, which JSON.stringify mangles. */
function show(v: unknown): string {
  if (v instanceof Uint8Array) return `Uint8Array(${v.length})[${[...v].join(',')}]`;
  if (ArrayBuffer.isView(v)) {
    const a = [...(v as unknown as Iterable<number>)];
    return `${v.constructor.name}(${a.length})[${a.join(',')}]`;
  }
  if (v instanceof Map) return `Map{${[...v.entries()].map(([k, x]) => `${k}:${show(x)}`).join(',')}}`;
  if (v instanceof Set) return `Set{${[...v].join(',')}}`;
  return JSON.stringify(v, (_k, x) => (x instanceof Map ? Object.fromEntries(x) : x)) ?? String(v);
}

// --- fixture access --------------------------------------------------------------------------

export const fixturePath = (name: string): string => join(HERE, 'fixtures', name);
export const readFixture = (name: string): Uint8Array => new Uint8Array(readFileSync(fixturePath(name)));
export const readJson = <T = unknown>(name: string): T =>
  JSON.parse(readFileSync(fixturePath(name), 'utf8')) as T;
export const readWeb = <T = unknown>(rel: string): T =>
  JSON.parse(readFileSync(join(HERE, '..', rel), 'utf8')) as T;

/** The generated layout export, typed. Every suite starts from this. */
export const layoutData = (): LayoutData => readWeb<LayoutData>('card_layout.json');

/** The generated engine catalogue, typed. */
export const engineData = (): EngineData => readWeb<EngineData>('engines.json');

export const b64 = (s: string): Uint8Array => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));

export async function sha256hex(bytes: Uint8Array): Promise<string> {
  const d = await crypto.subtle.digest('SHA-256', bytes as unknown as BufferSource);
  return [...new Uint8Array(d)].map((x) => x.toString(16).padStart(2, '0')).join('');
}

/** Generated JSON fixtures: shape owned by scripts/web_export.py, not restated here. */
// eslint-disable-next-line @typescript-eslint/no-explicit-any
export type Fixture = any;

export interface FixtureFile {
  path: string;
  size: number;
  head: string;
}

export interface FixtureCase {
  files: FixtureFile[];
  [key: string]: unknown;
}

/**
 * Rebuild a CardEntry list from a fixture case. The fixture stores each file's real byte count plus a
 * prefix, since verify only ever parses headers - the reader zero-fills the rest, which every check
 * treats identically to the original file.
 */
export function entriesFromCase(caseData: FixtureCase) {
  return caseData.files.map((f) => {
    const head = b64(f.head);
    return {
      path: f.path,
      size: f.size,
      async read(max?: number): Promise<Uint8Array> {
        const n = Math.min(max ?? f.size, f.size);
        const out = new Uint8Array(n);
        out.set(head.subarray(0, Math.min(n, head.length)));
        return out;
      },
    };
  });
}

export async function run(): Promise<number> {
  let passed = 0;
  const failures: Array<{ t: TestCase; e: unknown }> = [];
  for (const t of tests) {
    try {
      await t.fn();
      passed++;
    } catch (e) {
      failures.push({ t, e });
    }
  }
  for (const { t, e } of failures) {
    console.error(`FAIL  ${t.file} :: ${t.name}\n  ${(e as Error).message}`);
    if (!(e instanceof AssertionError)) console.error((e as Error).stack);
  }
  const total = tests.length;
  console.log(`\n${passed}/${total} passed${failures.length ? `, ${failures.length} FAILED` : ''}`);
  return failures.length ? 1 : 0;
}
