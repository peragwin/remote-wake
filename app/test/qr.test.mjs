/**
 * QR encoder validation.
 *
 * Rather than trust the encoder, the test independently DECODES the matrix it
 * produces: it re-derives the function-pattern layout, reads the format bits,
 * un-masks, walks the data placement in reverse, de-interleaves the blocks,
 * checks each block's Reed-Solomon parity is mathematically valid (the codeword
 * polynomial must be divisible by the generator, i.e. evaluate to zero at
 * α^0…α^(n-1)), and finally reads the byte-mode payload back out.
 */
import test from 'node:test';
import assert from 'node:assert/strict';
import { encode, toSVG } from '../src/qr.js';

/* Independent GF(256) for the parity check. */
const EXP = new Uint8Array(512);
const LOG = new Uint8Array(256);
{
  let x = 1;
  for (let i = 0; i < 255; i++) {
    EXP[i] = x;
    LOG[x] = i;
    x = (x << 1) ^ (x & 0x80 ? 0x11d : 0);
  }
  for (let i = 255; i < 512; i++) EXP[i] = EXP[i - 255];
}
const mul = (a, b) => (a === 0 || b === 0 ? 0 : EXP[LOG[a] + LOG[b]]);

const VERSIONS_L = {
  1: [19, 7, [[1, 19]]], 2: [34, 10, [[1, 34]]], 3: [55, 15, [[1, 55]]],
  4: [80, 20, [[1, 80]]], 5: [108, 26, [[1, 108]]], 6: [136, 18, [[2, 68]]],
  7: [156, 20, [[2, 78]]], 8: [194, 24, [[2, 97]]], 9: [232, 30, [[2, 116]]],
  10: [274, 18, [[2, 68], [2, 69]]],
};
const ALIGNMENT = {
  1: [], 2: [6, 18], 3: [6, 22], 4: [6, 26], 5: [6, 30],
  6: [6, 34], 7: [6, 22, 38], 8: [6, 24, 42], 9: [6, 26, 46], 10: [6, 28, 50],
};
const FORMAT_L = [0x77c4, 0x72f3, 0x7daa, 0x789d, 0x662f, 0x6318, 0x6c41, 0x6976];
const MASKS = [
  (r, c) => (r + c) % 2 === 0,
  (r) => r % 2 === 0,
  (_r, c) => c % 3 === 0,
  (r, c) => (r + c) % 3 === 0,
  (r, c) => (Math.floor(r / 2) + Math.floor(c / 3)) % 2 === 0,
  (r, c) => ((r * c) % 2) + ((r * c) % 3) === 0,
  (r, c) => (((r * c) % 2) + ((r * c) % 3)) % 2 === 0,
  (r, c) => (((r + c) % 2) + ((r * c) % 3)) % 2 === 0,
];

/** Independently mark which modules are function/reserved (not data). */
function reservedMap(version) {
  const size = 17 + version * 4;
  const res = Array.from({ length: size }, () => new Uint8Array(size));
  const mark = (r, c) => {
    if (r >= 0 && r < size && c >= 0 && c < size) res[r][c] = 1;
  };
  for (const [br, bc] of [[0, 0], [0, size - 7], [size - 7, 0]]) {
    for (let r = -1; r <= 7; r++) for (let c = -1; c <= 7; c++) mark(br + r, bc + c);
  }
  for (let i = 0; i < size; i++) {
    mark(6, i);
    mark(i, 6);
  }
  const centers = ALIGNMENT[version];
  for (const r of centers) {
    for (const c of centers) {
      if ((r === 6 && c === 6) || (r === 6 && c === size - 7) || (r === size - 7 && c === 6)) continue;
      for (let dr = -2; dr <= 2; dr++) for (let dc = -2; dc <= 2; dc++) mark(r + dr, c + dc);
    }
  }
  for (let i = 0; i < 9; i++) {
    mark(8, i);
    mark(i, 8);
  }
  for (let i = 0; i < 8; i++) {
    mark(8, size - 1 - i);
    mark(size - 1 - i, 8);
  }
  if (version >= 7) {
    for (let i = 0; i < 18; i++) {
      mark(Math.floor(i / 3), size - 11 + (i % 3));
      mark(size - 11 + (i % 3), Math.floor(i / 3));
    }
  }
  return res;
}

function readFormatMask(modules) {
  const size = modules.length;
  // Read the split copy (bottom-left + top-right), which is unambiguous.
  let bits = 0;
  for (let i = 0; i < 15; i++) {
    const bit = i < 8 ? modules[size - 1 - i][8] : modules[8][size - 15 + i];
    if (bit) bits |= 1 << i;
  }
  const mask = FORMAT_L.indexOf(bits);
  assert.notEqual(mask, -1, `format bits 0x${bits.toString(16)} are not a valid ECC-L format`);
  return mask;
}

function extractCodewords(modules, version, mask) {
  const size = modules.length;
  const res = reservedMap(version);
  const bits = [];
  let upward = true;
  for (let col = size - 1; col > 0; col -= 2) {
    if (col === 6) col--;
    for (let i = 0; i < size; i++) {
      const row = upward ? size - 1 - i : i;
      for (const c of [col, col - 1]) {
        if (res[row][c]) continue;
        bits.push((modules[row][c] ? 1 : 0) ^ (MASKS[mask](row, c) ? 1 : 0));
      }
    }
    upward = !upward;
  }
  const out = [];
  for (let i = 0; i + 8 <= bits.length; i += 8) {
    let b = 0;
    for (let j = 0; j < 8; j++) b = (b << 1) | bits[i + j];
    out.push(b);
  }
  return out;
}

