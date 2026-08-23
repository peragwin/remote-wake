/**
 * Service worker — offline app shell.
 *
 * Strategy:
 *   • Own assets (same-origin, in the precache list): cache-first, with a
 *     background revalidate so a redeploy is picked up on the next launch.
 *   • Navigations: try the network briefly, fall back to the cached shell.
 *   • Everything else (the relay API, cross-origin): NETWORK ONLY, never
 *     cached. Command envelopes and presence answers must never be replayed
 *     from a cache — a stale "online" or a re-sent command would both be
 *     wrong, and the relay's responses carry no signature.
 *
 * Bump CACHE_VERSION when shipping changed assets.
 */

const CACHE_VERSION = 'v1';
const CACHE_NAME = `remote-wake-${CACHE_VERSION}`;

const PRECACHE = [
  './',
  './index.html',
  './styles.css',
  './manifest.webmanifest',
  './src/app.js',
  './src/api.js',
  './src/commands.js',
  './src/crypto.js',
  './src/keys.js',
  './src/pairing.js',
  './src/qr.js',
  './src/signing.js',
  './src/store.js',
  './src/ui.js',
  './src/util.js',
  './src/webauthn.js',
  './vendor/noble-ed25519.js',
  './icons/icon.svg',
  './icons/icon-192.png',
  './icons/icon-512.png',
  './icons/maskable-512.png',
  './icons/apple-touch-icon.png',
];

self.addEventListener('install', (event) => {
  event.waitUntil(
    (async () => {
      const cache = await caches.open(CACHE_NAME);
      // addAll is all-or-nothing; add individually so one bad path cannot
      // block the whole install.
      await Promise.all(
        PRECACHE.map((url) =>
          cache.add(new Request(url, { cache: 'reload' })).catch((err) => {
            console.warn('[sw] precache miss', url, err.message);
          })
        )
      );
      await self.skipWaiting();
    })()
  );
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    (async () => {
      const names = await caches.keys();
      await Promise.all(
        names.filter((n) => n.startsWith('remote-wake-') && n !== CACHE_NAME).map((n) => caches.delete(n))
      );
      await self.clients.claim();
    })()
  );
});

/** Is this one of our own static assets? */
function isOwnAsset(url) {
  if (url.origin !== self.location.origin) return false;
  const scope = new URL('./', self.location).pathname;
  if (!url.pathname.startsWith(scope)) return false;
  // The relay API is never same-origin in the normal deployment, but if the
  // app is served *from* the relay it would be — so exclude it explicitly.
  return !url.pathname.startsWith(`${scope}v1/`);
}

self.addEventListener('fetch', (event) => {
  const { request } = event;

  // Only ever intercept GETs. POSTs (i.e. every command) go straight out.
  if (request.method !== 'GET') return;

  const url = new URL(request.url);

  // Network-only for anything that is not ours: the relay, and cross-origin.
  if (!isOwnAsset(url)) return;

  if (request.mode === 'navigate') {
    event.respondWith(
      (async () => {
        try {
          const fresh = await fetch(request);
          const cache = await caches.open(CACHE_NAME);
          cache.put('./index.html', fresh.clone());
          return fresh;
        } catch {
          const cached = await caches.match('./index.html', { ignoreSearch: true });
          return cached || Response.error();
        }
      })()
    );
    return;
  }

  event.respondWith(
    (async () => {
      const cache = await caches.open(CACHE_NAME);
      const cached = await cache.match(request, { ignoreSearch: true });

      const network = fetch(request)
        .then((response) => {
          if (response && response.ok && response.type === 'basic') {
            cache.put(request, response.clone());
          }
          return response;
        })
        .catch(() => null);

      if (cached) {
        event.waitUntil(network); // revalidate in the background
        return cached;
      }
      const fresh = await network;
      return fresh || new Response('Offline', { status: 503, statusText: 'Offline' });
    })()
  );
});

self.addEventListener('message', (event) => {
  if (event.data === 'skip-waiting') self.skipWaiting();
});
