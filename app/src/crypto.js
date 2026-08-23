/**
 * Ed25519 operator key management.
 *
 * Two backends, chosen by feature detection at first key generation:
 *
 *   'webcrypto' — crypto.subtle.generateKey({name:'Ed25519'}, false, …).
 *                 The private key is a non-extractable CryptoKey handed to
 *                 IndexedDB by structured clone; the raw bytes never exist in
 *                 JS memory and cannot be exfiltrated by XSS or a backup.
 *                 This is the preferred path (docs/security.md, "phone").
 *
 *   'noble'     — vendored @noble/ed25519, with the 32-byte seed stored as raw
 *                 bytes in IndexedDB. WEAKER: anything with script access to
 *                 this origin can read and copy the signing key. Used only
 *                 where WebCrypto has no Ed25519 (Safari < 17, older Chrome).
 *
 * Both paths sign the exact bytes produced by src/signing.js.
 */

import { bytesToB64url, bytesToHex } from './util.js';
import { getKeyRecord, putKeyRecord } from './store.js';
import { signingBytes } from './signing.js';

/**
 * This phone has exactly ONE operator key, stored under this fixed record id.
 *
 * Do not confuse it with the protocol's `kid`: that is a SLOT NUMBER ON A
 * DEVICE (p1…p4, "the device stores up to 4 operator public keys"), assigned
 * by each device independently at pairing time. The same phone key can be p1
 * on one device and p3 on another, so the kid lives on the device record
 * (`device.kid`), not here.
 */
const LOCAL_KEY_ID = 'phone';

const subtle = globalThis.crypto?.subtle;
let noblePromise = null;

const loadNoble = () => (noblePromise ??= import('../vendor/noble-ed25519.js'));

/** Does this browser have Ed25519 in WebCrypto? Cached; probes for real. */
let webCryptoSupport = null;
export async function hasWebCryptoEd25519() {
  if (webCryptoSupport !== null) return webCryptoSupport;
  if (!subtle) return (webCryptoSupport = false);
  try {
    const kp = await subtle.generateKey({ name: 'Ed25519' }, false, ['sign', 'verify']);
    // Some engines expose the algorithm but reject signing; probe end-to-end.
    await subtle.sign({ name: 'Ed25519' }, kp.privateKey, new Uint8Array([1, 2, 3]));
    webCryptoSupport = true;
  } catch {
    webCryptoSupport = false;
  }
  return webCryptoSupport;
}

/**
 * Get this phone's operator key, creating it on first use.
 * @returns {Promise<{mode:string, publicKeyRaw:Uint8Array,
 *                    publicKeyB64url:string, createdAt:number}>}
 */
export async function ensureKey() {
  const existing = await getKeyRecord(LOCAL_KEY_ID);
  if (existing) return describe(existing);

  if (await hasWebCryptoEd25519()) {
    const kp = await subtle.generateKey({ name: 'Ed25519' }, false, ['sign', 'verify']);
    const raw = new Uint8Array(await subtle.exportKey('raw', kp.publicKey));
    const rec = {
      kid: LOCAL_KEY_ID,
      mode: 'webcrypto',
      privateKey: kp.privateKey, // non-extractable CryptoKey, structured-cloned
      publicKeyRaw: raw,
      createdAt: Date.now(),
    };
    await putKeyRecord(rec);
    return describe(rec);
  }

  const ed = await loadNoble();
  const seed = crypto.getRandomValues(new Uint8Array(32));
  const pub = await ed.getPublicKeyAsync(seed);
  const rec = {
    kid: LOCAL_KEY_ID,
    mode: 'noble',
    seed, // WEAKER: extractable key material at rest — see module header
    publicKeyRaw: new Uint8Array(pub),
    createdAt: Date.now(),
  };
  await putKeyRecord(rec);
  return describe(rec);
}

function describe(rec) {
  const raw = new Uint8Array(rec.publicKeyRaw);
  return {
    mode: rec.mode,
    publicKeyRaw: raw,
    publicKeyB64url: bytesToB64url(raw),
    publicKeyHex: bytesToHex(raw),
    createdAt: rec.createdAt,
    extractable: rec.mode !== 'webcrypto',
  };
}

/** Describe this phone's key without creating one. */
export async function getKeyInfo() {
  const rec = await getKeyRecord(LOCAL_KEY_ID);
  return rec ? describe(rec) : null;
}

/**
 * Sign a command's canonical signing string with this phone's key.
 * @param {object} command {dev,id,ts,ctr,act,args}
 * @returns {Promise<string>} base64url signature (64 bytes)
 */
export async function signCommand(command) {
  const rec = await getKeyRecord(LOCAL_KEY_ID);
  if (!rec) throw new Error('no operator key on this phone — pair a device first');
  const msg = signingBytes(command);

  if (rec.mode === 'webcrypto') {
    const sig = await subtle.sign({ name: 'Ed25519' }, rec.privateKey, msg);
    return bytesToB64url(new Uint8Array(sig));
  }
  const ed = await loadNoble();
  return bytesToB64url(await ed.signAsync(msg, rec.seed));
}

/** Verify locally — used by the self-test in Settings, not on the hot path. */
export async function verifyCommand(command, sigB64url, publicKeyRaw) {
  const { b64urlToBytes } = await import('./util.js');
  const sig = b64urlToBytes(sigB64url);
  const msg = signingBytes(command);
  if (await hasWebCryptoEd25519()) {
    const key = await subtle.importKey('raw', publicKeyRaw, { name: 'Ed25519' }, false, ['verify']);
    return subtle.verify({ name: 'Ed25519' }, key, sig, msg);
  }
  const ed = await loadNoble();
  return ed.verifyAsync(sig, msg, publicKeyRaw);
}
