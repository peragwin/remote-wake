# Security model

## Assets
1. Ability to inject keystrokes into the host PC (includes typing credentials).
   Remote typing (`type` / `keys`) exists only in firmware built with
   `CONFIG_RW_ACT_KEYBOARD_INPUT`, which is off by default; `wake` always remains.
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
| Stolen phone | WebAuthn (biometric) gate before signing; key non-extractable. Revocation: factory-reset the device and re-pair (see [Suspected compromise](#suspected-compromise)). |
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

## Suspected compromise

Use this if the PWA's origin (the S3 bucket or CloudFront distribution behind
https://remote-wake.wryskware.dev/) may have served modified code, or a paired
phone may be in someone else's hands.

**Why act at the device, not the phone.** Code running on the app's origin can
ask the phone's key to sign commands for *future* timestamps and counters, and
keep them. Each one passes verification once its `ts` is within ±90 s of the
device clock and its `ctr` is above the slot's high-water mark, so a stash of
pre-signed commands stays usable after the bad code is gone, after the phone is
wiped, and after a new app is deployed. Only removing the key from the device
invalidates them, because the device then has no key to verify them against.

1. **Remove the phone's key from the device.** The firmware and setup page do
   not currently offer removing a single key slot, so this means a factory
   reset: hold the device's BOOT button for 15 s (`CONFIG_RW_FACTORY_HOLD_MS`;
   5 s only enters setup mode, which keeps every key). This erases the
   `rw_cfg` NVS namespace: all four key slots and their counters, the Wi-Fi
   credentials, the relay override and the deviceId/deviceToken. The device
   comes back with a new deviceId, so the relay treats it as a new device and
   the old token is useless. Every paired phone has to pair again.
2. **Clear the phone's site data** for the app origin (browser settings →
   site data for `remote-wake.wryskware.dev`, or remove the installed app and
   its data). This deletes the old signing key, counters, stored unlock text
   and the service worker that may be serving cached code.
3. **Audit the S3 bucket** for objects that the repository does not produce:

   ```sh
   aws s3 ls --recursive s3://wryskware-remote-wake-app
   ```

   The deploy workflow uploads exactly `index.html`, `styles.css`,
   `manifest.webmanifest`, `sw.js`, and the files under `app/src/`,
   `app/vendor/` and `app/icons/` (`git ls-files app/src app/vendor app/icons`).
   It never deletes, so files dropped from the repository can linger; anything
   else, or a listed file whose content differs from `main` (only `sw.js` is
   rewritten, for its `CACHE_VERSION`), is suspect. Remove what does not belong,
   review CloudTrail for who wrote it, redeploy from `main` with the
   Deploy website workflow (which invalidates CloudFront), and only then
   continue.
4. **Re-pair** each phone from the cleaned origin. After the reset the device
   is unprovisioned and starts in setup mode by itself; enter Wi-Fi and pair
   as in [setup.md](setup.md#3-pair-from-your-phone). If a relay override was
   set before, enter it again. Check the relay
   host the app shows before confirming.

If only the relay or a `deviceToken` is suspect, re-keying the device is not
needed: the relay cannot forge commands. A factory reset still rotates the
token. Re-entering the unlock text afterwards is a good moment to change that
password if it ever went through a relay you no longer trust.
