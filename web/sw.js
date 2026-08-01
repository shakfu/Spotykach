// sw.js - offline support.
//
// Worth having for a tool people use standing next to hardware rather than at a desk: the card is in
// the reader, the device is on the bench, and the wifi is somebody else's problem. Everything the app
// needs is static, so caching it is straightforward.
//
// Cache-first with a background refresh: the page must open instantly and work with no network, but a
// stale copy of card_layout.json would silently check cards against last release's rules, so every
// fetch also refreshes the entry for next time. Bump CACHE when the asset list changes.

const CACHE = 'sk-card-v1';

const ASSETS = [
  './',
  './index.html',
  './app.css',
  './card_layout.json',
  './patches.json',
  './js/layout.js',
  './js/wav.js',
  './js/verify.js',
  './js/build.js',
  './js/convert.js',
  './js/zip.js',
  './js/cardsource.js',
  './js/ui/main.js',
  './js/ui/dom.js',
  './js/ui/verify_view.js',
  './js/ui/build_view.js',
  './js/ui/convert_view.js',
  './js/ui/terminal_view.js',
  './js/terminal/framing.js',
  './js/terminal/descriptor.js',
  './js/terminal/device.js',
  './js/terminal/serial.js',
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
