# app

Installable PWA controller. No build step — static files deployable to any
static host (Cloudflare Pages, GitHub Pages) or servable from the relay.

- Ed25519 signing per `docs/protocol.md` (WebCrypto; vendored @noble/ed25519
  fallback for browsers without Ed25519 WebCrypto)
- WebAuthn platform-authenticator gate before any signing
- Counter + device config persisted in IndexedDB
- Dark, glassy UI: device presence card, WAKE, unlock composer,
  power tap / press-and-hold (with confirm) — see PLAN.md

Dev server: `npx serve app/` (or any static server, must be https or
localhost for WebCrypto/WebAuthn).
