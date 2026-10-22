/**
 * Pure decision logic behind three PWA safety checks: which actions the UI
 * offers (status `acts`), how far an unsigned `replay` reply may move the
 * counter, and when a pairing blob is replacing a stored relay or token.
 */
import test from 'node:test';
import assert from 'node:assert/strict';

import { parseActs, actAllowed, sameActs } from '../src/caps.js';
import { acceptFastForward, COUNTER_MAX_JUMP, COUNTER_CEILING } from '../src/store.js';
import { pairingChange, relayLabel } from '../src/pairing.js';

/* ───────────────────────────────────────────────────────────── acts ── */

test('no acts list (older firmware) keeps every action enabled', () => {
  for (const res of [{}, undefined, null, { acts: 'type' }, { acts: { type: true } }]) {
    const acts = parseActs(res);
    assert.equal(acts, null);
    for (const a of ['type', 'keys', 'wake', 'power_hold']) assert.equal(actAllowed(acts, a), true);
  }
  assert.equal(actAllowed(undefined, 'type'), true);
});

test('an acts list without type/keys disables exactly those', () => {
  const acts = parseActs({ acts: ['ping', 'status', 'wake', 'power_tap', 'power_hold'] });
  assert.equal(actAllowed(acts, 'type'), false);
  assert.equal(actAllowed(acts, 'keys'), false);
  assert.equal(actAllowed(acts, 'wake'), true);
  assert.equal(actAllowed(acts, 'power_tap'), true);

  const onlyKeys = parseActs({ acts: ['ping', 'status', 'wake', 'keys'] });
  assert.equal(actAllowed(onlyKeys, 'type'), false);
  assert.equal(actAllowed(onlyKeys, 'keys'), true);

  // An empty list is still a list: nothing is offered.
  assert.equal(actAllowed(parseActs({ acts: [] }), 'wake'), false);
});

test('acts parsing drops junk entries and duplicates', () => {
  assert.deepEqual(parseActs({ acts: ['type', 7, null, '', 'type', 'keys', 'x'.repeat(40)] }), ['type', 'keys']);
  assert.equal(parseActs({ acts: Array.from({ length: 100 }, (_, i) => `a${i}`) }).length, 32);
});

test('sameActs ignores order and treats missing lists alike', () => {
  assert.ok(sameActs(['a', 'b'], ['b', 'a']));
  assert.ok(sameActs(null, undefined));
  assert.ok(!sameActs(['a'], null));
  assert.ok(!sameActs(['a', 'b'], ['a']));
  assert.ok(!sameActs([], undefined));
});

/* ──────────────────────────────────────────────── counter fast-forward ── */

test('a replay reply may move the counter forward by at most 1,000,000', () => {
  assert.equal(COUNTER_MAX_JUMP, 1_000_000);
  assert.ok(acceptFastForward(0, 1));
  assert.ok(acceptFastForward(10, 10 + 1_000_000));
  assert.ok(!acceptFastForward(10, 10 + 1_000_001));
  assert.ok(acceptFastForward(5_000_000, 5_999_999));
});

test('the counter never moves backward or sideways', () => {
  assert.ok(!acceptFastForward(10, 10));
  assert.ok(!acceptFastForward(10, 9));
  assert.ok(!acceptFastForward(10, -1));
});

test('only safe integers below 2^52 are accepted', () => {
  assert.equal(COUNTER_CEILING, 2 ** 52);
  const near = 2 ** 52 - 10;
  assert.ok(acceptFastForward(near - 5, near));
  assert.ok(acceptFastForward(2 ** 52 - 2, 2 ** 52 - 1));
  assert.ok(!acceptFastForward(2 ** 52 - 1, 2 ** 52));
  assert.ok(!acceptFastForward(Number.MAX_SAFE_INTEGER - 1, Number.MAX_SAFE_INTEGER));
  for (const v of [11.5, NaN, Infinity, '12', 12n, null, undefined, {}, [12], true]) {
    assert.ok(!acceptFastForward(10, v), `rejects ${String(v)}`);
  }
});

/* ───────────────────────────────────────────────────── pairing change ── */

const DEV = {
  deviceId: 'a1b2c3d4e5f60718',
  deviceToken: 'e0yk9uPHzP93lnlGS9oTCkf9KCAzA9RAfZN1vn1VEK0',
  relayUrl: 'https://relay.example.com',
};

test('first pairing is new; identical re-pairing changes nothing', () => {
  assert.deepEqual(pairingChange(undefined, DEV), { isNew: true, relayChanged: false, tokenChanged: false });
  assert.deepEqual(pairingChange({ ...DEV, name: 'PC', unlockText: 'x' }, DEV), {
    isNew: false,
    relayChanged: false,
    tokenChanged: false,
  });
});

test('a blob replacing the stored relay or token is flagged', () => {
  const other = { ...DEV, relayUrl: 'https://evil.example.net' };
  assert.deepEqual(pairingChange(DEV, other), { isNew: false, relayChanged: true, tokenChanged: false });
  const token = { ...DEV, deviceToken: 'A'.repeat(43) };
  assert.deepEqual(pairingChange(DEV, token), { isNew: false, relayChanged: false, tokenChanged: true });
  assert.deepEqual(pairingChange(DEV, { ...other, deviceToken: 'A'.repeat(43) }), {
    isNew: false,
    relayChanged: true,
    tokenChanged: true,
  });
});

test('relay label shows host, port and base path', () => {
  assert.equal(relayLabel('https://relay.example.com'), 'relay.example.com');
  assert.equal(relayLabel('https://relay.example.com:8443/rw'), 'relay.example.com:8443/rw');
  assert.equal(relayLabel('http://localhost:8787'), 'localhost:8787');
});