function deinterleave(codewords, version) {
  const [, ecLen, groups] = VERSIONS_L[version];
  const sizes = [];
  for (const [count, size] of groups) for (let i = 0; i < count; i++) sizes.push(size);
  const blocks = sizes.map(() => []);
  let idx = 0;
  const maxSize = Math.max(...sizes);
  for (let i = 0; i < maxSize; i++) {
    for (let b = 0; b < sizes.length; b++) if (i < sizes[b]) blocks[b].push(codewords[idx++]);
  }
  const ec = sizes.map(() => []);
  for (let i = 0; i < ecLen; i++) for (let b = 0; b < sizes.length; b++) ec[b].push(codewords[idx++]);
  return { blocks, ec, ecLen };
}

/** A valid RS codeword evaluates to zero at α^0 … α^(ecLen-1). */
function assertParityValid(data, ec, ecLen, label) {
  const full = [...data, ...ec];
  for (let i = 0; i < ecLen; i++) {
    let acc = 0;
    for (const b of full) acc = mul(acc, EXP[i]) ^ b;
    assert.equal(acc, 0, `${label}: syndrome ${i} should be 0 (Reed-Solomon parity is wrong)`);
  }
}

function readPayload(blocks, version) {
  const data = blocks.flat();
  const bits = [];
  for (const b of data) for (let i = 7; i >= 0; i--) bits.push((b >> i) & 1);
  const take = (n) => {
    let v = 0;
    for (let i = 0; i < n; i++) v = (v << 1) | bits.shift();
    return v;
  };
  assert.equal(take(4), 0b0100, 'mode indicator must be byte mode');
  const len = take(version < 10 ? 8 : 16);
  const out = new Uint8Array(len);
  for (let i = 0; i < len; i++) out[i] = take(8);
  return new TextDecoder().decode(out);
}

function roundTrip(text) {
  const { modules, version, size } = encode(text);
  assert.equal(size, 17 + version * 4);
  assert.equal(modules.length, size);

  const mask = readFormatMask(modules);
  const codewords = extractCodewords(modules, version, mask);
  const [dataCw, ecLen] = VERSIONS_L[version];
  const { blocks, ec } = deinterleave(codewords, version);
  assert.equal(blocks.flat().length, dataCw);
  blocks.forEach((b, i) => assertParityValid(b, ec[i], ecLen, `${text.slice(0, 12)} block ${i}`));
  return readPayload(blocks, version);
}

test('finder, timing and dark modules are placed correctly', () => {
  const { modules, size } = encode('remote-wake');
  for (const [br, bc] of [[0, 0], [0, size - 7], [size - 7, 0]]) {
    assert.equal(modules[br][bc], true, 'finder outer ring corner');
    assert.equal(modules[br + 1][bc + 1], false, 'finder light ring');
    assert.equal(modules[br + 3][bc + 3], true, 'finder core');
  }
  for (let i = 8; i < size - 8; i++) {
    assert.equal(modules[6][i], i % 2 === 0, `horizontal timing at ${i}`);
    assert.equal(modules[i][6], i % 2 === 0, `vertical timing at ${i}`);
  }
  assert.equal(modules[size - 8][8], true, 'dark module');
});

test('round-trips a base64url Ed25519 public key (the real payload)', () => {
  const pubkey = 'e0yk9uPHzP93lnlGS9oTCkf9KCAzA9RAfZN1vn1VEK0';
  assert.equal(roundTrip(pubkey), pubkey);
});

test('round-trips payloads across every supported version', () => {
  const samples = [
    'hi',
    'remote-wake',
    'A'.repeat(30),
    'A'.repeat(60),
    'A'.repeat(90),
    'A'.repeat(120),
    'A'.repeat(150),
    'A'.repeat(180),
    'A'.repeat(220),
    'A'.repeat(270),
  ];
  const seen = new Set();
  for (const s of samples) {
    assert.equal(roundTrip(s), s, `payload of length ${s.length}`);
    seen.add(encode(s).version);
  }
  assert.ok(seen.size >= 8, `expected many versions exercised, saw ${[...seen].join(',')}`);
});

test('round-trips UTF-8 and JSON payloads', () => {
  const json = JSON.stringify({ pubkey: 'e0yk9uPHzP93lnlGS9oTCkf9KCAzA9RAfZN1vn1VEK0', name: 'Älex ✓' });
  assert.equal(roundTrip(json), json);
});

test('rejects payloads beyond the encoder capacity', () => {
  assert.throws(() => encode('A'.repeat(275)), /exceeds/);
});

test('toSVG emits a self-contained square SVG', () => {
  const svg = toSVG('e0yk9uPHzP93lnlGS9oTCkf9KCAzA9RAfZN1vn1VEK0');
  assert.match(svg, /^<svg xmlns="http:\/\/www\.w3\.org\/2000\/svg"/);
  assert.match(svg, /viewBox="0 0 (\d+) \1"/);
  assert.match(svg, /<path d="M/);
  assert.doesNotMatch(svg, /https?:\/\/(?!www\.w3\.org)/, 'must not reference external resources');
});
