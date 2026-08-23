# app

Installable PWA controller. **No build step** — plain ES modules and one
stylesheet, deployable to any static host (Cloudflare Pages, GitHub Pages) or
servable from the relay. ~260 KB total, no runtime dependencies except one
vendored crypto fallback.

```
npm test          # 53 tests: signing vectors, QR, pairing, static shell
npm run serve     # http://localhost:8080 (localhost is a secure context)
npm run icons     # regenerate app/icons/ (already committed)
```

---

## Layout

| Path | What it is |
|---|---|
| `index.html` | Whole app shell — three views (pair / home / settings) plus toast + sheet overlays |
| `styles.css` | Dark glass design system |
| `sw.js` | Service worker: cache-first app shell, **network-only** for the relay |
| `manifest.webmanifest` | Installability, icons, a "Wake" shortcut |
| `src/signing.js` | **The contract.** Canonical args + signing string. Dependency-free, isomorphic |
| `src/crypto.js` | Ed25519 key generation and signing (WebCrypto, noble fallback) |
| `src/store.js` | IndexedDB: devices, key, per-(device,slot) counters, settings |
| `src/api.js` | Relay transport, presence poller, `err` → human text |
| `src/commands.js` | Orchestration: gate → counter → sign → send. The only path to a command |
| `src/webauthn.js` | Local user-presence gate |
| `src/pairing.js` | Parse/validate the device's pairing blob |
| `src/keys.js` | Key-chord presets and combo parsing |
| `src/qr.js` | Minimal QR encoder (byte mode, ECC L, v1–10) |
| `src/ui.js` | Toasts, bottom sheets, press-and-hold controller |
| `src/app.js` | View wiring and rendering |
| `vendor/noble-ed25519.js` | Vendored `@noble/ed25519` **v2.3.0** (MIT), unmodified |
| `tools/make-icons.mjs` | Parametric icon generator → SVG + PNG (pure `node:zlib`) |
| `tools/serve.mjs` | Zero-dependency dev server with correct MIME types |

---

## Protocol conformance

`src/signing.js` implements `docs/protocol.md` §Signing string exactly:

```
remote-wake-v1\n<dev>\n<id>\n<ts>\n<ctr>\n<act>\n<args-json>
```

`<args-json>` is `JSON.stringify` semantics with **recursively sorted keys and
no whitespace**. The same canonical bytes are spliced into the envelope on the
wire by `serializeEnvelope()` rather than re-stringifying the object, so the
device re-serializes and gets byte-identical input.

`test/vectors.test.mjs` checks all five vectors in `docs/test-vectors.json`:
the signing strings match byte-for-byte, the vendored noble reproduces every
signature from the seed, `node:crypto` independently agrees, and tampering with
any covered field breaks verification.

### Key slots vs. this phone's key

The phone holds **one** Ed25519 key. The protocol's `kid` is a **slot on a
device** (`p1`…`p4`), assigned by each device at pairing — the same phone key
can be `p1` on one device and `p3` on another. So `kid` lives on the device
record, and the counter is scoped to `(deviceId, kid)` because each device
keeps its own high-water mark per slot.

> **Note for firmware:** v1's `/pair` reply is
> `{deviceId, deviceToken, relayUrl}` and does **not** report which slot the key
> landed in. The app defaults to `p1` and exposes a slot selector in Settings
> for the multi-operator case. If the reply gains a `"kid"` field, the app
> already reads and validates it — no app change needed.

---

## Crypto

Two backends, feature-detected once at key generation:

| | Storage | Notes |
|---|---|---|
| **WebCrypto** (preferred) | non-extractable `CryptoKey` in IndexedDB | `generateKey({name:'Ed25519'}, false, …)`. Raw private bytes never exist in JS; XSS or a device backup cannot exfiltrate the key. |
| **noble** (fallback) | 32-byte seed as raw bytes in IndexedDB | **Weaker** — anything with script access to this origin can read and copy the key. Used only where WebCrypto lacks Ed25519 (Safari < 17, older Chrome). Surfaced in Settings so the user can see which one is active. |

Detection probes end-to-end (generate *and* sign), because some engines expose
the algorithm name and then reject the operation.

---

## Security posture

Implements `docs/security.md`'s "phone is trusted" boundary:

