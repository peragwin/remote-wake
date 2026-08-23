/**
 * Minimal QR Code generator — byte mode, error-correction level L,
 * versions 1–10 (up to 274 data bytes). ~300 lines, no dependencies.
 *
 * Used to show the phone's Ed25519 public key so it can be carried to a laptop
 * that is joined to the device's setup AP, without retyping 43 characters.
 *
 * Returns a boolean module matrix; render() draws it as a crisp SVG.
 */

/* ------------------------------------------------------------- GF(256) */

const EXP = new Uint8Array(512);
const LOG = new Uint8Array(256);
(function initGF() {
  let x = 1;
  for (let i = 0; i < 255; i++) {
    EXP[i] = x;
    LOG[x] = i;
    x <<= 1;
    if (x & 0x100) x ^= 0x11d;
  }
  for (let i = 255; i < 512; i++) EXP[i] = EXP[i - 255];
})();

const gfMul = (a, b) => (a === 0 || b === 0 ? 0 : EXP[LOG[a] + LOG[b]]);

/**
 * Generator polynomial ∏(x − α^i) for i in 0…degree−1, returned in
 * DESCENDING degree order so that gen[0] is the leading 1 that rsEncode
 * divides by. (Built ascending, then reversed — the shift-by-one in the inner
 * loop is a multiply by x, which grows the exponent with the index.)
 */
function rsGenerator(degree) {
  let poly = [1];
  for (let i = 0; i < degree; i++) {
    const next = new Array(poly.length + 1).fill(0);
    for (let j = 0; j < poly.length; j++) {
      next[j] ^= gfMul(poly[j], EXP[i]);
      next[j + 1] ^= poly[j];
    }
    poly = next;
  }
  return poly.reverse();
}

function rsEncode(data, ecLen) {
  const gen = rsGenerator(ecLen);
  const res = new Uint8Array(ecLen);
  for (const byte of data) {
    const factor = byte ^ res[0];
    res.copyWithin(0, 1);
    res[ecLen - 1] = 0;
    for (let i = 0; i < ecLen; i++) res[i] ^= gfMul(gen[i + 1], factor);
  }
  return res;
}

/* -------------------------------------------------- version parameters */
// [total data codewords, ec codewords per block, [ [blocks, dataPerBlock], … ]]
const VERSIONS_L = {
  1: [19, 7, [[1, 19]]],
  2: [34, 10, [[1, 34]]],
  3: [55, 15, [[1, 55]]],
  4: [80, 20, [[1, 80]]],
  5: [108, 26, [[1, 108]]],
  6: [136, 18, [[2, 68]]],
  7: [156, 20, [[2, 78]]],
  8: [194, 24, [[2, 97]]],
  9: [232, 30, [[2, 116]]],
  10: [274, 18, [[2, 68], [2, 69]]],
};

const ALIGNMENT = {
  1: [], 2: [6, 18], 3: [6, 22], 4: [6, 26], 5: [6, 30],
  6: [6, 34], 7: [6, 22, 38], 8: [6, 24, 42], 9: [6, 26, 46], 10: [6, 28, 50],
};

const VERSION_INFO = { 7: 0x07c94, 8: 0x085bc, 9: 0x09a99, 10: 0x0a4d3 };

// Format information, ECC level L, masks 0–7 (BCH-encoded and XOR-masked).
const FORMAT_L = [0x77c4, 0x72f3, 0x7daa, 0x789d, 0x662f, 0x6318, 0x6c41, 0x6976];

const PAD_BYTES = [0xec, 0x11];

/* ------------------------------------------------------------ encoding */

function chooseVersion(byteLen) {
  for (let v = 1; v <= 10; v++) {
    const [dataCw] = VERSIONS_L[v];
    const countBits = v < 10 ? 8 : 16;
    const needed = Math.ceil((4 + countBits + byteLen * 8) / 8);
    if (needed <= dataCw) return v;
  }
  throw new Error(`QR: ${byteLen} bytes exceeds the 274-byte limit of this encoder`);
}

