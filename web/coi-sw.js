/*
 * coi-sw.js — cross-origin-isolation service worker shim.
 *
 * For static hosts that cannot send COOP/COEP response headers, this worker
 * re-serves every same-origin response with the two headers added, which is
 * sufficient for crossOriginIsolated to become true after one reload.
 * (Prefer real headers — serve.py and the nginx snippet in the README send
 * them — and treat this as a fallback for e.g. GitHub Pages.)
 */
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (e) => e.waitUntil(self.clients.claim()));

self.addEventListener('fetch', (event) => {
  const req = event.request;
  if (req.cache === 'only-if-cached' && req.mode !== 'same-origin') return;

  event.respondWith((async () => {
    const resp = await fetch(req);
    if (resp.status === 0) return resp;

    const headers = new Headers(resp.headers);
    headers.set('Cross-Origin-Opener-Policy', 'same-origin');
    headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
    headers.set('Cross-Origin-Resource-Policy', 'same-origin');

    return new Response(resp.body, {
      status: resp.status,
      statusText: resp.statusText,
      headers,
    });
  })());
});
