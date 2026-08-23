/**
 * Zero-dependency static server for local development. `npm run serve`
 *
 * Serves app/ on http://localhost:8080 with the correct MIME types for ES
 * modules and the web manifest. localhost is a secure context, so WebCrypto,
 * WebAuthn and service workers all work without TLS.
 */

import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { join, extname, normalize, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const PORT = Number(process.env.PORT || 8080);

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.webmanifest': 'application/manifest+json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
  '.txt': 'text/plain; charset=utf-8',
};

const server = createServer(async (req, res) => {
  try {
    const url = new URL(req.url, `http://${req.headers.host}`);
    let path = decodeURIComponent(url.pathname);
    if (path.endsWith('/')) path += 'index.html';

    // Contain path traversal.
    const full = join(ROOT, normalize(path).replace(/^(\.\.[/\\])+/, ''));
    if (!full.startsWith(ROOT)) {
      res.writeHead(403).end('Forbidden');
      return;
    }

    const info = await stat(full);
    if (!info.isFile()) throw new Error('not a file');

    const body = await readFile(full);
    res.writeHead(200, {
      'content-type': TYPES[extname(full)] || 'application/octet-stream',
      'content-length': body.length,
      'cache-control': 'no-cache',
      // A service worker at ./sw.js needs to be served from the app scope.
      'service-worker-allowed': '/',
    });
    res.end(body);
  } catch {
    res.writeHead(404, { 'content-type': 'text/plain' }).end('404 Not Found');
  }
});

server.listen(PORT, () => {
  console.log(`remote-wake app → http://localhost:${PORT}/`);
});
