import assert from 'node:assert/strict';
import { test } from 'node:test';
import { sendCommand, errorMessage } from '../src/api.js';

test('a command timeout is distinct from offline and does not resend', async () => {
  const original = globalThis.fetch;
  const device = { relayUrl: 'https://relay.test', deviceId: '0123456789abcdef',
    deviceToken: 'test-only-token' };
  const command = { v: 1, dev: device.deviceId, id: 'test', ts: 1, ctr: 1,
    act: 'wake', args: {}, kid: 'p1', sig: 'test' };
  let sends = 0;
  try {
    for (const code of ['device_timeout', 'device_offline']) {
      globalThis.fetch = async () => {
        sends++;
        return new Response(JSON.stringify({ err: code }), { status: 504 });
      };
      await assert.rejects(sendCommand(device, command), (err) => {
        assert.equal(err.code, code);
        if (code === 'device_timeout') assert.match(err.message, /may have run/);
        return true;
      });
    }
    assert.equal(sends, 2);
    assert.match(errorMessage('device_timeout'), /may have run/);
    assert.match(errorMessage('usb_down'), /may be partial/);
  } finally {
    globalThis.fetch = original;
  }
});
