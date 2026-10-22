/**
 * The controller's safety gates, exercised through the real src/app.js and its
 * DOM (see test/support/app_harness.mjs for what is faked and why):
 *   - firmware `acts` disables unlock typing and the key-combo builder/send;
 *   - turning the biometric gate off needs a passed presence check;
 *   - the stored unlock text stays out of the DOM until a presence check;
 *   - pairing shows the relay, and replacing a relay or token needs a
 *     confirmation that names the new relay.
 */
import test from 'node:test';
import assert from 'node:assert/strict';

import { bootApp, seedData } from './support/app_harness.mjs';

const A = {
  deviceId: 'a1b2c3d4e5f60718',
  deviceToken: 'e0yk9uPHzP93lnlGS9oTCkf9KCAzA9RAfZN1vn1VEK0',
  relayUrl: 'https://relay.example.com',
  name: 'Study PC',
  kid: 'p1',
  addedAt: 1,
};
const B = { ...A, deviceId: '0f1e2d3c4b5a6978', name: 'Media PC' };
const NO_KEYBOARD = ['ping', 'status', 'wake', 'power_tap', 'power_hold'];
const SECRET = 'S3cret-unlock-QZX';
const GATE_ON = { webauthnEnabled: true, webauthnCredentialId: 'AAAAAAAAAAAAAAAAAAAAAA' };

/** A relay whose `status` reply carries @p acts (undefined: no acts key). */
const statusRelay = (acts) => (env) =>
  env.act === 'status'
    ? { ok: true, res: { fw: '1.0.1', rssi: -50, usb: 'mounted', uptime: 9, ...(acts === undefined ? {} : { acts }) } }
    : { ok: true, res: {} };

const declined = async () => {
  throw Object.assign(new Error('cancelled'), { name: 'NotAllowedError' });
};

function keyboardState(h) {
  const chips = h.$$('#chip-row .chip');
  assert.ok(chips.length > 1, 'key-combo chips rendered');
  return {
    unlock: !h.$('#btn-unlock').disabled,
    chips: chips.map((c) => !c.disabled),
    keysSend: !h.$('#btn-keys-send').disabled,
  };
}

/* ───────────────────────────────────────────── requirement 3: acts gating ── */

test('acts without type/keys: unlock, every chip and send stay disabled, also after another command', async () => {
  const data = seedData({
    devices: [{ ...A, unlockText: SECRET, acts: NO_KEYBOARD }],
    settings: { activeDeviceId: A.deviceId },
  });
  const h = await bootApp({ data });
  const s = keyboardState(h);
  assert.equal(s.unlock, false, 'unlock send disabled');
  assert.ok(s.chips.every((on) => !on), 'every chip (presets and + Custom) disabled');
  assert.equal(s.keysSend, false, 'keys send disabled');

  // A command's busy/idle cycle must not re-enable them.
  await h.click(h.$('#btn-wake'));
  assert.deepEqual(h.sentActs(), ['wake']);
  assert.deepEqual(keyboardState(h), s);

  // Even if the handlers are reached, nothing is typed or pressed.
  await h.fire(h.$('#btn-unlock'), 'click');
  await h.fire(h.$$('#chip-row .chip')[0], 'click');
  await h.fire(h.$('#btn-keys-send'), 'click');
  assert.deepEqual(h.sentActs(), ['wake']);
});

test('no acts (older firmware) keeps unlock, chips and send enabled', async () => {
  const data = seedData({
    devices: [{ ...A, unlockText: SECRET }],
    settings: { activeDeviceId: A.deviceId },
  });
  const h = await bootApp({ data });
  const s = keyboardState(h);
  assert.equal(s.unlock, true);
  assert.ok(s.chips.every(Boolean));
  assert.equal(s.keysSend, true);
});

test('acts with keys but not type disables only unlock typing', async () => {
  const data = seedData({
    devices: [{ ...A, unlockText: SECRET, acts: [...NO_KEYBOARD, 'keys'] }],
    settings: { activeDeviceId: A.deviceId },
  });
  const h = await bootApp({ data });
  const s = keyboardState(h);
  assert.equal(s.unlock, false);
  assert.ok(s.chips.every(Boolean));
  assert.equal(s.keysSend, true);
});

test('a status reply applies acts at once and persists them for that device only', async () => {
  const data = seedData({
    devices: [{ ...A, unlockText: SECRET }, { ...B, unlockText: 'other' }],
    settings: { activeDeviceId: A.deviceId },
  });
  const h = await bootApp({ data, relay: statusRelay([...NO_KEYBOARD, 'keys']) });
  assert.equal(keyboardState(h).unlock, true);

  await h.click(h.$('#btn-refresh'));
  assert.deepEqual(h.sentActs(), ['status']);
  const s = keyboardState(h);
  assert.equal(s.unlock, false, 'type left out → unlock disabled');
  assert.ok(s.chips.every(Boolean), 'keys listed → chips stay enabled');

  assert.deepEqual([...data.devices.get(A.deviceId).acts].sort(), [...NO_KEYBOARD, 'keys'].sort());
  assert.equal(data.devices.get(B.deviceId).acts, undefined, 'the other device is untouched');

  // A later session starts from the persisted list.
  const again = await bootApp({ data });
  assert.equal(keyboardState(again).unlock, false);
});

