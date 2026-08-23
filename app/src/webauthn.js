/**
 * WebAuthn user-presence gate.
 *
 * This is a LOCAL gate only. The assertion is never sent anywhere — it is not
 * part of the protocol and the device never sees it. Its job is exactly the one
 * docs/security.md gives it: a stolen, unlocked phone should still not be able
 * to type your password into your PC or hold the power button, without a fresh
 * biometric/PIN check.
 *
 * Registration uses a platform authenticator with userVerification:'required'.
 * If the platform has none, setup skips gracefully and the gate stays off
 * (surfaced in Settings so the user knows what protection they do not have).
 */

import { bytesToB64url, b64urlToBytes } from './util.js';
import { getSetting, setSetting } from './store.js';

const RP_NAME = 'remote-wake';
const USER_NAME = 'operator';

export function isSupported() {
  return typeof PublicKeyCredential !== 'undefined' && !!navigator.credentials?.create;
}

export async function hasPlatformAuthenticator() {
  if (!isSupported()) return false;
  try {
    return await PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable();
  } catch {
    return false;
  }
}

/** True when a credential is registered AND the gate is switched on. */
export async function isGateActive() {
  const [enabled, credId] = await Promise.all([
    getSetting('webauthnEnabled'),
    getSetting('webauthnCredentialId'),
  ]);
  return !!(enabled && credId);
}

/**
 * Create the platform passkey. Resolves to a status string rather than
 * throwing for the expected "can't do it here" cases.
 * @returns {Promise<'registered'|'unsupported'|'declined'>}
 */
export async function register(displayName = 'remote-wake operator') {
  if (!(await hasPlatformAuthenticator())) return 'unsupported';

  const userId = crypto.getRandomValues(new Uint8Array(16));
  try {
    const cred = await navigator.credentials.create({
      publicKey: {
        challenge: crypto.getRandomValues(new Uint8Array(32)),
        rp: { name: RP_NAME, id: location.hostname },
        user: { id: userId, name: USER_NAME, displayName },
        pubKeyCredParams: [
          { type: 'public-key', alg: -7 }, // ES256
          { type: 'public-key', alg: -257 }, // RS256
        ],
        authenticatorSelection: {
          authenticatorAttachment: 'platform',
          userVerification: 'required',
          residentKey: 'discouraged',
          requireResidentKey: false,
        },
        attestation: 'none',
        timeout: 60_000,
      },
    });
    if (!cred) return 'declined';
    await setSetting('webauthnCredentialId', bytesToB64url(new Uint8Array(cred.rawId)));
    await setSetting('webauthnEnabled', true);
    return 'registered';
  } catch (err) {
    if (err?.name === 'NotAllowedError' || err?.name === 'AbortError') return 'declined';
    if (err?.name === 'NotSupportedError' || err?.name === 'SecurityError') return 'unsupported';
    throw err;
  }
}

/**
 * Require a fresh assertion before a signing action.
 * @returns {Promise<boolean>} true if the gate passed or is not configured.
 * @throws {Error} 'user-presence-declined' when the user cancels or fails.
 */
export async function requirePresence() {
  if (!(await isGateActive())) return true;
  const credId = await getSetting('webauthnCredentialId');

  try {
    const assertion = await navigator.credentials.get({
      publicKey: {
        challenge: crypto.getRandomValues(new Uint8Array(32)),
        rpId: location.hostname,
        allowCredentials: [{ type: 'public-key', id: b64urlToBytes(credId) }],
        userVerification: 'required',
        timeout: 60_000,
      },
    });
    if (!assertion) throw new Error('user-presence-declined');
    return true; // assertion is deliberately discarded — never leaves the phone
  } catch (err) {
    if (err?.name === 'NotAllowedError' || err?.name === 'AbortError') {
      throw new Error('user-presence-declined');
    }
    throw err;
  }
}

/** Turn the gate off (keeps the credential id so it can be re-enabled). */
export const disableGate = () => setSetting('webauthnEnabled', false);

export async function enableGate() {
  if (await getSetting('webauthnCredentialId')) {
    await setSetting('webauthnEnabled', true);
    return 'registered';
  }
  return register();
}

export async function forgetCredential() {
  await setSetting('webauthnCredentialId', null);
  await setSetting('webauthnEnabled', false);
}