- **WebAuthn gate.** A fresh `credentials.get` assertion with
  `userVerification: 'required'` is required before signing. The assertion is
  **discarded** — it is a local presence check, not part of the protocol, and
  is never transmitted. Registration uses a platform authenticator and skips
  gracefully when none exists (Settings says so plainly). The toggle is there,
  and turning it *off* requires confirming what you lose.
  - The gate also guards *saving* the unlock text, since the secret is the
    asset, not just its use.
  - **Deliberate exception:** the read-only `status`/`ping` refresh skips the
    gate, so pulling to refresh does not demand a fingerprint. Every action
    that types, presses keys or touches power always goes through it.
- **Counter.** `nextCounter()` increments **and awaits the IndexedDB write**
  before the command is signed. A crash can only burn counter values, never
  reuse one. A `replay` rejection carrying the device's `lastCtr` fast-forwards
  the local counter so the next command succeeds.
- **Unlock text.** Stored only on the phone, behind the gate, with a permanent
  amber warning in Settings stating that the relay sees `type` args in the
  clear in v1 and recommending a short PIN and a self-hosted relay.
- **Service worker.** Never caches anything under `/v1/`, never touches
  non-GET requests. A cached command or a stale "online" would both be wrong.
- **Pairing input** is validated strictly: 16 lowercase hex `deviceId`,
  43-character base64url `deviceToken`, https relay URL (localhost allowed for
  development).

---

## Design

Phone-first at ~390 px, comfortable to desktop (the column widens, nothing
reflows). Dark glass: layered translucent surfaces over a fixed aurora
gradient, hairline borders with a gradient top edge, one cyan/indigo accent
ramp and red reserved exclusively for power-off.

- **WAKE** is a 216 px conic-gradient disc — the obvious target, reachable by
  thumb, with a spring scale on press and concentric "radio wave" rings while
  in flight. Success pops a green halo; failure shakes.
- **Power off** is two-stage: hold the button for a real 1.5 s (a progress ring
  driven by rAF tracks it, and releasing early aborts) and *then* confirm in a
  sheet naming the consequence. Accidental power-off should be effectively
  impossible.
- **Presence** is a pulsing dot with a since-time; RSSI, USB state and firmware
  version fill in from a `status` fetch.
- Micro-interactions throughout: spring-in toasts and sheets, chip pop-in,
  active-state scale, haptics (toggleable).
- `prefers-reduced-motion` collapses every decorative animation to ~0 ms. The
  hold progress ring deliberately survives, because it is information: it tells
  you how much longer to hold.
- Type is the system stack with tightened tracking on headings and tabular
  numerals for counters. Inputs are ≥16 px so iOS does not zoom on focus.
- Icons are generated parametrically (`tools/make-icons.mjs`) — a power glyph
  with radio waves — as SVG plus 192/512/maskable/apple-touch PNGs written by a
  hand-rolled encoder over `node:zlib`. No image dependency.

---

## Testing

`npm test` → `node --test`, no devDependencies.

| File | Covers |
|---|---|
| `test/vectors.test.mjs` | Signing strings byte-for-byte vs `docs/test-vectors.json`; noble and `node:crypto` both reproduce every signature; canonical-JSON edge cases; tamper detection |
| `test/qr.test.mjs` | Independently **decodes** the generated matrix — reads format bits, un-masks, de-interleaves, verifies Reed-Solomon parity algebraically, reads the payload back — across all 10 versions |
| `test/pairing.test.mjs` | Pairing-blob validation, chord parsing, presets, limits vs the spec |
| `test/shell.test.mjs` | Every module import and asset reference resolves; manifest icons exist; the service worker precaches the whole graph and excludes `/v1/`; no external network references; no duplicate/missing element ids; balanced HTML |

The shell was additionally verified over HTTP with `tools/serve.mjs`: every
referenced asset returns 200 with the right content type.

**Not covered:** no browser ran this code — the sandbox could not download
Chromium. IndexedDB, WebCrypto, WebAuthn and service-worker paths are reviewed
and type-consistent but unexercised. First run on a real phone should confirm
key generation, the pairing round-trip and the install prompt.

---

## Deploying

Any static host. Must be **https or localhost** — WebCrypto, WebAuthn and
service workers all require a secure context. Point `relayUrl` at your relay
during pairing; the relay must send permissive CORS headers for the app's
origin (`POST /v1/send/*`, `GET /v1/presence/*`, `Authorization` header).
