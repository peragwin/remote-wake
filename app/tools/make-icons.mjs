/**
 * Icon generator — run once, output is committed. `npm run icons`
 *
 * Motif: a power glyph (ring with a gap at 12 o'clock plus a vertical stem)
 * with radio waves arcing out to either side — "power, at a distance".
 *
 * Everything is described parametrically in unit coordinates below, so the SVG
 * and the PNG rasters are guaranteed to be the same drawing. PNGs are written
 * with a hand-rolled encoder over node:zlib (no image dependencies) using 4×4
 * supersampling for clean edges — the shapes are all distance fields, so this
 * is cheap and exact.
 */

import { deflateSync } from 'node:zlib';
import { writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const OUT = join(dirname(fileURLToPath(import.meta.url)), '..', 'icons');

/* --------------------------------------------------------- the drawing */

const BG_TOP = [11, 14, 26];      // #0b0e1a
const BG_BOTTOM = [23, 18, 48];   // #171230
const GLYPH = [126, 231, 255];    // #7ee7ff  cyan
const WAVE = [124, 138, 255];     // #7c8affx indigo

/** Geometry in unit space (0..1), for a glyph occupying `scale` of the canvas. */
function geometry(scale) {
  const c = 0.5;
  const s = scale;
  return {
    c,
    ring: { r: 0.205 * s, w: 0.062 * s, gapHalfAngle: 0.62 },
    stem: { y0: c - 0.295 * s, y1: c - 0.075 * s, w: 0.062 * s },
    waves: [
      { r: 0.315 * s, w: 0.048 * s, halfAngle: 0.62 },
      { r: 0.415 * s, w: 0.048 * s, halfAngle: 0.5 },
    ],
  };
}

/** Signed coverage helpers — all shapes are distance fields. */
function inArc(dx, dy, r, w, centerAngle, halfAngle) {
  const dist = Math.hypot(dx, dy);
  if (Math.abs(dist - r) > w / 2) return false;
  let a = Math.atan2(dy, dx) - centerAngle;
  while (a > Math.PI) a -= 2 * Math.PI;
  while (a < -Math.PI) a += 2 * Math.PI;
  return Math.abs(a) <= halfAngle;
}

function inCapsule(x, y, x0, y0, x1, y1, w) {
  const vx = x1 - x0;
  const vy = y1 - y0;
  const len2 = vx * vx + vy * vy;
  const t = len2 === 0 ? 0 : Math.max(0, Math.min(1, ((x - x0) * vx + (y - y0) * vy) / len2));
  return Math.hypot(x - (x0 + t * vx), y - (y0 + t * vy)) <= w / 2;
}

/**
 * Sample the icon at a point in unit space.
 * @returns {[r,g,b] | null} colour of the topmost shape, or null for background
 */
function sample(x, y, g) {
  const dx = x - g.c;
  const dy = y - g.c;

  // Power ring: full circle minus a gap centred on 12 o'clock (−π/2).
  const distFromRing = Math.abs(Math.hypot(dx, dy) - g.ring.r);
  if (distFromRing <= g.ring.w / 2) {
    let a = Math.atan2(dy, dx) + Math.PI / 2; // 0 at the top
    while (a > Math.PI) a -= 2 * Math.PI;
    while (a < -Math.PI) a += 2 * Math.PI;
    if (Math.abs(a) > g.ring.gapHalfAngle) return GLYPH;
  }

  // Power stem.
  if (inCapsule(x, y, g.c, g.stem.y0, g.c, g.stem.y1, g.stem.w)) return GLYPH;

  // Radio waves, left and right.
  for (const w of g.waves) {
    if (inArc(dx, dy, w.r, w.w, 0, w.halfAngle)) return WAVE;
    if (inArc(dx, dy, w.r, w.w, Math.PI, w.halfAngle)) return WAVE;
  }
  return null;
}

/* ------------------------------------------------------------ PNG codec */

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(buf) {
  let c = 0xffffffff;
  for (const b of buf) c = CRC_TABLE[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length);
  const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(body));
  return Buffer.concat([len, body, crc]);
}

