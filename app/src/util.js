/** Small isomorphic helpers: base64url, hex, uuid, time formatting, DOM. */

export function bytesToB64url(bytes) {
  const b = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  let s = '';
  for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]);
  return btoa(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

export function b64urlToBytes(str) {
  const s = String(str).replace(/-/g, '+').replace(/_/g, '/');
  const pad = s.length % 4 === 0 ? '' : '='.repeat(4 - (s.length % 4));
  const bin = atob(s + pad);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

export function isB64url(str, byteLen) {
  if (typeof str !== 'string' || !/^[A-Za-z0-9_-]+$/.test(str)) return false;
  try {
    return byteLen === undefined ? true : b64urlToBytes(str).length === byteLen;
  } catch {
    return false;
  }
}

export function bytesToHex(bytes) {
  let s = '';
  for (const b of bytes) s += b.toString(16).padStart(2, '0');
  return s;
}

export function uuid4() {
  if (globalThis.crypto?.randomUUID) return crypto.randomUUID();
  const b = crypto.getRandomValues(new Uint8Array(16));
  b[6] = (b[6] & 0x0f) | 0x40;
  b[8] = (b[8] & 0x3f) | 0x80;
  const h = bytesToHex(b);
  return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
}

/** Unix seconds, from the phone clock (protocol §ts). */
export const nowSec = () => Math.floor(Date.now() / 1000);

/** "3m ago", "just now", "2d ago" */
export function since(tsSec) {
  if (!tsSec) return '';
  const d = Math.max(0, nowSec() - tsSec);
  if (d < 10) return 'just now';
  if (d < 60) return `${d}s`;
  if (d < 3600) return `${Math.floor(d / 60)}m`;
  if (d < 86400) return `${Math.floor(d / 3600)}h`;
  return `${Math.floor(d / 86400)}d`;
}

export const $ = (sel, root = document) => root.querySelector(sel);
export const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];

export function el(tag, attrs = {}, ...children) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v === null || v === undefined || v === false) continue;
    if (k === 'class') node.className = v;
    else if (k === 'text') node.textContent = v;
    else if (k.startsWith('on') && typeof v === 'function') node.addEventListener(k.slice(2), v);
    else node.setAttribute(k, v === true ? '' : v);
  }
  for (const c of children.flat()) {
    if (c === null || c === undefined || c === false) continue;
    node.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return node;
}

export const prefersReducedMotion = () =>
  globalThis.matchMedia?.('(prefers-reduced-motion: reduce)').matches ?? false;

let hapticsEnabled = true;

/** Mirrors the Settings toggle so callers don't have to read IndexedDB. */
export const setHapticsEnabled = (on) => {
  hapticsEnabled = on !== false;
};

export function haptic(ms = 12) {
  if (!hapticsEnabled || prefersReducedMotion()) return;
  try {
    navigator.vibrate?.(ms);
  } catch {
    /* ignore */
  }
}
