/**
 * Requirement 7 through the real command path: commands.run sends a signed
 * command, the (unsigned) relay reply says `replay` with a `lastCtr`, and the
 * real store decides whether the phone's counter jumps. Only IndexedDB, fetch
 * and the authenticator are faked (test/support/app_harness.mjs).
 *
 * run() first increments-and-persists the counter (10 → 11), so the values
 * below sit clear of both readings of "ahead of the current counter" (10 or
 * 11): accepted ones are ≤ 1,000,000 ahead of 11, rejected ones > 1,000,000
 * ahead of 11.
 */
import test from 'node:test';
import assert from 'node:assert/strict';

import { freshGraph, installPlatform, seedData } from './support/app_harness.mjs';

const DEV = {
  deviceId: 'a1b2c3d4e5f60718',
  deviceToken: 'e0yk9uPHzP93lnlGS9oTCkf9KCAzA9RAfZN1vn1VEK0',
  relayUrl: 'https://relay.example.com',
  kid: 'p1',
};
const CTR_KEY = `${DEV.deviceId}:p1`;

async function replayWith(lastCtr, start = 10) {
  const data = seedData({ counters: { [CTR_KEY]: start } });
  const h = installPlatform({
    data,
    relay: () => ({ ok: false, err: 'replay', res: lastCtr === undefined ? {} : { lastCtr } }),
  });
  const load = freshGraph();
  const crypto = await load('crypto.js');
  const commands = await load('commands.js');
  await crypto.ensureKey();
  const r = await commands.run(DEV, 'ping', {}, { skipPresence: true });
  assert.equal(r.err, 'replay');
  assert.equal(h.sent.length, 1);
  assert.equal(h.sent[0].env.ctr, start + 1, 'signed with the next counter');
  return data.counters.get(CTR_KEY).ctr;
}

test('a replay reply within 1,000,000 moves the counter to lastCtr', async () => {
  assert.equal(await replayWith(11 + 999_999), 1_000_010);
  assert.equal(await replayWith(12), 12);
});

test('a replay reply more than 1,000,000 ahead is ignored', async () => {
  assert.equal(await replayWith(10 + 1_000_002), 11);
  assert.equal(await replayWith(Number.MAX_SAFE_INTEGER), 11);
});

test('a replay reply near the 2^52 ceiling: below it moves, at it does not', async () => {
  assert.equal(await replayWith(2 ** 52 - 1, 2 ** 52 - 100), 2 ** 52 - 1);
  assert.equal(await replayWith(2 ** 52, 2 ** 52 - 100), 2 ** 52 - 99);
});

for (const bad of ['500', '1e3', 500.5, true, null, [500], { n: 500 }, undefined]) {
  test(`a replay reply with lastCtr ${JSON.stringify(bad)} leaves the counter alone`, async () => {
    assert.equal(await replayWith(bad), 11);
  });
}
