/**
 * Static-shell sanity checks — the things that silently break a no-build-step
 * PWA: a mistyped module path, an id referenced by JS that is not in the HTML,
 * an asset the manifest or service worker promises but does not ship, or an
 * accidental external dependency (the app must work on a plane, and must not
 * leak to a CDN).
 *
 * Complements test/serve-check.mjs, which fetches the same files over HTTP.
 */
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const APP = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const read = (p) => readFileSync(join(APP, p), 'utf8');

const html = read('index.html');
const manifest = JSON.parse(read('manifest.webmanifest'));
const sw = read('sw.js');

/**
 * Strip comments and string/template literals so that import scanning cannot
 * be fooled by prose. (The vendored noble file documents its own usage with an
 * `import … from '@noble/ed25519'` example inside a JSDoc block.)
 */
function stripNonCode(src) {
  let out = '';
  let i = 0;
  const n = src.length;
  while (i < n) {
    const c = src[i];
    const d = src[i + 1];
    if (c === '/' && d === '*') {
      const end = src.indexOf('*/', i + 2);
      i = end === -1 ? n : end + 2;
      out += ' ';
    } else if (c === '/' && d === '/') {
      const end = src.indexOf('\n', i);
      i = end === -1 ? n : end;
      out += ' ';
    } else if (c === '"' || c === "'" || c === '`') {
      // Keep the quotes and contents: import specifiers live in strings, and
      // only comments produce false positives here.
      const quote = c;
      let j = i + 1;
      while (j < n && src[j] !== quote) j += src[j] === '\\' ? 2 : 1;
      out += src.slice(i, Math.min(j + 1, n));
      i = j + 1;
    } else {
      out += c;
      i++;
    }
  }
  return out;
}

/** Every href/src referenced by index.html. */
function htmlRefs() {
  return [...html.matchAll(/(?:href|src)="(\.\/[^"]+)"/g)].map((m) => m[1]);
}

test('every asset index.html references exists on disk', () => {
  const refs = htmlRefs();
  assert.ok(refs.length >= 5, `expected several references, found ${refs.length}`);
  for (const ref of refs) {
    assert.ok(existsSync(join(APP, ref)), `index.html references a missing file: ${ref}`);
  }
});

