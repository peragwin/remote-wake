# Implementation plan

Status legend: ☐ todo · ◐ in progress · ☑ done

## Phase 0 — contracts (this commit)
- ☑ Repo skeleton, README
- ☑ Protocol spec (`docs/protocol.md`) — the source of truth for all components
- ☑ Security model (`docs/security.md`)
- ☑ Hardware design (`hardware/README.md`)

## Phase 1 — components (parallel)

### firmware/ (ESP-IDF ≥ 5.2, target esp32s3; power-only profile for esp32/c3)
- ☐ Project scaffolding: `CMakeLists.txt`, `sdkconfig.defaults`, `partitions.csv`
- ☐ `config` — NVS-backed settings (wifi, relay url, deviceId/token, operator pubkeys, ctr high-water marks)
- ☐ `usb_hid` — TinyUSB HID keyboard: wake, `type` (US layout), `keys` chords; report USB state (mounted/suspended)
- ☐ `power_btn` — GPIO pulse driver (active-drive with bounds from protocol; default GPIO 5), safe idle state, mutual exclusion
- ☐ `auth` — signing-string builder, Ed25519 verify (mbedtls), ts window, per-kid counter persistence (write ctr BEFORE executing)
- ☐ `relay_client` — WSS with bearer header, hello, dispatch, responses, backoff+jitter, SNTP
- ☐ `provisioning` — SoftAP + HTTP setup flow incl. `/pair` (setup mode only), factory reset via BOOT hold
- ☐ `main` — wiring it together, status LED patterns

### relay/ (Cloudflare Worker + Durable Object, TypeScript)
- ☑ Worker routing `/v1/send`, `/v1/presence`, `/v1/device` (WS upgrade) → per-device Durable Object
- ☑ DO: single device WS, token check (constant-time), pending-command map (id → resolver, 10 s timeout), presence
- ☑ Limits: 4 KiB body, token bucket rate limit, no persistence of payloads
- ☑ `wrangler.toml`, deploy docs, vitest unit tests for routing/limits

### app/ (PWA, no framework build step — vanilla TS/JS, single deployable dir)
- ☐ Sleek dark UI: device card (online dot, RSSI), big WAKE, unlock-sequence composer, power tap / press-and-hold with 2-step confirm on hold
- ☐ Crypto: WebCrypto Ed25519 (fallback @noble/ed25519 vendored), non-extractable where supported; counter persistence; signing string per spec
- ☐ WebAuthn (platform authenticator) gate before any signing
- ☐ Pairing flow UI (setup-mode page is served by firmware; app imports the returned JSON blob / QR)
- ☐ PWA: manifest, service worker, installable, offline shell

## Phase 2 — integration
- ☐ Cross-check all three signing-string implementations against shared test vectors (`docs/test-vectors.json`)
- ☐ CI: build firmware (idf docker), typecheck+test relay, lint app
- ☐ End-to-end walkthrough doc (`docs/setup.md`)

## Deliberate non-goals (v1)
- OTA updates, multiple devices per relay token rotation UI, signed responses,
  non-US keyboard layouts, BLE fallback for classic ESP32.