function buildDataCodewords(bytes, version) {
  const [dataCw] = VERSIONS_L[version];
  const countBits = version < 10 ? 8 : 16;
  const bits = [];
  const push = (value, len) => {
    for (let i = len - 1; i >= 0; i--) bits.push((value >> i) & 1);
  };

  push(0b0100, 4); // byte mode
  push(bytes.length, countBits);
  for (const b of bytes) push(b, 8);

  // Terminator, then pad to a byte boundary, then alternating pad bytes.
  const capacityBits = dataCw * 8;
  push(0, Math.min(4, capacityBits - bits.length));
  while (bits.length % 8) bits.push(0);

  const out = new Uint8Array(dataCw);
  for (let i = 0; i < bits.length; i += 8) {
    let byte = 0;
    for (let j = 0; j < 8; j++) byte = (byte << 1) | bits[i + j];
    out[i / 8] = byte;
  }
  for (let i = bits.length / 8, p = 0; i < dataCw; i++, p++) out[i] = PAD_BYTES[p % 2];
  return out;
}

/** Split into blocks, RS-encode each, then interleave data then EC. */
function interleave(dataCodewords, version) {
  const [, ecLen, groups] = VERSIONS_L[version];
  const dataBlocks = [];
  const ecBlocks = [];
  let offset = 0;
  for (const [count, size] of groups) {
    for (let i = 0; i < count; i++) {
      const block = dataCodewords.subarray(offset, offset + size);
      offset += size;
      dataBlocks.push(block);
      ecBlocks.push(rsEncode(block, ecLen));
    }
  }

  const out = [];
  const maxData = Math.max(...dataBlocks.map((b) => b.length));
  for (let i = 0; i < maxData; i++) {
    for (const b of dataBlocks) if (i < b.length) out.push(b[i]);
  }
  for (let i = 0; i < ecLen; i++) {
    for (const b of ecBlocks) out.push(b[i]);
  }
  return Uint8Array.from(out);
}

/* -------------------------------------------------------------- matrix */

function newMatrix(size) {
  return Array.from({ length: size }, () => new Int8Array(size).fill(-1));
}

function placeFunctionPatterns(m, version) {
  const size = m.length;

  const finder = (row, col) => {
    for (let r = -1; r <= 7; r++) {
      for (let c = -1; c <= 7; c++) {
        const rr = row + r;
        const cc = col + c;
        if (rr < 0 || rr >= size || cc < 0 || cc >= size) continue;
        const inRing = r >= 0 && r <= 6 && c >= 0 && c <= 6;
        const dark =
          inRing && (r === 0 || r === 6 || c === 0 || c === 6 || (r >= 2 && r <= 4 && c >= 2 && c <= 4));
        m[rr][cc] = dark ? 1 : 0;
      }
    }
  };
  finder(0, 0);
  finder(0, size - 7);
  finder(size - 7, 0);

  // Timing patterns
  for (let i = 8; i < size - 8; i++) {
    m[6][i] = m[i][6] = i % 2 === 0 ? 1 : 0;
  }

  // Alignment patterns (skipping the three finder corners)
  const centers = ALIGNMENT[version];
  for (const r of centers) {
    for (const c of centers) {
      if ((r === 6 && c === 6) || (r === 6 && c === size - 7) || (r === size - 7 && c === 6)) continue;
      for (let dr = -2; dr <= 2; dr++) {
        for (let dc = -2; dc <= 2; dc++) {
          m[r + dr][c + dc] = Math.max(Math.abs(dr), Math.abs(dc)) !== 1 ? 1 : 0;
        }
      }
    }
  }

  m[size - 8][8] = 1; // dark module

  // Reserve format areas (filled later)
  for (let i = 0; i < 9; i++) {
    if (m[8][i] === -1) m[8][i] = 0;
    if (m[i][8] === -1) m[i][8] = 0;
  }
  for (let i = 0; i < 8; i++) {
    if (m[8][size - 1 - i] === -1) m[8][size - 1 - i] = 0;
    if (m[size - 1 - i][8] === -1) m[size - 1 - i][8] = 0;
  }

  // Version information (v ≥ 7)
  if (version >= 7) {
    const info = VERSION_INFO[version];
    for (let i = 0; i < 18; i++) {
      const bit = (info >> i) & 1;
      m[Math.floor(i / 3)][size - 11 + (i % 3)] = bit;
      m[size - 11 + (i % 3)][Math.floor(i / 3)] = bit;
    }
  }
}

/** Which modules carry data — everything left as -1 after function patterns. */
function placeData(m, codewords, reserved) {
  const size = m.length;
  let bitIndex = 0;
  let upward = true;
  for (let col = size - 1; col > 0; col -= 2) {
    if (col === 6) col--; // skip the vertical timing column
    for (let i = 0; i < size; i++) {
      const row = upward ? size - 1 - i : i;
      for (const c of [col, col - 1]) {
        if (reserved[row][c]) continue;
        const byte = codewords[bitIndex >> 3];
        const bit = byte === undefined ? 0 : (byte >> (7 - (bitIndex & 7))) & 1;
        m[row][c] = bit;
        bitIndex++;
      }
    }
    upward = !upward;
  }
}

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

