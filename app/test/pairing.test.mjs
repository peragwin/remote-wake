/**
 * Pairing-blob parsing and action-argument validation — the two places where
 * the app decides whether something is protocol-shaped before it is trusted.
 */
import test from 'node:test';
import assert from 'node:assert/strict';

import { parsePairingBlob } from '../src/pairing.js';
import { parseChord, prettyChord, PRESETS, isValidKeyName } from '../src/keys.js';
import { LIMITS, DEFAULTS } from '../src/commands.js';
import { canonicalArgs } from '../src/signing.js';

const GOOD = {
  deviceId: 'a1b2c3d4e5f60718',
  deviceToken: 'e0yk9uPHzP93lnlGS9oTCkf9KCAzA9RAfZN1vn1VEK0', // 43 chars = 32 bytes
  relayUrl: 'https://relay.example.com',
};

/* ───────────────────────────────────────────────────── pairing blob ── */

test('accepts a well-formed pairing blob', () => {
  const out = parsePairingBlob(JSON.stringify(GOOD));
  assert.deepEqual(out, GOOD);
});

test('tolerates whitespace, code fences and a trailing slash', () => {
  const fenced = '```json\n' + JSON.stringify({ ...GOOD, relayUrl: 'https://relay.example.com/' }) + '\n```';
  assert.equal(parsePairingBlob('  \n' + fenced + '  ').relayUrl, 'https://relay.example.com');
});

test('keeps a relay path prefix but drops query and fragment', () => {
  const out = parsePairingBlob(
    JSON.stringify({ ...GOOD, relayUrl: 'https://example.com/rw/?x=1#frag' })
  );
  assert.equal(out.relayUrl, 'https://example.com/rw');
});

test('carries an optional device name through, clamped', () => {
  assert.equal(parsePairingBlob(JSON.stringify({ ...GOOD, name: '  Study PC ' })).name, 'Study PC');
  assert.equal(parsePairingBlob(JSON.stringify({ ...GOOD, name: 'x'.repeat(90) })).name.length, 40);
  assert.equal(parsePairingBlob(JSON.stringify(GOOD)).name, undefined);
});

test('allows http only for localhost development', () => {
  assert.doesNotThrow(() => parsePairingBlob(JSON.stringify({ ...GOOD, relayUrl: 'http://localhost:8787' })));
  assert.doesNotThrow(() => parsePairingBlob(JSON.stringify({ ...GOOD, relayUrl: 'http://127.0.0.1:8787' })));
  assert.throws(
    () => parsePairingBlob(JSON.stringify({ ...GOOD, relayUrl: 'http://relay.example.com' })),
    /must be https/
  );
});

test('rejects malformed identifiers', () => {
  const bad = [
    [{}, /deviceId/],
    [{ ...GOOD, deviceId: 'A1B2C3D4E5F60718' }, /lowercase hex/], // uppercase
    [{ ...GOOD, deviceId: 'a1b2c3d4e5f607' }, /16 lowercase hex/], // too short
    [{ ...GOOD, deviceId: 'a1b2c3d4e5f60718aa' }, /16 lowercase hex/], // too long
    [{ ...GOOD, deviceToken: 'short' }, /43 characters/],
    [{ ...GOOD, deviceToken: 'a'.repeat(43) + '=' }, /43 characters/], // padded
    [{ ...GOOD, deviceToken: 'a/b+' + 'c'.repeat(39) }, /43 characters/], // base64, not base64url
    [{ ...GOOD, relayUrl: 'not a url' }, /valid URL/],
    [{ ...GOOD, relayUrl: '' }, /valid URL/],
  ];
  for (const [obj, re] of bad) {
    assert.throws(() => parsePairingBlob(JSON.stringify(obj)), re, `should reject ${JSON.stringify(obj)}`);
  }
});

test('rejects non-JSON and non-objects', () => {
  assert.throws(() => parsePairingBlob(''), /Paste the JSON/);
  assert.throws(() => parsePairingBlob('   '), /Paste the JSON/);
  assert.throws(() => parsePairingBlob('hello'), /not valid JSON/);
  assert.throws(() => parsePairingBlob('[1,2]'), /JSON object/);
  assert.throws(() => parsePairingBlob('null'), /JSON object/);
});

/* ─────────────────────────────────────────────────────── key chords ── */

test('parses key combos with common aliases', () => {
  assert.deepEqual(parseChord('ctrl+alt+del'), ['LCTRL', 'LALT', 'DELETE']);
  assert.deepEqual(parseChord('Win+L'), ['LGUI', 'L']);
  assert.deepEqual(parseChord('  cmd , shift  esc '), ['LGUI', 'LSHIFT', 'ESC']);
  assert.deepEqual(parseChord('LCTRL+F4'), ['LCTRL', 'F4']);
  assert.deepEqual(parseChord('ctrl+ctrl+a'), ['LCTRL', 'A'], 'duplicates collapse');
});

test('rejects nonsense combos', () => {
  assert.throws(() => parseChord(''), /Type a key combo/);
  assert.throws(() => parseChord('   '), /Type a key combo/);
  assert.throws(() => parseChord('ctrl+é'), /Not a key name/);
  assert.throws(() => parseChord('a+b+c+d+e+f+g'), /Too many keys/);
});

test('presets are all protocol-valid and render nicely', () => {
  for (const p of PRESETS) {
    assert.ok(p.seq.length >= 1 && p.seq.length <= LIMITS.keysMaxChords);
    for (const chord of p.seq) {
      assert.ok(chord.length > 0);
      for (const k of chord) assert.ok(isValidKeyName(k), `${p.id}: bad key name ${k}`);
    }
    assert.ok(prettyChord(p.seq[0]).length > 0);
  }
  const ctrlAltDel = PRESETS.find((p) => p.id === 'ctrl-alt-del');
  assert.equal(canonicalArgs({ seq: ctrlAltDel.seq }), '{"seq":[["LCTRL","LALT","DELETE"]]}');
  assert.equal(prettyChord(ctrlAltDel.seq[0]), 'Ctrl + Alt + Del');
});

/* ─────────────────────────────────────────────── protocol constants ── */

test('client-side limits match docs/protocol.md', () => {
  assert.equal(LIMITS.typeMaxChars, 256);
  assert.equal(LIMITS.keysMaxChords, 32);
  assert.deepEqual(LIMITS.powerTapMs, [50, 1000]);
  assert.deepEqual(LIMITS.powerHoldMs, [3000, 12000]);
  assert.equal(DEFAULTS.power_tap, 200);
  assert.equal(DEFAULTS.power_hold, 6000);
  // Defaults must sit inside their own bounds.
  assert.ok(DEFAULTS.power_tap >= LIMITS.powerTapMs[0] && DEFAULTS.power_tap <= LIMITS.powerTapMs[1]);
  assert.ok(DEFAULTS.power_hold >= LIMITS.powerHoldMs[0] && DEFAULTS.power_hold <= LIMITS.powerHoldMs[1]);
});