test('every ES module import resolves to a real file', () => {
  const seen = new Set();
  const queue = ['src/app.js'];
  while (queue.length) {
    const file = queue.pop();
    if (seen.has(file)) continue;
    seen.add(file);
    assert.ok(existsSync(join(APP, file)), `missing module: ${file}`);
    const src = stripNonCode(read(file));
    for (const m of src.matchAll(/(?:^|[^\w.])(?:import|export)\b[^'"`;]*?from\s*['"]([^'"]+)['"]/g)) {
      resolveImport(file, m[1], queue);
    }
    for (const m of src.matchAll(/\bimport\(\s*['"]([^'"]+)['"]\s*\)/g)) {
      resolveImport(file, m[1], queue);
    }
  }
  // The whole graph should be reachable from the entry point.
  assert.ok(seen.has('src/signing.js'), 'signing.js must be reachable from app.js');
  assert.ok(seen.has('vendor/noble-ed25519.js'), 'the vendored fallback must be reachable');
});

function resolveImport(fromFile, spec, queue) {
  assert.ok(
    spec.startsWith('./') || spec.startsWith('../'),
    `${fromFile} imports a bare specifier "${spec}" — there is no bundler or import map to resolve it`
  );
  const target = join(dirname(fromFile), spec).replace(/\\/g, '/');
  assert.ok(existsSync(join(APP, target)), `${fromFile} imports a missing file: ${spec}`);
  queue.push(target);
}

test('no external network dependencies in the shell', () => {
  for (const [name, src] of [['index.html', html], ['styles.css', read('styles.css')]]) {
    const external = [...src.matchAll(/(?:href|src|url\()\s*["']?(https?:\/\/[^"')\s]+)/g)]
      .map((m) => m[1])
      .filter((u) => !u.startsWith('http://www.w3.org/'));
    assert.deepEqual(external, [], `${name} must be self-contained, found: ${external.join(', ')}`);
  }
});

test('manifest icons all exist and cover the required sizes', () => {
  for (const icon of manifest.icons) {
    assert.ok(existsSync(join(APP, icon.src)), `manifest icon missing: ${icon.src}`);
  }
  const sizes = manifest.icons.map((i) => i.sizes);
  assert.ok(sizes.includes('192x192'), 'a 192px icon is required for installability');
  assert.ok(sizes.includes('512x512'), 'a 512px icon is required for installability');
  assert.ok(
    manifest.icons.some((i) => i.purpose === 'maskable'),
    'a maskable icon is required for a good Android home-screen icon'
  );
  assert.equal(manifest.display, 'standalone');
  assert.ok(manifest.start_url && manifest.scope, 'start_url and scope are required');
});

test('manifest shortcut targets exist', () => {
  for (const s of manifest.shortcuts || []) {
    const path = s.url.split('?')[0];
    assert.ok(existsSync(join(APP, path)), `shortcut target missing: ${path}`);
    for (const i of s.icons || []) assert.ok(existsSync(join(APP, i.src)), `shortcut icon missing: ${i.src}`);
  }
});

test('service worker precaches every shipped module and asset', () => {
  const list = sw.match(/const PRECACHE = \[([\s\S]*?)\];/)[1];
  const entries = [...list.matchAll(/'([^']+)'/g)].map((m) => m[1]);

  for (const entry of entries) {
    if (entry === './') continue;
    assert.ok(existsSync(join(APP, entry)), `sw.js precaches a missing file: ${entry}`);
  }
  // Anything index.html or the module graph loads must be precached, or the
  // app will not start offline.
  for (const ref of htmlRefs()) {
    if (ref.startsWith('./icons/')) continue;
    assert.ok(entries.includes(ref), `sw.js does not precache ${ref}, so the app breaks offline`);
  }
  for (const mod of [
    './src/app.js', './src/api.js', './src/commands.js', './src/crypto.js',
    './src/keys.js', './src/pairing.js', './src/qr.js', './src/signing.js', './src/store.js',
    './src/ui.js', './src/util.js', './src/webauthn.js', './vendor/noble-ed25519.js',
  ]) {
    assert.ok(entries.includes(mod), `sw.js does not precache ${mod}`);
  }
});

test('service worker never caches the relay API or non-GET requests', () => {
  assert.match(sw, /request\.method !== 'GET'/, 'must bail out on non-GET (commands are POSTs)');
  assert.match(sw, /isOwnAsset/, 'must scope caching to its own assets');
  assert.match(sw, /v1\//, 'must explicitly exclude the /v1/ API path');
});

test('every element id the controller reaches for exists in the HTML', () => {
  const ids = new Set([...html.matchAll(/\bid="([^"]+)"/g)].map((m) => m[1]));
  const sources = ['src/app.js', 'src/ui.js'].map(read).join('\n');
  const used = new Set([...sources.matchAll(/\$\('#([a-zA-Z0-9_-]+)'\)/g)].map((m) => m[1]));

  const missing = [...used].filter((id) => !ids.has(id));
  assert.deepEqual(missing, [], `JS queries ids that index.html does not define: ${missing.join(', ')}`);
});

test('index.html is structurally sound', () => {
  assert.match(html, /^<!doctype html>/i);
  assert.match(html, /<html lang="en"/);
  assert.match(html, /<meta charset="utf-8">/);
  assert.match(html, /<meta name="viewport"[^>]*viewport-fit=cover/);
  assert.match(html, /<link rel="manifest" href="\.\/manifest\.webmanifest">/);
  assert.match(html, /<script type="module" src="\.\/src\/app\.js"><\/script>/);

  // Tags must balance for the containers the controller manipulates.
  for (const tag of ['html', 'head', 'body', 'main', 'header']) {
    const open = (html.match(new RegExp(`<${tag}[\\s>]`, 'g')) || []).length;
    const close = (html.match(new RegExp(`</${tag}>`, 'g')) || []).length;
    assert.equal(open, close, `unbalanced <${tag}> tags`);
  }
  for (const tag of ['div', 'section', 'article', 'button', 'p', 'span']) {
    const open = (html.match(new RegExp(`<${tag}[\\s>]`, 'g')) || []).length;
    const close = (html.match(new RegExp(`</${tag}>`, 'g')) || []).length;
    assert.equal(open, close, `unbalanced <${tag}> tags: ${open} open, ${close} close`);
  }
  // No duplicate ids.
  const all = [...html.matchAll(/\bid="([^"]+)"/g)].map((m) => m[1]);
  assert.equal(new Set(all).size, all.length, 'duplicate id attributes in index.html');
});

test('styles honour prefers-reduced-motion', () => {
  const css = read('styles.css');
  assert.match(css, /@media \(prefers-reduced-motion: reduce\)/);
});

test('the hold ring dasharray matches the SVG circle radius', () => {
  const css = read('styles.css');
  const r = Number(html.match(/class="hold-track" cx="50" cy="50" r="(\d+)"/)[1]);
  const dash = Number(css.match(/stroke-dasharray:\s*(\d+)/)[1]);
  const js = Number(read('src/ui.js').match(/const CIRCUMFERENCE = (\d+)/)[1]);
  const expected = Math.round(2 * Math.PI * r);
  assert.equal(dash, expected, 'CSS stroke-dasharray must equal the circumference');
  assert.equal(js, expected, 'ui.js CIRCUMFERENCE must equal the circumference');
});
