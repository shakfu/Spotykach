// sw.js - offline support.
//
// Worth having for a tool people use standing next to hardware rather than at a desk: the card is in
// the reader, the device is on the bench, and the wifi is somebody else's problem. Everything the app
// needs is static, so caching it is straightforward.
//
// Cache-first with a background refresh: the page must open instantly and work with no network, but a
// stale copy of card_layout.json would silently check cards against last release's rules, so every
// fetch also refreshes the entry for next time. Bump CACHE when the asset list changes.

// One bundle rather than the twenty modules this used to list: the sources are TypeScript now, so the
// browser is served `dist/app.js` and the hand-maintained module list - the thing that could silently
// go stale and break offline - is gone with it. The remaining entries are the page and its data.
const CACHE = 'sk-card-v7';

const ASSETS = [
  './',
  './index.html',
  './app.css',
  // Both themes, not just the active one: switching theme while offline must not produce an
  // unstyled page, and the inactive skin is 6 KB.
  './themes/system6.css',
  './themes/plain.css',
  './vendor/water.css/water.css',
  './card_layout.json',
  './patches.json',
  './engines.json',
  './dist/app.js',
  // system.css and the assets it url()-references. Only the woff2 faces: each @font-face declares two
  // separate `src` lines and the second one wins outright, so the .woff copies beside them are never
  // fetched. The two SVGs are the button border-images - without them every button loses its frame.
  './vendor/system.css/system.css',
  './vendor/system.css/button.svg',
  './vendor/system.css/button-default.svg',
  './vendor/system.css/ChicagoFLF.woff2',
  './vendor/system.css/ChiKareGo2.woff2',
  './vendor/system.css/FindersKeepers.woff2',
  './vendor/system.css/monaco.woff2',
];

self.addEventListener('install', (e) => {
  e.waitUntil(caches.open(CACHE).then((c) => c.addAll(ASSETS)).then(() => self.skipWaiting()));
});

self.addEventListener('activate', (e) => {
  e.waitUntil(
    caches.keys()
      .then((keys) => Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k))))
      .then(() => self.clients.claim()),
  );
});

// The engine pages and their diagrams are fetched on demand - 22 documents and 1.4 MB of SVG have no
// business in the install step - so they are cached the first time they are read rather than up front.
// The fetch handler below already does that for anything not in ASSETS.

self.addEventListener('fetch', (e) => {
  if (e.request.method !== 'GET') return;
  e.respondWith(
    caches.match(e.request).then((hit) => {
      const live = fetch(e.request).then((res) => {
        if (res.ok) caches.open(CACHE).then((c) => c.put(e.request, res.clone()));
        return res;
      });
      return hit || live;
    }).catch(() => caches.match('./index.html')),
  );
});