/** @param {Uint8Array} rgba length = w*h*4 */
function encodePNG(rgba, w, h) {
  const raw = Buffer.alloc(h * (w * 4 + 1));
  for (let y = 0; y < h; y++) {
    raw[y * (w * 4 + 1)] = 0; // filter: none
    Buffer.from(rgba.buffer, rgba.byteOffset + y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0);
  ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 6; // colour type: RGBA
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', deflateSync(raw, { level: 9 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

/* ------------------------------------------------------------ rasterise */

function raster(size, { scale = 1, rounded = true, ss = 4 } = {}) {
  const g = geometry(scale);
  const px = new Uint8Array(size * size * 4);
  const radius = rounded ? 0.21 : 0; // squircle-ish corner in unit space

  for (let py = 0; py < size; py++) {
    for (let pxi = 0; pxi < size; pxi++) {
      let r = 0;
      let gg = 0;
      let b = 0;
      let a = 0;
      for (let sy = 0; sy < ss; sy++) {
        for (let sx = 0; sx < ss; sx++) {
          const x = (pxi + (sx + 0.5) / ss) / size;
          const y = (py + (sy + 0.5) / ss) / size;

          // Rounded-rect clip for the plain icon; maskable icons stay square.
          if (radius > 0) {
            const qx = Math.max(radius - Math.min(x, 1 - x), 0);
            const qy = Math.max(radius - Math.min(y, 1 - y), 0);
            if (Math.hypot(qx, qy) > radius) continue;
          }

          const t = y; // vertical gradient
          const bg = [
            BG_TOP[0] + (BG_BOTTOM[0] - BG_TOP[0]) * t,
            BG_TOP[1] + (BG_BOTTOM[1] - BG_TOP[1]) * t,
            BG_TOP[2] + (BG_BOTTOM[2] - BG_TOP[2]) * t,
          ];
          const shape = sample(x, y, g);
          const col = shape || bg;
          r += col[0];
          gg += col[1];
          b += col[2];
          a += 255;
        }
      }
      const n = ss * ss;
      const i = (py * size + pxi) * 4;
      const cov = a / n / 255;
      px[i] = cov ? Math.round(r / (a / 255)) : 0;
      px[i + 1] = cov ? Math.round(gg / (a / 255)) : 0;
      px[i + 2] = cov ? Math.round(b / (a / 255)) : 0;
      px[i + 3] = Math.round(a / n);
    }
  }
  return encodePNG(px, size, size);
}

/* ------------------------------------------------------------------ SVG */

const rgb = ([r, g, b]) => `rgb(${r},${g},${b})`;

/** Arc path between two angles at a given radius (stroked, not filled). */
function arcPath(cx, cy, r, from, to) {
  const x0 = cx + r * Math.cos(from);
  const y0 = cy + r * Math.sin(from);
  const x1 = cx + r * Math.cos(to);
  const y1 = cy + r * Math.sin(to);
  const large = Math.abs(to - from) > Math.PI ? 1 : 0;
  return `M${x0.toFixed(3)} ${y0.toFixed(3)}A${r.toFixed(3)} ${r.toFixed(3)} 0 ${large} 1 ${x1.toFixed(3)} ${y1.toFixed(3)}`;
}

function svg(size, { scale = 1, rounded = true } = {}) {
  const g = geometry(scale);
  const S = (v) => (v * size).toFixed(2);
  const cx = g.c * size;
  const cy = g.c * size;
  const gap = g.ring.gapHalfAngle;

  const ring = arcPath(cx, cy, g.ring.r * size, -Math.PI / 2 + gap, -Math.PI / 2 - gap + 2 * Math.PI);

  const waves = g.waves
    .flatMap((w) => [
      arcPath(cx, cy, w.r * size, -w.halfAngle, w.halfAngle),
      arcPath(cx, cy, w.r * size, Math.PI - w.halfAngle, Math.PI + w.halfAngle),
    ])
    .map(
      (d, i) =>
        `    <path d="${d}" stroke="${rgb(WAVE)}" stroke-width="${S(g.waves[Math.floor(i / 2)].w)}" ` +
        `stroke-linecap="round" fill="none" opacity="${0.92 - Math.floor(i / 2) * 0.22}"/>`
    )
    .join('\n');

  const clip = rounded
    ? `  <rect width="${size}" height="${size}" rx="${S(0.21)}" fill="url(#bg)"/>`
    : `  <rect width="${size}" height="${size}" fill="url(#bg)"/>`;

  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${size} ${size}" width="${size}" height="${size}" role="img" aria-label="remote-wake">
  <title>remote-wake</title>
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="${rgb(BG_TOP)}"/>
      <stop offset="1" stop-color="${rgb(BG_BOTTOM)}"/>
    </linearGradient>
    <filter id="glow" x="-40%" y="-40%" width="180%" height="180%">
      <feGaussianBlur stdDeviation="${S(0.018)}" result="b"/>
      <feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>
  </defs>
${clip}
  <g fill="none">
${waves}
  </g>
  <g filter="url(#glow)">
    <path d="${ring}" stroke="${rgb(GLYPH)}" stroke-width="${S(g.ring.w)}" stroke-linecap="round" fill="none"/>
    <path d="M${cx.toFixed(2)} ${S(g.stem.y0)}V${S(g.stem.y1)}" stroke="${rgb(GLYPH)}" stroke-width="${S(g.stem.w)}" stroke-linecap="round"/>
  </g>
</svg>
`;
}

/* ----------------------------------------------------------------- main */

mkdirSync(OUT, { recursive: true });

const outputs = [
  ['icon.svg', () => svg(512, { scale: 1, rounded: true })],
  // Maskable: the glyph is shrunk into the 80% safe zone, background is square.
  ['maskable.svg', () => svg(512, { scale: 0.72, rounded: false })],
  ['icon-192.png', () => raster(192, { scale: 1, rounded: true })],
  ['icon-512.png', () => raster(512, { scale: 1, rounded: true })],
  ['maskable-512.png', () => raster(512, { scale: 0.72, rounded: false })],
  ['apple-touch-icon.png', () => raster(180, { scale: 1, rounded: false })],
];

for (const [name, make] of outputs) {
  const data = make();
  writeFileSync(join(OUT, name), data);
  console.log(`${name.padEnd(24)} ${String(Buffer.byteLength(data)).padStart(7)} bytes`);
}
console.log('\nIcons written to app/icons/');
