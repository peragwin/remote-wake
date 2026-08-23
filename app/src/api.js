/**
 * Relay transport (docs/protocol.md §Transport, "Phone ↔ relay").
 *
 *   POST {relayUrl}/v1/send/{deviceId}      Authorization: Bearer <deviceToken>
 *        body = command envelope (≤ 4 KiB), reply = the device's response
 *        envelope, or 504 {"err":"device_offline"} after the relay's 10 s
 *        long-poll.
 *   GET  {relayUrl}/v1/presence/{deviceId}  → { online, since }
 *
 * The bearer token gates relay access only and carries no command authority —
 * authority lives entirely in the Ed25519 signature.
 */

import { serializeEnvelope } from './signing.js';

/** Relay-enforced body cap. We check locally so we can give a better message. */
export const MAX_BODY_BYTES = 4096;
const SEND_TIMEOUT_MS = 15_000; // relay long-polls up to 10 s
const PRESENCE_TIMEOUT_MS = 8_000;

export class RelayError extends Error {
  constructor(code, message, extra = {}) {
    super(message);
    this.name = 'RelayError';
    this.code = code;
    Object.assign(this, extra);
  }
}

function base(relayUrl) {
  return String(relayUrl).replace(/\/+$/, '');
}

async function withTimeout(ms, fn) {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), ms);
  try {
    return await fn(ctl.signal);
  } catch (err) {
    if (err?.name === 'AbortError') throw new RelayError('timeout', 'Relay did not answer in time');
    throw new RelayError('network', 'Cannot reach the relay — check your connection');
  } finally {
    clearTimeout(timer);
  }
}

/**
 * Send a signed command envelope and return the device's response envelope.
 * @param {{relayUrl:string, deviceId:string, deviceToken:string}} device
 * @param {object} envelope signed command envelope
 * @returns {Promise<{v:number,id:string,ok:boolean,res?:object,err?:string}>}
 */
export async function sendCommand(device, envelope) {
  const body = serializeEnvelope(envelope);
  const size = new TextEncoder().encode(body).length;
  if (size > MAX_BODY_BYTES) {
    throw new RelayError('too_large', `Command is ${size} bytes; the relay caps bodies at 4 KiB`);
  }

  const res = await withTimeout(SEND_TIMEOUT_MS, (signal) =>
    fetch(`${base(device.relayUrl)}/v1/send/${encodeURIComponent(device.deviceId)}`, {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        authorization: `Bearer ${device.deviceToken}`,
      },
      body,
      signal,
      cache: 'no-store',
      mode: 'cors',
    })
  );

  let payload = null;
  try {
    payload = await res.json();
  } catch {
    /* non-JSON body */
  }

  if (res.status === 504 || payload?.err === 'device_offline') {
    throw new RelayError('device_offline', 'Device is offline', { status: res.status });
  }
  if (res.status === 401 || res.status === 403) {
    throw new RelayError('unauthorized', 'Relay rejected the device token — re-pair the device', {
      status: res.status,
    });
  }
  if (res.status === 429) {
    throw new RelayError('rate_limited', 'Too many commands — slow down', { status: res.status });
  }
  if (res.status === 413) {
    throw new RelayError('too_large', 'Relay rejected the command as too large', { status: res.status });
  }
  if (!res.ok) {
    throw new RelayError(payload?.err || 'relay_error', `Relay error ${res.status}`, {
      status: res.status,
    });
  }
  if (!payload || typeof payload !== 'object') {
    throw new RelayError('bad_response', 'Relay returned an unreadable response');
  }
  if (payload.id && envelope.id && payload.id !== envelope.id) {
    throw new RelayError('id_mismatch', 'Response did not match the command that was sent');
  }
  return payload;
}

/** @returns {Promise<{online:boolean, since?:number}>} */
export async function getPresence(device) {
  const res = await withTimeout(PRESENCE_TIMEOUT_MS, (signal) =>
    fetch(`${base(device.relayUrl)}/v1/presence/${encodeURIComponent(device.deviceId)}`, {
      headers: { authorization: `Bearer ${device.deviceToken}` },
      signal,
      cache: 'no-store',
      mode: 'cors',
    })
  );
  if (res.status === 401 || res.status === 403) {
    throw new RelayError('unauthorized', 'Relay rejected the device token');
  }
  if (!res.ok) throw new RelayError('relay_error', `Presence check failed (${res.status})`);
  const json = await res.json().catch(() => null);
  if (!json || typeof json.online !== 'boolean') {
    throw new RelayError('bad_response', 'Relay returned an unreadable presence payload');
  }
  return json;
}

/**
 * Presence poller: every 15 s while the page is visible, paused when hidden,
 * and refreshed immediately on becoming visible again.
 */
export function createPresencePoller({ intervalMs = 15_000, getDevice, onUpdate, onError }) {
  let timer = null;
  let stopped = true;
  let inflight = false;

  async function tick() {
    if (inflight) return;
    const device = await getDevice();
    if (!device) {
      onUpdate?.(null);
      return;
    }
    inflight = true;
    try {
      onUpdate?.(await getPresence(device));
    } catch (err) {
      onError?.(err);
      onUpdate?.({ online: false, error: err.code || 'error' });
    } finally {
      inflight = false;
    }
  }

  function schedule() {
    clearTimeout(timer);
    if (stopped || document.hidden) return;
    timer = setTimeout(async () => {
      await tick();
      schedule();
    }, intervalMs);
  }

  function onVisibility() {
    if (document.hidden) {
      clearTimeout(timer);
    } else if (!stopped) {
      tick().then(schedule);
    }
  }

  return {
    start() {
      if (!stopped) return;
      stopped = false;
      document.addEventListener('visibilitychange', onVisibility);
      tick().then(schedule);
    },
    stop() {
      stopped = true;
      clearTimeout(timer);
      document.removeEventListener('visibilitychange', onVisibility);
    },
    refresh: () => tick().then(schedule),
  };
}

/**
 * Human text for device-side `err` codes (protocol §Response envelope).
 * The device includes its own time in `stale` errors so we can name the drift.
 */
export function errorMessage(err, res) {
  switch (err) {
    case 'stale': {
      const drift =
        res?.deviceTime && Number.isFinite(res.deviceTime)
          ? ` (device clock is ${Math.abs(Math.floor(Date.now() / 1000) - res.deviceTime)}s off)`
          : '';
      return `Rejected: device clock is out of sync${drift}. It resyncs over SNTP — retry shortly.`;
    }
    case 'replay':
      return 'Rejected as a replay — the device has already seen this counter. Resyncing.';
    case 'sig':
      return 'Signature rejected. This phone’s key is not registered on the device — re-pair.';
    case 'unknown_act':
      return 'The device firmware does not know this action.';
    case 'busy':
      return 'Device is busy with another command. Try again in a moment.';
    case 'usb_down':
      return 'USB is not connected to the PC — keystrokes cannot be delivered.';
    case 'device_offline':
      return 'Device is offline — it is not connected to the relay.';
    case 'unauthorized':
      return 'Relay rejected the device token — re-pair the device.';
    case 'rate_limited':
      return 'Rate limited. Slow down.';
    case 'timeout':
      return 'The relay did not answer in time.';
    case 'network':
      return 'Cannot reach the relay — check your connection.';
    case 'too_large':
      return 'Command is too large (the relay caps bodies at 4 KiB).';
    default:
      return err ? `Device rejected the command: ${err}` : 'Command failed.';
  }
}
