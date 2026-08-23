# remote-wake

Wake, unlock, and power-cycle your computer from anywhere — with an ESP32-S3
acting as a USB keyboard and a hardware finger on your power button.

```
 ┌──────────┐   HTTPS/WSS   ┌─────────┐   WSS (outbound)   ┌──────────────┐  USB HID   ┌──────────┐
 │  Phone   │ ────────────▶ │  Relay  │ ◀───────────────── │   ESP32-S3   │ ─────────▶ │    PC    │
 │  (PWA)   │  signed cmds  │ (dumb)  │    signed cmds     │   firmware   │ ─── GPIO ─▶│ PWR_SW ⏻ │
 └──────────┘               └─────────┘                    └──────────────┘  optocoupler└──────────┘
```

- **Wake from sleep** — the ESP32-S3 enumerates as a real USB HID keyboard;
  a keypress wakes the host (enable *"Allow this device to wake the computer"*).
- **Type your login** — send keystroke sequences end-to-end encrypted-in-intent:
  every command is Ed25519-signed by your phone; the relay can't read or forge them.
- **Physically press the power button** — a GPIO drives an optocoupler wired
  across the motherboard front-panel `PWR_SW` header. Tap to power on, hold to
  force off. Works even when the OS is wedged.
- **No port forwarding** — the device dials *out* to a tiny zero-knowledge relay
  (Cloudflare Worker). Your phone talks to the same relay.

## Repo layout

| Path        | What                                                        |
|-------------|-------------------------------------------------------------|
| `firmware/` | ESP-IDF project for ESP32-S3 (TinyUSB HID, WSS client, Ed25519 verify, GPIO) |
| `relay/`    | Cloudflare Worker + Durable Object rendezvous relay (TypeScript) |
| `app/`      | Installable PWA controller (WebCrypto Ed25519, WebAuthn gate) |
| `hardware/` | Wiring, BOM, optocoupler circuit                            |
| `docs/`     | Protocol spec, security model, setup guide                  |

## Security in one paragraph

Your phone generates a non-extractable Ed25519 keypair; during one-time pairing
over the device's local setup AP, the public key is handed straight to the
device (trust-on-first-use, no cloud in the loop). Every command is signed over
`(version, deviceId, commandId, timestamp, counter, action, args)`; the device
rejects anything unsigned, stale (±90 s), or with a non-increasing counter
(persisted across reboots). The relay is a dumb pipe secured by TLS plus a
bearer token that only rate-limits access — it holds no key material and cannot
mint commands. Full threat model: [`docs/security.md`](docs/security.md).

## Quick start

1. Build the pushbutton tap: [`hardware/README.md`](hardware/README.md)
2. Flash the firmware: [`firmware/README.md`](firmware/README.md)
3. Deploy the relay: [`relay/README.md`](relay/README.md)
4. Install the PWA & pair: [`app/README.md`](app/README.md)

Chip support: **ESP32-S3** (full: USB HID + GPIO). Classic ESP32 / ESP32-C3
have no usable USB OTG — they build in a *power-button-only* profile.
