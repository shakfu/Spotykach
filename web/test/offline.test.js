// offline.test.js - the service worker must cache the app it is shipped with.
//
// sw.js lists its assets by hand, and nothing else in the build knows that list exists. The failure it
// invites is quiet and one-sided: add a module, forget the list, and the app keeps working in every
// test and every online visit, because the network answers. It breaks only offline, only for users who
// have already installed the worker, and only for the tab that needed the missing file - which is
// precisely the audience the worker was added for, someone standing next to the hardware with the
// wifi somebody else's problem.
//
// So the list is checked against the files that actually ship, in both directions.

import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative } from 'node:path';

import { suite, test, ok, eq } from './harness.js';

suite('offline');

const WEB = new URL('..', import.meta.url).pathname;
const sw = readFileSync(join(WEB, 'sw.js'), 'utf8');

/** The ASSETS array, read out of the source - there is no other way in without a worker global. */
function assets() {
  const body = sw.match(/const ASSETS = \[([\s\S]*?)\];/);
  ok(body, 'sw.js no longer declares an ASSETS array in a form this test can read');
  return [...body[1].matchAll(/'([^']+)'/g)].map((m) => m[1]);
}

/**
 * Everything served to a browser: the page, its styles, its data and its modules.
 *
 * Deliberately a filesystem walk rather than a list, because a list here would need the same
 * maintenance as the one it is checking and would fail the same way.
 */
function shipped(dir = WEB, out = []) {
  for (const name of readdirSync(dir)) {
    // The tests, the node manifest and the worker itself are not part of what a browser loads. A
    // worker that cached itself would also pin its own replacement.
    if (['test', 'node_modules', 'package.json', 'sw.js', 'README.md', '.DS_Store'].includes(name)) continue;
    const full = join(dir, name);
    if (statSync(full).isDirectory()) shipped(full, out);
    else out.push(`./${relative(WEB, full)}`);
  }
  return out;
}

test('the service worker caches every file the app ships', () => {
  const listed = new Set(assets());
  const missing = shipped().filter((f) => !listed.has(f));
  eq(missing, [], 'these ship but are not cached, so the app breaks offline once installed');
});

test('the service worker caches nothing that no longer exists', () => {
  // addAll() is atomic: one 404 rejects the install and the app silently has no offline support at
  // all, so a stale entry is worse than a missing one.
  const present = new Set(shipped());
  const stale = assets().filter((f) => f !== './' && !present.has(f));
  eq(stale, [], 'these are cached but absent, and one 404 fails the whole install');
});

test('the service worker caches the bare directory URL as well as index.html', () => {
  // The page is reachable as both, and the fetch handler falls back to './index.html' by name.
  const listed = assets();
  ok(listed.includes('./'), 'a visit to the directory URL must hit the cache too');
  ok(listed.includes('./index.html'), 'and the offline fallback target must be there to fall back to');
});

test('every module the page loads is reachable from the entry point', () => {
  // The asset list can be complete while the app is still broken: a module nobody imports is dead
  // weight, and index.html loading a script the walk never saw would be missed by both tests above.
  const entry = './js/ui/main.js';
  ok(readFileSync(join(WEB, 'index.html'), 'utf8').includes(entry), 'index.html loads the entry point');

  const seen = new Set();
  const walk = (rel) => {
    if (seen.has(rel)) return;
    seen.add(rel);
    const src = readFileSync(join(WEB, rel), 'utf8');
    for (const m of src.matchAll(/from '(\.[^']+)'/g)) {
      walk(`./${relative(WEB, join(WEB, rel, '..', m[1]))}`);
    }
  };
  walk(entry);

  const js = shipped().filter((f) => f.endsWith('.js'));
  eq(js.filter((f) => !seen.has(f)), [], 'these modules ship but nothing imports them');
});
