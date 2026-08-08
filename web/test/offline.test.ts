// offline.test.ts - the shipped artifact, and the layering that keeps the rest testable.
//
// Two jobs, both about things no other test can see:
//
//   1. The browser is served `dist/app.js`, but every test imports `src/`. So the suite can be green
//      while the page is stale or broken, and nothing else would notice.
//   2. The core/platform/app/ui split is an architecture claim, and architecture claims rot silently.
//      One convenient `document.` in a view-model and the models are no longer testable without a
//      browser - which nobody discovers until the next test needs a shim it should not have needed.

import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative } from 'node:path';

import { suite, test, ok, eq } from './harness.ts';

suite('offline');

const WEB = new URL('..', import.meta.url).pathname;
const read = (rel: string): string => readFileSync(join(WEB, rel), 'utf8');

/** Every file with one of `exts` under a directory, as repo-relative paths. */
function sources(dir: string, exts: string[] = ['.ts'], out: string[] = []): string[] {
  for (const name of readdirSync(join(WEB, dir))) {
    const rel = `${dir}/${name}`;
    if (statSync(join(WEB, rel)).isDirectory()) sources(rel, exts, out);
    else if (exts.some((e) => name.endsWith(e))) out.push(rel);
  }
  return out;
}

// --- the built bundle -----------------------------------------------------------------------------

test('the service worker caches the bundle and the data, and nothing that has moved', () => {
  // addAll() is atomic: one 404 rejects the install and the app silently has no offline support at
  // all, so a stale entry is worse than a missing one.
  const sw = read('sw.js');
  const listed = [...sw.matchAll(/'(\.[^']+)'/g)].map((m) => m[1]);
  ok(listed.includes('./dist/app.js'), 'the bundle the page actually loads');
  ok(listed.includes('./dist/app.css'), 'and the stylesheet, or an offline visit renders unstyled');
  ok(listed.includes('./card_layout.json'), 'and the data it is generated from');
  ok(listed.includes('./'), 'a visit to the directory URL must hit the cache too');
  ok(listed.includes('./index.html'), 'and the offline fallback target must be there to fall back to');

  const missing = listed.filter((f) => {
    if (f === './') return false;
    try {
      statSync(join(WEB, f));
      return false;
    } catch {
      return true;
    }
  });
  eq(missing, [], 'these are cached but absent, and one 404 fails the whole install');
});

// Both artifacts are committed so GitHub Pages can serve web/ with no CI step, which means either can
// be committed stale. Every test here reads src/ directly, so a forgotten `make web-build` is
// invisible everywhere except in the browser.
//
// They are checked SEPARATELY, against their own inputs. Lumping them together would blame the JS
// bundle for a CSS edit and - worse - would let dist/app.css go unguarded entirely, which is how the
// stylesheet ends up shipping a state nobody has seen.
//
// The CSS build ends in `touch`, and that is what makes this check usable rather than a trap.
// Tailwind skips writing when the output would be byte-identical, and a COMMENT-ONLY edit to
// src/app.css produces exactly that - comments do not survive minification. Without the touch the
// source is newer than an artifact the build refuses to rewrite, so this test goes red and no amount
// of rebuilding clears it. The build did run; the timestamp should say so.
for (const [artifact, exts] of [
  ['dist/app.js', ['.ts']],
  ['dist/app.css', ['.css']],
] as const) {
  test(`${artifact} is not older than the sources it was built from`, () => {
    const built = statSync(join(WEB, artifact)).mtimeMs;
    const stale = sources('src', [...exts])
      .filter((f) => statSync(join(WEB, f)).mtimeMs > built + 1000);
    eq(stale, [], 'these changed after the last build - run `make web-build`');
  });
}

test('the built stylesheet contains the app rather than a stub', () => {
  // The CSS equivalent of the bundle check below: Tailwind emits a valid, tiny file if its @source
  // globs match nothing, so "it compiled" is not evidence that any component survived.
  const css = read('dist/app.css');
  ok(css.length > 10_000, `dist/app.css is only ${css.length} bytes`);
  ok(css.includes('.finding'), 'the findings styling is in there');
  ok(css.includes('data-theme'), 'and the dark palette');
});

