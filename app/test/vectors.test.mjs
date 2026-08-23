/**
 * Cross-implementation conformance test for the remote-wake v1 signing string.
 *
 *   node --test          (from app/)   or   npm test
 *
 * Checks, against docs/test-vectors.json:
 *   1. buildSigningString() reproduces every `signing_strings` entry byte-for-byte.
 *   2. The vendored @noble/ed25519 signs those bytes with the vector seed and
 *      reproduces every `sig` byte-for-byte, and verifies them.
 *   3. node:crypto (independent implementation) agrees with noble.
 *   4. serializeEnvelope() emits the canonical args bytes.
 */
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { createPrivateKey, createPublicKey, sign as nodeSign, verify as nodeVerify } from 'node:crypto';

import {
  buildSigningString,
  signingBytes,
  canonicalJSON,
  canonicalArgs,
  buildEnvelope,
  serializeEnvelope,
} from '../src/signing.js';
import * as ed from '../vendor/noble-ed25519.js';

const VECTORS = JSON.parse(
  readFileSync(new URL('../../docs/test-vectors.json', import.meta.url), 'utf8')
);

const hexToBytes = (h) => Uint8Array.from(h.match(/../g).map((b) => parseInt(b, 16)));
const b64urlToBytes = (s) =>
  new Uint8Array(Buffer.from(s.replace(/-/g, '+').replace(/_/g, '/'), 'base64'));
const bytesToB64url = (b) =>
  Buffer.from(b).toString('base64').replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');

const seed = hexToBytes(VECTORS.seed_hex);

// node:crypto needs the raw seed wrapped in a PKCS#8 header.
const PKCS8_ED25519_PREFIX = hexToBytes('302e020100300506032b657004220420');
const nodePriv = createPrivateKey({
  key: Buffer.concat([Buffer.from(PKCS8_ED25519_PREFIX), Buffer.from(seed)]),
  format: 'der',
  type: 'pkcs8',
});
const nodePub = createPublicKey(nodePriv);

test('vector file exposes a public key consistent with the seed', async () => {
  const pub = await ed.getPublicKeyAsync(seed);
  assert.equal(Buffer.from(pub).toString('hex'), VECTORS.pubkey_hex);
  assert.equal(bytesToB64url(pub), VECTORS.pubkey_b64url);
});

for (const [i, v] of VECTORS.vectors.entries()) {
  const e = v.envelope;

  test(`vector ${i} (${e.act}): signing string is byte-identical`, () => {
    const built = buildSigningString({
      dev: e.dev,
      id: e.id,
      ts: e.ts,
      ctr: e.ctr,
      act: e.act,
      args: e.args,
    });
    assert.equal(built, v.signing_string);
    assert.deepEqual(
      Buffer.from(signingBytes(e)),
      Buffer.from(v.signing_string, 'utf8'),
      'UTF-8 bytes must match'
    );
  });

  test(`vector ${i} (${e.act}): noble signature matches and verifies`, async () => {
    const msg = signingBytes(e);
    const sig = await ed.signAsync(msg, seed);
    assert.equal(bytesToB64url(sig), e.sig, 'deterministic Ed25519 signature must match');
    assert.equal(
      await ed.verifyAsync(b64urlToBytes(e.sig), msg, hexToBytes(VECTORS.pubkey_hex)),
      true
    );
  });

  test(`vector ${i} (${e.act}): node:crypto agrees`, () => {
    const msg = Buffer.from(signingBytes(e));
    assert.equal(bytesToB64url(nodeSign(null, msg, nodePriv)), e.sig);
    assert.equal(nodeVerify(null, msg, nodePub, Buffer.from(b64urlToBytes(e.sig))), true);
  });

  test(`vector ${i} (${e.act}): envelope round-trips with canonical args`, () => {
    const env = buildEnvelope(e, e.kid, e.sig);
    const wire = serializeEnvelope(env);
    const parsed = JSON.parse(wire);
    assert.equal(parsed.v, 1);
    assert.equal(parsed.act, e.act);
    assert.deepEqual(parsed.args, e.args);
    // The args substring on the wire is exactly the signed serialization.
    assert.ok(
      wire.includes('"args":' + v.signing_string.slice(v.signing_string.lastIndexOf('\n') + 1)),
      'wire args must be the canonical bytes'
    );
  });
}

test('canonicalJSON sorts recursively, emits no whitespace', () => {
  assert.equal(canonicalJSON({}), '{}');
  assert.equal(canonicalJSON({ b: 1, a: 2 }), '{"a":2,"b":1}');
  assert.equal(
    canonicalJSON({ z: { y: 1, x: [3, { b: 1, a: 0 }] }, a: null }),
    '{"a":null,"z":{"x":[3,{"a":0,"b":1}],"y":1}}'
  );
  assert.equal(canonicalJSON({ 'ä': 1, 'a': 2, 'A': 3 }), '{"A":3,"a":2,"ä":1}');
  assert.equal(canonicalJSON({ s: 'a"b\\c\nd' }), '{"s":"a\\"b\\\\c\\nd\\u0007"}');
  assert.equal(canonicalJSON({ u: '✓ ünïcø∂e' }), '{"u":"✓ ünïcø∂e"}');
  assert.equal(canonicalJSON({ dropped: undefined, kept: 1 }), '{"kept":1}');
});

test('canonicalArgs defaults an absent args object to {}', () => {
  assert.equal(canonicalArgs(undefined), '{}');
  assert.equal(canonicalArgs(null), '{}');
  assert.throws(() => canonicalArgs([1]), TypeError);
});

test('buildSigningString rejects malformed commands', () => {
  const ok = { dev: 'a1', id: 'i', ts: 1, ctr: 1, act: 'wake', args: {} };
  assert.doesNotThrow(() => buildSigningString(ok));
  assert.throws(() => buildSigningString({ ...ok, dev: '' }), TypeError);
  assert.throws(() => buildSigningString({ ...ok, ts: 1.5 }), TypeError);
  assert.throws(() => buildSigningString({ ...ok, ctr: -1 }), TypeError);
});

// Sanity: signature must not survive tampering with any covered field.
test('every semantic field is covered by the signature', async () => {
  const e = VECTORS.vectors[1].envelope;
  const pub = hexToBytes(VECTORS.pubkey_hex);
  const mutations = [
    { ...e, dev: 'ffffffffffffffff' },
    { ...e, id: '00000000-0000-4000-8000-000000000000' },
    { ...e, ts: e.ts + 1 },
    { ...e, ctr: e.ctr + 1 },
    { ...e, act: 'wake' },
    { ...e, args: { ...e.args, text: 'hunter3!' } },
  ];
  for (const m of mutations) {
    assert.equal(
      await ed.verifyAsync(b64urlToBytes(e.sig), signingBytes(m), pub),
      false,
      `tampered field should not verify: ${JSON.stringify(m)}`
    );
  }
});

void fileURLToPath;
