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

/** @returns {{deviceId:string, deviceToken:string, relayUrl:string, kid?:string, name?:string}} */
export function parsePairingBlob(raw) {
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
  const localhost = url.hostname === 'localhost' || url.hostname === '127.0.0.1';
  if (url.protocol !== 'https:' && !localhost) {
    throw new Error('relayUrl must be https (localhost is allowed for development).');
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