test('the bundle contains the app rather than a stub', () => {
  const js = read('dist/app.js');
  ok(js.length > 20_000, `dist/app.js is only ${js.length} bytes`);
  ok(js.includes('sk-card-starter.zip'), 'the build tab is in there');
  ok(js.includes('describe'), 'and so is the terminal client');
});

// --- layering -------------------------------------------------------------------------------------
//
// The rule, in one line: dependencies point inwards. ui -> app -> core, platform -> core, and nothing
// points back out.

const DOM_GLOBALS = /\b(document|window|navigator|localStorage|OfflineAudioContext|CompressionStream|URL\.createObjectURL)\b/;

/** Strip comments and string literals: this is about what the code DOES, not what it discusses. */
const executable = (src: string): string => src
  .replace(/\/\*[\s\S]*?\*\//g, ' ')
  .split('\n').map((l) => l.replace(/(^|\s)\/\/.*$/, '')).join('\n')
  .replace(/'(?:[^'\\]|\\.)*'/g, "''")
  .replace(/"(?:[^"\\]|\\.)*"/g, '""')
  .replace(/`(?:[^`\\]|\\.)*`/g, '``');

test('core touches no browser API, so it runs anywhere', () => {
  const offenders = sources('src/core')
    .filter((f) => DOM_GLOBALS.test(executable(read(f))));
  eq(offenders, [], 'these reach for a browser global instead of taking a port');
});

test('the view-models touch no browser API either - that is what makes them testable', () => {
  const offenders = sources('src/app')
    .filter((f) => DOM_GLOBALS.test(executable(read(f))));
  eq(offenders, [], 'a model that reaches for `document` cannot be tested without a DOM');
});

test('nothing inside core or app imports a platform adapter', () => {
  // The direction is the whole point. core/ports.ts declares what is needed; src/platform implements
  // it; the wiring happens in the views, which are allowed to know both.
  const offenders = [...sources('src/core'), ...sources('src/app')]
    .filter((f) => /from '[^']*platform\//.test(read(f)));
  eq(offenders, [], 'these depend outwards, which inverts the layering');
});

test('platform is the only place the DOM-specific APIs live', () => {
  // Not an aesthetic rule: the four APIs this app is built on (File System Access, Web Audio,
  // WebSerial, CompressionStream) are exactly the four things that cannot be tested here, so they are
  // kept where the untestable surface is small and named.
  const users = [...sources('src'), ...[]]
    .filter((f) => /\b(OfflineAudioContext|CompressionStream|showDirectoryPicker|navigator\.serial)\b/
      .test(executable(read(f))));
  eq(users.filter((f) => !f.startsWith('src/platform/')), [],
    'a browser-only API escaped the platform layer');
});

test('every module is reachable from the entry point', () => {
  // A module nothing imports is dead weight the bundler still has to be told about, and it is usually
  // the residue of a refactor that half-happened.
  const seen = new Set<string>();
  const walk = (rel: string): void => {
    if (seen.has(rel)) return;
    seen.add(rel);
    for (const m of read(rel).matchAll(/from '(\.[^']+)'/g)) {
      walk(relative(WEB, join(WEB, rel, '..', m[1])));
    }
  };
  walk('src/ui/main.ts');
  eq(sources('src').filter((f) => !seen.has(f)), [], 'these ship but nothing imports them');
});

test('a modal dialog is still centred despite Preflight', () => {
  // Tailwind's Preflight sets `margin: 0` on every element, which overrides the browser's own
  // `dialog:modal { margin: auto }` and pins About and the diagram viewer to the top-left corner.
  // src/app.css puts it back. Asserted against the BUILT stylesheet because the failure is an
  // interaction between two stylesheets, and only the compiled result shows who won.
  const css = read('dist/app.css');
  ok(/dialog:modal\{[^}]*margin:\s*auto/.test(css),
    'without this every modal falls to the top-left corner');
});
