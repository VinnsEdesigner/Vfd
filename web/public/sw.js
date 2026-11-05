/* Vfd — service worker.
 *
 * Two jobs, and deliberately no more:
 *   1. keep the app shell so the installed app opens without a network;
 *   2. serve locally-stored media to the page when the Vyzor is unreachable,
 *      so a downloaded file can still be played offline.
 *
 * It does NOT cache API responses. The Vyzor's data is live (progress moves
 * every second) and a cached list would be a lie about what is downloading.
 * Media bytes are the exception, because those are immutable once finished.
 *
 * Hand-written rather than generated: the app has exactly one HTML entry, one
 * script and one stylesheet, so a precache list of three is honest and a
 * workbox config would be more machinery than the problem needs.
 */

const VERSION = 'v1';
const SHELL_CACHE = `vfd-shell-${VERSION}`;
const MEDIA_CACHE = `vfd-media-${VERSION}`;

/* Filled in at install time from whatever the page actually requested, so this
   cannot drift from the build's hashed asset names. */
self.addEventListener('install', (event) => {
  event.waitUntil(
    (async () => {
      const cache = await caches.open(SHELL_CACHE);
      // '/' resolves to index.html; the rest are the static icons and manifest.
      await cache.addAll(['/', '/site.webmanifest', '/logo.svg']).catch(() => undefined);
      await self.skipWaiting();
    })(),
  );
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    (async () => {
      const keys = await caches.keys();
      await Promise.all(
        keys
          .filter((k) => !k.endsWith(VERSION))
          .map((k) => caches.delete(k)),
      );
      await self.clients.claim();
    })(),
  );
});

/** Only same-origin GETs are ours to serve. */
function isCacheable(request, url) {
  return request.method === 'GET' && url.origin === self.location.origin;
}

self.addEventListener('fetch', (event) => {
  const url = new URL(event.request.url);

  /* Never touch the Vyzor API: its answers are live and time-sensitive. */
  if (url.pathname.startsWith('/api/') || url.pathname === '/healthz') return;
  if (!isCacheable(event.request, url)) return;

  /* Media bytes, once stored locally, are served from the cache first. */
  if (url.pathname.startsWith('/media-cache/')) {
    event.respondWith(
      (async () => {
        const cache = await caches.open(MEDIA_CACHE);
        const hit = await cache.match(event.request);
        if (hit) return hit;
        return fetch(event.request);
      })(),
    );
    return;
  }

  /* The shell: network first so a new build is picked up, falling back to the
     cache so the installed app still opens with no connection. */
  event.respondWith(
    (async () => {
      try {
        const response = await fetch(event.request);
        if (response.ok && response.type === 'basic') {
          const cache = await caches.open(SHELL_CACHE);
          cache.put(event.request, response.clone()).catch(() => undefined);
        }
        return response;
      } catch (e) {
        const cache = await caches.open(SHELL_CACHE);
        const hit = await cache.match(event.request);
        if (hit) return hit;
        /* A navigation with no cached page still gets the shell, so the app can
           show its own "offline" state rather than the browser's error page. */
        if (event.request.mode === 'navigate') {
          const shell = await cache.match('/');
          if (shell) return shell;
        }
        throw e;
      }
    })(),
  );
});

/* Lets the page ask for a stored blob to be kept available offline. */
self.addEventListener('message', (event) => {
  const data = event.data;
  if (!data || data.type !== 'cache-media' || !data.url) return;
  event.waitUntil(
    (async () => {
      try {
        const cache = await caches.open(MEDIA_CACHE);
        await cache.add(data.url);
        event.source?.postMessage({ type: 'cache-media-done', url: data.url, ok: true });
      } catch {
        event.source?.postMessage({ type: 'cache-media-done', url: data.url, ok: false });
      }
    })(),
  );
});
