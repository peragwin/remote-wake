/**
 * Parsing and validating the pairing blob the firmware's setup page hands back
 * (docs/protocol.md §Pairing step 3):
 *
 *   { "deviceId": …, "deviceToken": …, "relayUrl": … }
 *
 * Tolerant about how the text arrives (stray whitespace, a wrapping code
 * fence, a QR payload), strict about what it contains — a malformed deviceId
 * or a plaintext relay URL is a setup mistake worth catching here rather than
 * discovering as a silent failure later.
 *
 * Dependency-free so it can be unit tested outside a browser.
 */

const LOOPBACK = new Set(['localhost', '127.0.0.1', '[::1]']);

/** True for a loopback hostname as `URL#hostname` / `location.hostname` spell it. */
export function isLoopbackHost(hostname) {
  const h = String(hostname ?? '').toLowerCase();
  return LOOPBACK.has(h) || h === '::1';
}

/**
 * @param {string} raw
 * @param {{appHostname?: string}} [opts] the hostname this app is served from
 *        (defaults to location.hostname). An http relay is accepted only when
 *        both it and the app are on loopback: a pairing blob must not be able
 *        to point a deployed app at a plaintext relay on the phone itself.
 * @returns {{deviceId:string, deviceToken:string, relayUrl:string, kid?:string, name?:string}}
 */
export function parsePairingBlob(raw, opts = {}) {
  const appHostname = opts.appHostname ?? globalThis.location?.hostname ?? '';
  const text = String(raw ?? '')
    .trim()
    .replace(/^```(?:json)?/i, '')
    .replace(/```$/, '')
    .trim();

  if (!text) throw new Error('Paste the JSON blob from the device setup page.');

  let obj;
  try {
    obj = JSON.parse(text);
  } catch {
    throw new Error('That is not valid JSON. Copy the whole blob, including the braces.');
  }
  if (!obj || typeof obj !== 'object' || Array.isArray(obj)) {
    throw new Error('Expected a JSON object.');
  }

  const deviceId = String(obj.deviceId ?? '').trim();
  const deviceToken = String(obj.deviceToken ?? '').trim();
  const relayUrl = String(obj.relayUrl ?? '').trim();

  // deviceId: 8 random bytes, lowercase hex.
  if (!/^[0-9a-f]{16}$/.test(deviceId)) {
    throw new Error('deviceId must be 16 lowercase hex characters.');
  }
  // deviceToken: 32 bytes, base64url, no padding → exactly 43 characters.
  if (!/^[A-Za-z0-9_-]{43}$/.test(deviceToken)) {
    throw new Error('deviceToken must be 32 bytes of base64url (43 characters).');
  }

  let url;
  try {
    url = new URL(relayUrl);
  } catch {
    throw new Error('relayUrl is not a valid URL.');
  }
  if (url.protocol !== 'https:') {
    const devHttp =
      url.protocol === 'http:' && isLoopbackHost(url.hostname) && isLoopbackHost(appHostname);
    if (!devHttp) {
      throw new Error(
        'relayUrl must be https (an http localhost relay is allowed only while the app itself runs on localhost).'
      );
    }
  }

  const result = {
    deviceId,
    deviceToken,
    // Normalised: no trailing slash, no query or fragment — api.js appends
    // /v1/send/<id> and /v1/presence/<id> directly.
    relayUrl: (url.origin + url.pathname).replace(/\/+$/, ''),
  };
  const name = String(obj.name ?? '').trim();
  if (name) result.name = name.slice(0, 40);

  // Optional: the operator slot the device filed our public key under.
  // protocol v1's /pair reply does not include it (the device uses p1, then the
  // next free slot), so the app defaults to p1 and lets the user correct it in
  // Settings. Accepted here so a firmware that does report it just works.
  if (obj.kid !== undefined) {
    const kid = String(obj.kid).trim();
    if (!/^p[1-4]$/.test(kid)) throw new Error('kid must be one of p1…p4.');
    result.kid = kid;
  }
  return result;
}

/** What the user is shown for a relay: host, plus any base path. */
export function relayLabel(relayUrl) {
  try {
    const url = new URL(relayUrl);
    const path = url.pathname.replace(/\/+$/, '');
    return url.host + path;
  } catch {
    return String(relayUrl ?? '');
  }
}

/**
 * How a parsed blob would change an already-stored device record (the store
 * merges `{...existing, ...blob}`). A replaced relay or token re-routes every
 * future command, so the UI must confirm it explicitly.
 * @returns {{isNew:boolean, relayChanged:boolean, tokenChanged:boolean}}
 */
export function pairingChange(existing, parsed) {
  if (!existing) return { isNew: true, relayChanged: false, tokenChanged: false };
  return {
    isNew: false,
    relayChanged: (existing.relayUrl ?? '') !== parsed.relayUrl,
    tokenChanged: (existing.deviceToken ?? '') !== parsed.deviceToken,
  };
}