function penalty(m) {
  const size = m.length;
  let score = 0;

  // Rule 1: runs of 5+ same-colour modules in a row/column.
  for (const transpose of [false, true]) {
    for (let a = 0; a < size; a++) {
      let run = 1;
      for (let b = 1; b < size; b++) {
        const cur = transpose ? m[b][a] : m[a][b];
        const prev = transpose ? m[b - 1][a] : m[a][b - 1];
        if (cur === prev) {
          run++;
          if (run === 5) score += 3;
          else if (run > 5) score += 1;
        } else run = 1;
      }
    }
  }

  // Rule 2: 2×2 blocks of one colour.
  for (let r = 0; r < size - 1; r++) {
    for (let c = 0; c < size - 1; c++) {
      const v = m[r][c];
      if (v === m[r][c + 1] && v === m[r + 1][c] && v === m[r + 1][c + 1]) score += 3;
    }
  }

  // Rule 3: 1:1:3:1:1 finder-like patterns with 4 light modules on a side.
  const P1 = [1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0];
  const P2 = [0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1];
  for (const transpose of [false, true]) {
    for (let a = 0; a < size; a++) {
      for (let b = 0; b + 11 <= size; b++) {
        let m1 = true;
        let m2 = true;
        for (let k = 0; k < 11; k++) {
          const v = transpose ? m[b + k][a] : m[a][b + k];
          if (v !== P1[k]) m1 = false;
          if (v !== P2[k]) m2 = false;
        }
        if (m1 || m2) score += 40;
      }
    }
  }

  // Rule 4: deviation from a 50/50 dark ratio.
  let dark = 0;
  for (const row of m) for (const v of row) dark += v;
  const percent = (dark * 100) / (size * size);
  score += Math.floor(Math.abs(percent - 50) / 5) * 10;

  return score;
}

function applyFormat(m, mask) {
  const size = m.length;
  const bits = FORMAT_L[mask];
  for (let i = 0; i < 15; i++) {
    const bit = (bits >> i) & 1;
    // Top-left copy
    if (i < 6) m[8][i] = bit;
    else if (i === 6) m[8][7] = bit;
    else if (i === 7) m[8][8] = bit;
    else if (i === 8) m[7][8] = bit;
    else m[14 - i][8] = bit;
    // Split copy
    if (i < 8) m[size - 1 - i][8] = bit;
    else m[8][size - 15 + i] = bit;
  }
}

/**
 * @param {string} text
 * @returns {{size:number, modules:boolean[][], version:number}}
 */
export function encode(text) {
  const bytes = new TextEncoder().encode(String(text));
  const version = chooseVersion(bytes.length);
  const size = 17 + version * 4;

  const reserved = newMatrix(size);
  placeFunctionPatterns(reserved, version);
  const isReserved = reserved.map((row) => row.map((v) => v !== -1));

  const codewords = interleave(buildDataCodewords(bytes, version), version);

  let best = null;
  for (let mask = 0; mask < 8; mask++) {
    const m = reserved.map((row) => Int8Array.from(row));
    placeData(m, codewords, isReserved);
    for (let r = 0; r < size; r++) {
      for (let c = 0; c < size; c++) {
        if (!isReserved[r][c] && MASKS[mask](r, c)) m[r][c] ^= 1;
      }
    }
    applyFormat(m, mask);
    const score = penalty(m);
    if (!best || score < best.score) best = { score, m };
  }

  return {
    version,
    size,
    modules: best.m.map((row) => Array.from(row, (v) => v === 1)),
  };
}

/**
 * Render to an SVG string. One path for all dark modules keeps it tiny and
 * crisp at any size; `currentColor` lets CSS theme it.
 */
export function toSVG(text, { margin = 4, className = 'qr' } = {}) {
  const { modules, size } = encode(text);
  const dim = size + margin * 2;
  let d = '';
  for (let r = 0; r < size; r++) {
    for (let c = 0; c < size; c++) {
      if (modules[r][c]) d += `M${c + margin} ${r + margin}h1v1h-1z`;
    }
  }
  return (
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${dim} ${dim}" ` +
    `class="${className}" shape-rendering="crispEdges" role="img" ` +
    `aria-label="QR code"><rect width="${dim}" height="${dim}" fill="#fff"/>` +
    `<path d="${d}" fill="#000"/></svg>`
  );
}
