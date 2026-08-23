/**
 * Command orchestration: the one place where the ordering that the security
 * model depends on is enforced, for every action, without exception.
 *
 *   1. WebAuthn user-presence gate (local; nothing is transmitted)
 *   2. increment-and-PERSIST the per-kid counter, then read it back
 *   3. build the canonical signing string and sign it
 *   4. POST the envelope, splicing in the canonical args bytes verbatim
 *
 * Every UI affordance goes through `run()` — there is no side door.
 */

import { uuid4, nowSec } from './util.js';
import { buildEnvelope } from './signing.js';
import { signCommand } from './crypto.js';
import { nextCounter, bumpCounterTo, counterKey } from './store.js';
import { requirePresence } from './webauthn.js';
import { sendCommand, RelayError } from './api.js';

/** Action argument bounds straight from docs/protocol.md §Actions. */
export const LIMITS = {
  typeMaxChars: 256,
  keysMaxChords: 32,
  powerTapMs: [50, 1000],
  powerHoldMs: [3000, 12000],
};

export const DEFAULTS = {
  power_tap: 200,
  power_hold: 6000,
};

function validate(act, args) {
  switch (act) {
    case 'ping':
    case 'status':
    case 'wake':
      return {};
    case 'type': {
      const text = String(args?.text ?? '');
      if (!text) throw new Error('Nothing to type.');
      if (text.length > LIMITS.typeMaxChars) {
        throw new Error(`Text is ${text.length} characters; the limit is ${LIMITS.typeMaxChars}.`);
      }
      return { text, enter: !!args.enter };
    }
    case 'keys': {
      const seq = args?.seq;
      if (!Array.isArray(seq) || !seq.length) throw new Error('No key chords to send.');
      if (seq.length > LIMITS.keysMaxChords) {
        throw new Error(`${seq.length} chords; the limit is ${LIMITS.keysMaxChords}.`);
      }
      for (const chord of seq) {
        if (!Array.isArray(chord) || !chord.length) throw new Error('A chord is empty.');
        for (const k of chord) {
          if (typeof k !== 'string' || !/^[A-Z0-9_]+$/.test(k)) {
            throw new Error(`Invalid key name: ${k}`);
          }
        }
      }
      return { seq };
    }
    case 'power_tap':
    case 'power_hold': {
      const [lo, hi] = act === 'power_tap' ? LIMITS.powerTapMs : LIMITS.powerHoldMs;
      const ms = Math.round(Number(args?.ms ?? DEFAULTS[act]));
      if (!Number.isFinite(ms) || ms < lo || ms > hi) {
        throw new Error(`${act} duration must be ${lo}–${hi} ms.`);
      }
      return { ms };
    }
    default:
      throw new Error(`Unknown action: ${act}`);
  }
}

/**
 * Sign and send one command.
 *
 * @param {object} device  {deviceId, deviceToken, relayUrl, kid}
 * @param {string} act
 * @param {object} [args]
 * @param {{skipPresence?:boolean}} [opts] skipPresence is for the automatic
 *        `status`/`ping` refresh only — never for anything the user triggers.
 * @returns {Promise<{ok:boolean, res?:object, err?:string, envelope:object}>}
 */
export async function run(device, act, args = {}, opts = {}) {
  if (!device) throw new Error('No device paired.');
  // The slot THIS device filed our public key under (protocol §kid, p1…p4).
  const kid = device.kid || 'p1';
  const ctrKey = counterKey(device.deviceId, kid);
  const canonicalisedArgs = validate(act, args);

  if (!opts.skipPresence) await requirePresence();

  // Persist BEFORE signing: a crash here can only burn a counter value, and
  // the device's high-water mark makes reuse impossible either way.
  const ctr = await nextCounter(ctrKey);

  const command = {
    dev: device.deviceId,
    id: uuid4(),
    ts: nowSec(),
    ctr,
    act,
    args: canonicalisedArgs,
  };

  const sig = await signCommand(command);
  const envelope = buildEnvelope(command, kid, sig);

  const response = await sendCommand(device, envelope);

  // The device is ahead of us (restored backup, reinstalled app): catch up so
  // the next command is accepted instead of failing the same way.
  if (response?.ok === false && response.err === 'replay') {
    const deviceCtr = Number(response.res?.lastCtr);
    if (Number.isFinite(deviceCtr)) await bumpCounterTo(ctrKey, deviceCtr);
  }

  return { ...response, envelope };
}

/** Convenience wrappers used by the UI. */
export const wake = (device) => run(device, 'wake', {});
export const ping = (device) => run(device, 'ping', {}, { skipPresence: true });
export const status = (device) => run(device, 'status', {}, { skipPresence: true });
export const typeText = (device, text, enter = true) => run(device, 'type', { text, enter });
export const sendKeys = (device, seq) => run(device, 'keys', { seq });
export const powerTap = (device, ms = DEFAULTS.power_tap) => run(device, 'power_tap', { ms });
export const powerHold = (device, ms = DEFAULTS.power_hold) => run(device, 'power_hold', { ms });

export { RelayError };