for (const [label, acts] of [
  ['non-string entries', ['ping', 7, null, { type: true }, ['keys']]],
  ['a huge list with type/keys past its cap', [...Array.from({ length: 50_000 }, (_, i) => `x${i}`), 'type', 'keys']],
  ['oversized strings', ['type'.repeat(10_000), `keys${' '.repeat(5000)}`]],
]) {
  test(`malformed acts (${label}) neither crash the app nor enable type/keys`, async () => {
    const data = seedData({
      devices: [{ ...A, unlockText: SECRET, acts: NO_KEYBOARD }],
      settings: { activeDeviceId: A.deviceId },
    });
    const h = await bootApp({ data, relay: statusRelay(acts) });
    await h.click(h.$('#btn-refresh'));
    assert.deepEqual(h.toasts.filter((t) => t.kind === 'err'), []);
    const s = keyboardState(h);
    assert.equal(s.unlock, false);
    assert.ok(s.chips.every((on) => !on));
    assert.equal(s.keysSend, false);
  });
}

/**
 * Strict expected failure (see the report): a present `acts` that is not an
 * array is read as "older firmware" (parseActs → null), so it re-enables
 * type and keys on a device whose firmware ruled them out. The requirement
 * names "not an array" among the malformed values that must not enable
 * gated actions. When this starts passing, drop the wrapper.
 */
for (const acts of ['ping,status,wake', { type: false }, 42]) {
  test(`malformed acts (not an array: ${JSON.stringify(acts)}) must not re-enable type/keys [expected failure]`, async () => {
    const data = seedData({
      devices: [{ ...A, unlockText: SECRET, acts: NO_KEYBOARD }],
      settings: { activeDeviceId: A.deviceId },
    });
    const h = await bootApp({ data, relay: statusRelay(acts) });
    await h.click(h.$('#btn-refresh'));
    const s = keyboardState(h);
    const holds = !s.unlock && !s.keysSend && s.chips.every((on) => !on);
    assert.equal(holds, false, 'XPASS: the defect is fixed; make this an ordinary test');
  });
}

/* ──────────────────────────────── requirement 4: turning the gate off ── */

async function turnGateOff(assertion) {
  const data = seedData({
    devices: [A],
    settings: { activeDeviceId: A.deviceId, ...GATE_ON },
  });
  const h = await bootApp({ data, assertion, answer: (kind) => kind === 'confirm' });
  await h.click(h.$('#btn-settings'));
  const toggle = h.$('#toggle-webauthn');
  assert.equal(toggle.checked, true, 'gate shown as on');
  assert.equal(toggle.disabled, false);
  toggle.checked = false; // the user flips the switch
  await h.fire(toggle, 'change');
  return { h, toggle, enabled: data.settings.get('webauthnEnabled').v };
}

test('turning the biometric gate off asks for a presence check and then turns it off', async () => {
  const { h, toggle, enabled } = await turnGateOff(async () => ({ id: 'assertion' }));
  assert.equal(h.credentialGets, 1, 'one presence check');
  assert.equal(enabled, false);
  assert.equal(toggle.checked, false);
});

for (const [label, assertion] of [
  ['is cancelled', declined],
  ['returns no assertion', async () => null],
  ['fails with another error', async () => { throw Object.assign(new Error('x'), { name: 'InvalidStateError' }); }],
]) {
  test(`a presence check that ${label} leaves the biometric gate on`, async () => {
    const { h, toggle, enabled } = await turnGateOff(assertion);
    assert.equal(h.credentialGets, 1);
    assert.equal(enabled, true, 'stored gate still on');
    assert.equal(toggle.checked, true, 'switch flips back on');
  });
}

/* ──────────────────────────────────── requirement 5: stored unlock text ── */

async function settingsWithSecret(assertion, devices = [{ ...A, unlockText: SECRET }]) {
  const data = seedData({ devices, settings: { activeDeviceId: A.deviceId, ...GATE_ON } });
  const h = await bootApp({ data, assertion });
  assert.equal(h.domContains(SECRET), false, 'home view does not show the text');
  await h.click(h.$('#btn-settings'));
  return { h, data };
}

test('Settings keeps the stored unlock text out of the DOM until a presence check passes', async () => {
  let pass = false;
  const { h } = await settingsWithSecret(async () => (pass ? { id: 'ok' } : declined()));
  assert.equal(h.domContains(SECRET), false, 'opening Settings does not load it');

  await h.click(h.$('#btn-unlock-load'));
  assert.equal(h.credentialGets, 1);
  assert.equal(h.domContains(SECRET), false, 'a failed check does not load it');

  pass = true;
  await h.click(h.$('#btn-unlock-load'));
  assert.equal(h.$('#unlock-text').value, SECRET);
});

