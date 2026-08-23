/**
 * remote-wake v1 signing-string builder.
 *
 * Dependency-free and isomorphic (browser + node) on purpose: `app/test/
 * vectors.test.mjs` imports this file directly and checks it byte-for-byte
 * against `docs/test-vectors.json`.
 *
 * Contract (docs/protocol.md):
 *
 *   remote-wake-v1\n<dev>\n<id>\n<ts>\n<ctr>\n<act>\n<args-json>
 *
 * where <args-json> is the args object serialized with keys sorted
 * lexicographically, no whitespace, UTF-8. The exact same bytes MUST be what
 * ends up in the envelope's `args` field.
 */

export const PROTOCOL_PREFIX = 'remote-wake-v1';
export const PROTOCOL_VERSION = 1;

/**
 * JSON.stringify semantics + recursively sorted object keys + no whitespace.
 *
 * Sorting is by UTF-16 code unit (JS default `Array#sort` on strings), which
 * matches byte-wise ordering of the UTF-8 encoding for every code point below
 * U+10000 and for surrogate pairs alike (both orderings agree except across
 * the U+E000..U+FFFF / astral boundary, which action args never use).
 */
export function canonicalJSON(value) {
  if (value === null) return 'null';

  const t = typeof value;

  if (t === 'boolean') return value ? 'true' : 'false';
  if (t === 'string') return JSON.stringify(value);
  if (t === 'number') return Number.isFinite(value) ? JSON.stringify(value) : 'null';
  if (t === 'bigint') throw new TypeError('canonicalJSON: BigInt is not serializable');
  if (t === 'undefined' || t === 'function' || t === 'symbol') return 'null';

  if (Array.isArray(value)) {
    let out = '';
    for (let i = 0; i < value.length; i++) {
      if (i) out += ',';
      out += canonicalJSON(value[i]);
    }
    return '[' + out + ']';
  }

  if (typeof value.toJSON === 'function') return canonicalJSON(value.toJSON());

  const keys = Object.keys(value)
    .filter((k) => {
      const v = value[k];
      return v !== undefined && typeof v !== 'function' && typeof v !== 'symbol';
    })
    .sort();

  let out = '';
  for (let i = 0; i < keys.length; i++) {
    if (i) out += ',';
    out += JSON.stringify(keys[i]) + ':' + canonicalJSON(value[keys[i]]);
  }
  return '{' + out + '}';
}

/** Canonical serialization of an action `args` object (never absent → `{}`). */
export function canonicalArgs(args) {
  if (args === undefined || args === null) return '{}';
  if (typeof args !== 'object' || Array.isArray(args)) {
    throw new TypeError('args must be a plain object');
  }
  return canonicalJSON(args);
}

/**
 * Build the exact signing string for a command envelope.
 * @param {{dev:string,id:string,ts:number,ctr:number,act:string,args?:object}} c
 * @returns {string}
 */
export function buildSigningString(c) {
  if (!c || typeof c !== 'object') throw new TypeError('command required');
  for (const f of ['dev', 'id', 'act']) {
    if (typeof c[f] !== 'string' || c[f] === '') {
      throw new TypeError(`command.${f} must be a non-empty string`);
    }
  }
  for (const f of ['ts', 'ctr']) {
    if (!Number.isInteger(c[f]) || c[f] < 0) {
      throw new TypeError(`command.${f} must be a non-negative integer`);
    }
  }
  return (
    PROTOCOL_PREFIX +
    '\n' + c.dev +
    '\n' + c.id +
    '\n' + c.ts +
    '\n' + c.ctr +
    '\n' + c.act +
    '\n' + canonicalArgs(c.args)
  );
}

/** UTF-8 bytes of the signing string — this is what gets signed. */
export function signingBytes(command) {
  return new TextEncoder().encode(buildSigningString(command));
}

/**
 * Assemble the wire envelope. `args` is re-parsed from the canonical string so
 * that JSON.stringify(envelope) emits exactly the canonicalized bytes the
 * device will re-serialize and verify against.
 */
export function buildEnvelope(command, kid, sigB64url) {
  return {
    v: PROTOCOL_VERSION,
    dev: command.dev,
    id: command.id,
    ts: command.ts,
    ctr: command.ctr,
    act: command.act,
    args: JSON.parse(canonicalArgs(command.args)),
    kid,
    sig: sigB64url,
  };
}

/**
 * Serialize an envelope for the wire, splicing in the canonical `args` bytes
 * verbatim rather than trusting JSON.stringify's key order (integer-like keys
 * would otherwise be hoisted). Use this, not JSON.stringify(envelope).
 */
export function serializeEnvelope(env) {
  return (
    '{"v":' + JSON.stringify(env.v) +
    ',"dev":' + JSON.stringify(env.dev) +
    ',"id":' + JSON.stringify(env.id) +
    ',"ts":' + JSON.stringify(env.ts) +
    ',"ctr":' + JSON.stringify(env.ctr) +
    ',"act":' + JSON.stringify(env.act) +
    ',"args":' + canonicalArgs(env.args) +
    ',"kid":' + JSON.stringify(env.kid) +
    ',"sig":' + JSON.stringify(env.sig) +
    '}'
  );
}
