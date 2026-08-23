# Security model

## Assets
1. Ability to inject keystrokes into the host PC (includes typing credentials).
2. Ability to actuate the physical power button.
3. The login secret itself, if the user stores an unlock sequence in the PWA.

## Trust boundaries
- **Phone (PWA)** — trusted. Holds the Ed25519 private key (WebCrypto,
  non-extractable where the platform supports it) behind a WebAuthn user
  presence check.
- **Relay** — *untrusted for integrity and authority*, trusted only for
  availability. It sees command plaintext (action names and args — note:
  `type` args contain the typed text; see mitigation below) but cannot forge,
  modify, or replay commands past the device's verification.
- **Device (ESP32-S3)** — trusted. Verifies every command independently.
- **Home network** — untrusted after pairing. The pairing endpoint exists only
  in SoftAP setup mode on an isolated AP with a printed password.

## Properties & mitigations

| Threat | Mitigation |
|---|---|
| Relay compromise → forged commands | Ed25519 signature verified on-device; relay holds no keys. |
| Replay (relay or network) | Strictly increasing per-key counter persisted in NVS **before** execution + ±90 s timestamp window. |
| Stolen `deviceToken` | Grants relay access only → attacker can probe presence / spam invalid commands, all rejected at step 3 of verification; rate-limited on device and relay. |
| Stolen phone | WebAuthn (biometric) gate before signing; key non-extractable. Revocation: re-enter setup mode, remove key slot. |
| Relay reads typed password (`type` action) | v1 accepts this residual risk and documents it; **recommended usage**: store the unlock text only on the phone, prefer short PIN logins, run your own relay. v2 candidate: XChaCha20-Poly1305 payload sealing to the device's X25519 key, established at pairing. |
| Evil-maid pairing | Pairing only in setup mode (physical button hold), AP password printed on device/serial only. |
| Command tampering in flight | Signature covers all semantic fields; TLS on both hops. |
| Device clock skew breaking ts window | SNTP on boot + periodic resync; commands rejected as `stale` include device time in the error so the app can warn. |
| DoS on relay | Cloudflare edge, token gate, token-bucket per device, 4 KiB body cap. |

## Firmware hardening checklist
- No pairing/HTTP server outside setup mode.
- NVS encryption + flash encryption + secure boot documented as the
  production profile in `firmware/README.md` (off by default for dev).
- Counter persistence is write-then-execute; a power loss can only skip
  counters, never allow reuse.
- Constant-time comparisons for token and signature checks (mbedtls).
- Watchdog on the relay-client task; GPIO defaults to hi-Z/idle on boot and
  panic (power pin can never latch on).