test('saving with the empty, never-loaded field keeps the stored text', async () => {
  const { h, data } = await settingsWithSecret(async () => ({ id: 'ok' }));
  assert.equal(h.$('#unlock-text').value, '');
  h.$('#toggle-enter').checked = false;
  await h.click(h.$('#btn-unlock-save'));
  const stored = data.devices.get(A.deviceId);
  assert.equal(stored.unlockText, SECRET);
  assert.equal(stored.unlockEnter, false, 'the other setting is still saved');
});

test('switching device after loading one text removes it from the field', async () => {
  const { h, data } = await settingsWithSecret(async () => ({ id: 'ok' }), [
    { ...A, unlockText: SECRET },
    { ...B, unlockText: 'B-only-secret-77' },
  ]);
  await h.click(h.$('#btn-unlock-load'));
  assert.equal(h.$('#unlock-text').value, SECRET);

  const rowB = h.$$('.device-row').find((r) => r.textContent.includes(B.deviceId));
  assert.ok(rowB, 'device B listed');
  await h.fire(rowB, 'click');
  assert.equal(h.domContains(SECRET), false, "A's text left in the DOM");
  assert.equal(h.domContains('B-only-secret-77'), false, "B's text loaded without a check");

  // Saving B now (empty field) must not write anything over B's text.
  await h.click(h.$('#btn-unlock-save'));
  assert.equal(data.devices.get(B.deviceId).unlockText, 'B-only-secret-77');
});

/* ──────────────────────────────────────────────── requirement 6: pairing ── */

const blob = (o) => JSON.stringify({ deviceId: A.deviceId, deviceToken: A.deviceToken, relayUrl: A.relayUrl, ...o });
const dialogText = (d) => [d.title, d.body, d.confirmLabel, ...(d.actions ?? []).map((a) => a.label)].join(' ');

async function pair(h, text) {
  h.$('#pair-input').value = text;
  await h.click(h.$('#btn-pair-save'));
}

test('first pairing names the relay host before anything is stored', async () => {
  let accept = false;
  const data = seedData();
  const h = await bootApp({
    data,
    answer: (kind, opts) => (/relay/i.test(dialogText(opts)) ? accept : false),
  });
  await pair(h, blob({ relayUrl: 'https://relay.example.com:8443/rw' }));
  assert.equal(h.dialogs.length, 1);
  assert.match(dialogText(h.dialogs[0]), /relay\.example\.com/);
  assert.equal(data.devices.size, 0, 'cancelled: nothing stored');

  accept = true;
  await pair(h, blob({ relayUrl: 'https://relay.example.com:8443/rw' }));
  assert.equal(data.devices.get(A.deviceId)?.relayUrl, 'https://relay.example.com:8443/rw');
});

for (const [label, change, newHost] of [
  ['relay', { relayUrl: 'https://evil.example.net' }, 'evil.example.net'],
  ['device token', { deviceToken: 'Z'.repeat(43) }, 'relay.example.com'],
  ['relay and token', { relayUrl: 'https://evil.example.net', deviceToken: 'Z'.repeat(43) }, 'evil.example.net'],
]) {
  test(`re-pairing that replaces the ${label} needs a confirmation naming ${newHost}`, async () => {
    let accept = false;
    const data = seedData({ devices: [A], settings: { activeDeviceId: A.deviceId } });
    const h = await bootApp({ data, answer: () => accept });
    await h.click(h.$('#btn-settings'));
    await h.click(h.$('#btn-add-device'));

    await pair(h, blob(change));
    const naming = h.dialogs.filter((d) => dialogText(d).includes(newHost));
    assert.equal(naming.length, 1, `one dialog names ${newHost}: ${JSON.stringify(h.dialogs)}`);
    const stored = data.devices.get(A.deviceId);
    assert.equal(stored.relayUrl, A.relayUrl, 'declined: relay unchanged');
    assert.equal(stored.deviceToken, A.deviceToken, 'declined: token unchanged');

    accept = true;
    await pair(h, blob(change));
    const after = data.devices.get(A.deviceId);
    assert.equal(after.relayUrl, change.relayUrl ?? A.relayUrl);
    assert.equal(after.deviceToken, change.deviceToken ?? A.deviceToken);
  });
}

test('a deployed app refuses an http loopback relay before any dialog', async () => {
  const data = seedData();
  const h = await bootApp({ data, appHostname: 'remote-wake.example.com', answer: () => true });
  await pair(h, blob({ relayUrl: 'http://localhost:8787' }));
  assert.equal(h.$('#pair-error').hidden, false);
  assert.match(h.$('#pair-error').textContent, /https/);
  assert.equal(h.dialogs.length, 0);
  assert.equal(data.devices.size, 0);
});

test('an app served from localhost accepts an http loopback relay', async () => {
  const data = seedData();
  const h = await bootApp({ data, appHostname: 'localhost', answer: (kind) => kind === 'sheet' && true });
  await pair(h, blob({ relayUrl: 'http://127.0.0.1:8787' }));
  assert.equal(h.$('#pair-error').hidden, true);
  assert.equal(data.devices.get(A.deviceId)?.relayUrl, 'http://127.0.0.1:8787');
});
