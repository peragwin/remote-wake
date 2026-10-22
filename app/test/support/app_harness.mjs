/**
 * Runs the real PWA controller (src/app.js) in Node against fakes at its
 * platform boundaries only:
 *   - document: a small fake DOM built from index.html's elements;
 *   - indexedDB: an in-memory fake, so the real store.js runs;
 *   - fetch: a fake relay that answers signed envelopes (real api.js,
 *     commands.js, signing.js and WebCrypto Ed25519 produce them);
 *   - navigator.credentials / PublicKeyCredential: a scripted platform
 *     authenticator, so the real webauthn.js runs;
 *   - ui.js (toasts, sheets) and the presence poller: recorded stand-ins.
 *
 * Every harness imports a fresh module graph (app.js and the stateful modules
 * get a `?h=N` query), so tests do not share store or controller state.
 *
 * Waiting: the fakes resolve in microtasks, so `settle()` (a few event-loop
 * turns) drains them; anything slower (WebCrypto) is awaited through the
 * event handler's own promise or `until()`, which is bounded by wall time.
 */
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { registerHooks } from 'node:module';

const SRC = new URL('../../src/', import.meta.url).href;
const APP = `${SRC}app.js`;
const FAKE_UI = new URL('./fake_ui.mjs', import.meta.url).href;
const FAKE_API = new URL('./fake_api.mjs', import.meta.url).href;
const PER_GRAPH = new Set(['./store.js', './commands.js', './crypto.js', './webauthn.js']);

let hooked = false;
function installHooks() {
  if (hooked) return;
  hooked = true;
  registerHooks({
    resolve(specifier, context, next) {
      const parent = context.parentURL || '';
      const q = parent.startsWith(SRC) ? parent.match(/\?h=\d+$/)?.[0] : undefined;
      if (q) {
        if (parent.startsWith(APP) && specifier === './ui.js') return { url: FAKE_UI, shortCircuit: true };
        if (parent.startsWith(APP) && specifier === './api.js') return { url: FAKE_API, shortCircuit: true };
        if (PER_GRAPH.has(specifier)) {
          const r = next(specifier, context);
          return { ...r, url: r.url + q };
        }
      }
      return next(specifier, context);
    },
  });
}

let graph = 0;
/** An importer for one new module graph: `(name) => import('src/<name>?h=N')`. */
export function freshGraph() {
  installHooks();
  graph += 1;
  const n = graph;
  return (name) => import(`${SRC}${name}?h=${n}`);
}
export const freshModule = (name) => freshGraph()(name);

/* ─────────────────────────────────────────────────────────── fake DOM ── */

class ClassList {
  constructor() {
    this.set = new Set();
  }
  add(...c) {
    c.forEach((x) => this.set.add(x));
  }
  remove(...c) {
    c.forEach((x) => this.set.delete(x));
  }
  contains(c) {
    return this.set.has(c);
  }
  toggle(c, force) {
    const on = force === undefined ? !this.set.has(c) : !!force;
    if (on) this.set.add(c);
    else this.set.delete(c);
    return on;
  }
}

export class FakeNode {
  constructor(tag) {
    this.tagName = String(tag).toUpperCase();
    this.children = [];
    this.parent = null;
    this.listeners = {};
    this.attrs = {};
    this.classList = new ClassList();
    this._text = '';
    this.id = '';
    this.hidden = false;
    this.disabled = false;
    this.value = '';
    this.type = '';
    this.placeholder = '';
    this.checked = false;
    this.innerHTML = '';
    this.style = {};
  }
  set className(v) {
    this.classList.set = new Set(String(v).split(/\s+/).filter(Boolean));
  }
  get className() {
    return [...this.classList.set].join(' ');
  }
  get textContent() {
    return this._text + this.children.map((c) => c.textContent).join('');
  }
  set textContent(v) {
    this._text = String(v ?? '');
    this.children = [];
  }
  append(...nodes) {
    for (const n of nodes) {
      const node = n instanceof FakeNode ? n : textNode(String(n));
      node.parent = this;
      this.children.push(node);
    }
  }
  replaceChildren(...nodes) {
    this.children = [];
    this._text = '';
    this.append(...nodes);
  }
  addEventListener(type, fn) {
    (this.listeners[type] ??= []).push(fn);
  }
  removeEventListener(type, fn) {
    this.listeners[type] = (this.listeners[type] ?? []).filter((f) => f !== fn);
  }
  setAttribute(k, v) {
    const val = String(v);
    this.attrs[k] = val;
    if (k === 'id') this.id = val;
    else if (k === 'class') this.className = val;
    else if (k === 'hidden') this.hidden = true;
    else if (k === 'disabled') this.disabled = true;
    else if (k === 'checked') this.checked = true;
    else if (k === 'type' || k === 'placeholder' || k === 'value') this[k] = val;
  }
  getAttribute(k) {
    return this.attrs[k] ?? null;
  }
  focus() {}
  blur() {}
  *descendants() {
    for (const c of this.children) {
      yield c;
      yield* c.descendants();
    }
  }
  querySelectorAll(sel) {
    let scope = [this];
    for (const part of sel.trim().split(/\s+/)) {
      const found = new Set();
      for (const s of scope) for (const d of s.descendants()) if (matches(d, part)) found.add(d);
      scope = [...found];
    }
    return scope;
  }
  querySelector(sel) {
    return this.querySelectorAll(sel)[0] ?? null;
  }
}

