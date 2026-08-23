# firmware

ESP-IDF (≥ 5.2) project for ESP32-S3. Implements `docs/protocol.md` verbatim.

## Build & flash

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor   # UART port, not the OTG port
```

Power-button-only profile (classic ESP32 / C3): `idf.py set-target esp32`
builds with `CONFIG_RW_PROFILE_POWER_ONLY` (no TinyUSB).

## Layout (see PLAN.md Phase 1)
- `main/config.*` — NVS settings & key slots
- `main/usb_hid.*` — TinyUSB HID keyboard
- `main/power_btn.*` — GPIO pulse driver
- `main/auth.*` — signing-string + Ed25519 verify + anti-replay
- `main/relay_client.*` — WSS client, dispatch loop
- `main/provisioning.*` — SoftAP setup mode + `/pair`

## Pins (defaults, Kconfig-able)
- `GPIO5` → optocoupler → PWR_SW (see `hardware/README.md`)
- `GPIO48` on-board WS2812 status LED
- `GPIO0` BOOT button: hold 5 s → setup mode; hold 15 s → factory reset

## Production hardening
Enable flash encryption + secure boot v2 + NVS encryption; see
`docs/security.md`.
