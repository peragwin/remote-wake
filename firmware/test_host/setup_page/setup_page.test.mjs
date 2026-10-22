// Relay field behaviour of the served setup page (firmware/main/www/setup.html).
// The page's inline script runs unmodified in a vm context with a minimal DOM
// (only the element ids the page declares) and a fetch fake for /info and
// /config. Contract: the field is pre-filled only with an explicit override,
// the firmware default is shown as the placeholder, and Save omits `relay`
// when the field is empty or still equals the loaded override.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';

const pagePath = fileURLToPath(new URL('../../main/www/setup.html', import.meta.url));
const html = readFileSync(pagePath, 'utf8');
const scripts = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((m) => m[1]);
assert.equal(scripts.length, 1, 'setup.html has one inline script');
const ids = [...html.matchAll(/\bid="([^"]+)"/g)].map((m) => m[1]);

const DEFAULT = 'wss://relay.home.test';

function info(overrides) {
  return { deviceId: '0123456789abcdef', keys: 0, maxKeys: 4, ssid: '',
           relayUrl: DEFAULT, relayOverride: false, relayDefault: DEFAULT,
           fw: '1.0.1', ...overrides };
}

// Every macrotask boundary drains the page's promise chains.
const settle = () => new Promise((resolve) => setImmediate(resolve));

async function loadPage(firstInfo) {
  const elements = new Map(ids.map((id) => [id, {
    id, value: '', textContent: '', placeholder: '', className: '', style: {},
    listeners: {},
    addEventListener(type, fn) { this.listeners[type] = fn; },
  }]));
  const page = {
    info: firstInfo,
    posts: [],
    el: (id) => elements.get(id),
    async save() {
      let prevented = false;
      elements.get('f-net').listeners.submit({ preventDefault() { prevented = true; } });
      assert.ok(prevented, 'submit handler prevents navigation');
      await settle();
      const post = page.posts.at(-1);
      assert.equal(post.path, '/config');
      return post.body;
    },
  };
  const fetch = (path, opts = {}) => {
    let reply;
    if (path === '/info') {
      reply = page.info;
    } else {
      page.posts.push({ path, body: JSON.parse(opts.body) });
      reply = { ok: true };
    }
    return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(reply) });
  };
  const document = { getElementById: (id) => elements.get(id) ?? null };
  vm.runInNewContext(scripts[0], { document, fetch, JSON, Error, Promise });
  await settle();
  return page;
}

test('no override: field stays empty with the default as placeholder; save omits relay', async () => {
  const page = await loadPage(info());
  assert.equal(page.el('relay').value, '');
  assert.equal(page.el('relay').placeholder, DEFAULT);

  page.el('ssid').value = 'home';
  page.el('pass').value = 'password1';
  const body = await page.save();
  assert.deepEqual(body, { ssid: 'home', pass: 'password1' });
});

test('override: field is pre-filled, and saving it unchanged omits relay', async () => {
  const page = await loadPage(info({ relayUrl: 'https://mine.relay.test', relayOverride: true }));
  assert.equal(page.el('relay').value, 'https://mine.relay.test');
  assert.equal(page.el('relay').placeholder, DEFAULT);

  page.el('ssid').value = 'home';
  const body = await page.save();
  assert.equal('relay' in body, false, `relay sent: ${JSON.stringify(body)}`);
});

test('override: an emptied field omits relay instead of sending ""', async () => {
  const page = await loadPage(info({ relayUrl: 'https://mine.relay.test', relayOverride: true }));
  page.el('ssid').value = 'home';
  page.el('relay').value = '';
  const body = await page.save();
  assert.equal('relay' in body, false, `relay sent: ${JSON.stringify(body)}`);
});

test('a changed relay is sent, and the reloaded override is then treated as unchanged', async () => {
  const page = await loadPage(info({ relayUrl: 'https://mine.relay.test', relayOverride: true }));
  page.el('ssid').value = 'home';
  page.el('relay').value = 'https://new.relay.test';
  page.info = info({ relayUrl: 'https://new.relay.test', relayOverride: true });
  assert.equal((await page.save()).relay, 'https://new.relay.test');

  await settle(); // the post-save /info refresh
  assert.equal(page.el('relay').value, 'https://new.relay.test');
  const again = await page.save();
  assert.equal('relay' in again, false, `relay re-sent: ${JSON.stringify(again)}`);
});

test('typing the default is sent, so the device can clear its override', async () => {
  const page = await loadPage(info({ relayUrl: 'https://mine.relay.test', relayOverride: true }));
  page.el('ssid').value = 'home';
  page.el('relay').value = DEFAULT;
  assert.equal((await page.save()).relay, DEFAULT);
});