function matches(node, part) {
  if (part.startsWith('#')) return node.id === part.slice(1);
  if (part.startsWith('.')) return node.classList.contains(part.slice(1));
  return node.tagName === part.toUpperCase();
}

function textNode(text) {
  const n = new FakeNode('#text');
  n._text = text;
  return n;
}

const html = readFileSync(new URL('../../index.html', import.meta.url), 'utf8');

function buildDocument() {
  const root = new FakeNode('html');
  for (const m of html.matchAll(/<([a-zA-Z][\w-]*)\b([^>]*)>/g)) {
    const node = new FakeNode(m[1]);
    for (const a of m[2].matchAll(/([^\s=/]+)(?:=(?:"([^"]*)"|'([^']*)'|([^\s>]+)))?/g)) {
      node.setAttribute(a[1], a[2] ?? a[3] ?? a[4] ?? '');
    }
    root.append(node); // flat: only dynamically appended nodes nest
  }
  const listeners = {};
  return {
    root,
    hidden: false,
    querySelector: (s) => root.querySelector(s),
    querySelectorAll: (s) => root.querySelectorAll(s),
    getElementById: (id) => root.querySelector(`#${id}`),
    createElement: (tag) => new FakeNode(tag),
    createTextNode: (t) => textNode(t),
    addEventListener: (t, fn) => (listeners[t] ??= []).push(fn),
    removeEventListener() {},
  };
}

/* ──────────────────────────────────────────────────────── fake IndexedDB ── */

const clone = (v) => (v === undefined ? undefined : structuredClone(v));

/**
 * @param {Record<string, Map<any, any>>} data  store name → (key → record);
 *        shared with the caller so a later harness can reopen the same data.
 */
function fakeIndexedDB(data) {
  const keyPaths = {};
  const db = {
    objectStoreNames: { contains: (n) => n in keyPaths },
    createObjectStore(name, { keyPath }) {
      keyPaths[name] = keyPath;
      data[name] ??= new Map();
    },
    transaction(names, _mode) {
      let pending = 0;
      let done = false;
      const t = { oncomplete: null, onerror: null, onabort: null, error: null };
      const maybeComplete = () =>
        queueMicrotask(() => {
          if (pending === 0 && !done) {
            done = true;
            t.oncomplete?.();
          }
        });
      const request = (fn) => {
        const req = { result: undefined, onsuccess: null, onerror: null };
        pending += 1;
        queueMicrotask(() => {
          req.result = fn();
          req.onsuccess?.();
          pending -= 1;
          maybeComplete();
        });
        return req;
      };
      t.objectStore = (name) => {
        assert.ok([names].flat().includes(name), `store ${name} not in transaction`);
        const s = data[name];
        return {
          get: (k) => request(() => clone(s.get(k))),
          getAll: () => request(() => [...s.values()].map(clone)),
          put: (v) => request(() => void s.set(v[keyPaths[name]], clone(v))),
          delete: (k) => request(() => void s.delete(k)),
          clear: () => request(() => void s.clear()),
        };
      };
      maybeComplete();
      return t;
    },
  };
  return {
    open() {
      const req = { result: db, onupgradeneeded: null, onsuccess: null, onerror: null };
      queueMicrotask(() => {
        req.onupgradeneeded?.();
        req.onsuccess?.();
      });
      return req;
    },
  };
}

/** Records keyed by their store's keyPath, for seeding. */
export function seedData({ devices = [], settings = {}, counters = {} } = {}) {
  return {
    devices: new Map(devices.map((d) => [d.deviceId, structuredClone(d)])),
    settings: new Map(Object.entries(settings).map(([k, v]) => [k, { k, v }])),
    counters: new Map(Object.entries(counters).map(([kid, ctr]) => [kid, { kid, ctr }])),
    keys: new Map(),
  };
}

/* ────────────────────────────────────────────────────────────── harness ── */

export const settle = async (turns = 5) => {
  for (let i = 0; i < turns; i++) await new Promise((r) => setImmediate(r));
};

export async function until(cond, what, ms = 5000) {
  const deadline = Date.now() + ms;
  while (!cond()) {
    if (Date.now() > deadline) assert.fail(`timed out waiting for ${what}`);
    await new Promise((r) => setImmediate(r));
  }
}

/**
 * Install fresh globals over @p data and boot a fresh app.js.
 * @param {object} o
 * @param {object} o.data        from seedData(); mutated in place by the app
 * @param {string} [o.appHostname]  location.hostname the app is served from
 * @param {(env:object)=>object} [o.relay]  device reply for a sent envelope
 * @param {(kind:string, opts:object)=>any} [o.answer]  dialog answers
 * @param {()=>Promise<any>} [o.assertion]  navigator.credentials.get behaviour
 */
export async function bootApp(o) {
  const h = installPlatform(o);
  const { document } = h;
  await freshModule('app.js');
  await until(() => !document.querySelector('#app').hidden || document.querySelector('#boot').innerHTML,
    'app boot');
  assert.equal(document.querySelector('#boot').innerHTML, '', 'boot failed');
  await settle();
  return h;
}

/** The platform globals alone (for driving modules other than app.js). */
export function installPlatform(o) {
  const h = {
    data: o.data,
    toasts: [],
    dialogs: [],
    sent: [], // envelopes the relay received
    credentialGets: 0,
    answer: o.answer ?? (() => false),
    relay: o.relay ?? (() => ({ ok: true, res: {} })),
    assertion: o.assertion ?? (async () => ({ id: 'ok' })),
  };
  globalThis.__rwHarness = h;

  const document = buildDocument();
  h.document = document;
  const define = (name, value) =>
    Object.defineProperty(globalThis, name, { value, configurable: true, writable: true });
  define('document', document);
  define('Node', FakeNode);
  define('window', { scrollTo() {} });
  define('history', { replaceState() {} });
  define('location', {
    search: '',
    hostname: o.appHostname ?? 'remote-wake.example.com',
    pathname: '/',
    protocol: 'https:',
  });
  define('indexedDB', fakeIndexedDB(o.data));
  define('PublicKeyCredential', { isUserVerifyingPlatformAuthenticatorAvailable: async () => true });
  define('navigator', {
    vibrate() {},
    credentials: {
      create: async () => null,
      get: async () => {
        h.credentialGets += 1;
        return h.assertion();
      },
    },
  });
  define('fetch', async (url, init) => {
    const env = JSON.parse(init.body);
    h.sent.push({ url: String(url), env });
    const reply = { v: 1, id: env.id, ...h.relay(env) };
    return { status: 200, ok: true, json: async () => reply };
  });

  h.$ = (sel) => {
    const n = document.querySelector(sel);
    assert.ok(n, `no element ${sel}`);
    return n;
  };
  h.$$ = (sel) => document.querySelectorAll(sel);
  /** Run every listener for @p type and wait for what they return. */
  h.fire = async (node, type, init = {}) => {
    const ev = { type, target: node, currentTarget: node, preventDefault() {}, stopPropagation() {}, ...init };
    await Promise.all((node.listeners[type] ?? []).map((fn) => fn(ev)));
    await settle();
  };
  /** A user click: browsers do not deliver clicks to disabled controls. */
  h.click = async (node) => {
    assert.equal(node.disabled, false, `clicked a disabled control: ${node.id || node.textContent}`);
    await h.fire(node, 'click');
  };
  h.sentActs = () => h.sent.map((s) => s.env.act);
  /** Every place a string can live in the fake DOM. */
  h.domContains = (secret) => {
    for (const n of [document.root, ...document.root.descendants()]) {
      const fields = [n._text, n.value, n.placeholder, n.innerHTML, ...Object.values(n.attrs)];
      if (fields.some((f) => typeof f === 'string' && f.includes(secret))) return true;
    }
    return false;
  };
  return h;
}
