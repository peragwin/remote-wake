# firmware

ESP-IDF (≥ 5.2) project for ESP32-S3. Implements `docs/protocol.md` verbatim.

## Build & flash

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor   # UART port, not the OTG port
```

Managed components (`main/idf_component.yml`) are fetched automatically on the
first build: `espressif/libsodium`, `espressif/esp_websocket_client`,
`espressif/esp_tinyusb`, `espressif/led_strip`, `espressif/qrcode`.

Power-button-only profile (classic ESP32 / C3): `idf.py set-target esp32`
builds with `CONFIG_RW_PROFILE_POWER_ONLY`, which is auto-selected on any
target without a USB-OTG device peripheral (`!SOC_USB_OTG_SUPPORTED`). In that
profile `wake` / `type` / `keys` are still *known* actions but answer
`usb_down`; `ping`, `status`, `power_tap` and `power_hold` work normally.
Per-target seeds live in `sdkconfig.defaults.esp32s3` / `.esp32` / `.esp32c3`.

## Layout
- `main/config.*` — NVS settings, deviceId/deviceToken, 4 operator key slots
  (`p1`…`p4`) and their counter high-water marks
- `main/auth.*` — signing string, canonical-JSON args, Ed25519 verify,
  ts window, anti-replay (write-then-execute)
- `main/canon_json.*` — canonical JSON serializer (sorted keys, no whitespace)
- `main/b64url.*` — unpadded base64url + constant-time compare
- `main/usb_hid.*` — TinyUSB HID keyboard (`wake`, `type`, `keys`)
- `main/power_btn.*` — GPIO pulse driver for the PWR_SW optocoupler
- `main/relay_client.*` — WSS client, dispatch loop, backoff, SNTP
- `main/provisioning.*` — SoftAP setup mode + `/pair` (+ `main/www/setup.html`)
- `main/led.*` — WS2812 status patterns
- `main/main.c` — boot flow and mode selection

## Pins (defaults, Kconfig-able under `menuconfig → remote-wake`)
- `GPIO5` → 330 Ω → optocoupler → PWR_SW (see `hardware/README.md`)
- `GPIO48` on-board WS2812 status LED (`GPIO8` on C3)
- `GPIO0` BOOT button: hold 5 s → setup mode; hold 15 s → factory reset

## Status LED
Breathing cyan = setup mode · amber pulse = connecting · faint green = connected
· green blink = executing a command · solid red = auth-failure lockout ·
slow red blink = no Wi-Fi / no relay.

## Setup mode
Entered on first boot (no Wi-Fi credentials, no relay URL, or no operator key)
or by holding BOOT for 5 s and releasing. The device then:

1. brings up WPA2 SoftAP `remote-wake-<first 4 of deviceId>`. The passphrase is
   `base64url(SHA-256("remote-wake-softap-v1" ‖ deviceToken))[0:12]` — derived
   rather than a slice of the token, so telling someone the AP password does
   not leak relay credentials. It is printed on the serial console together
   with a scannable `WIFI:` QR code;
2. serves the single-page UI on `http://192.168.4.1` (no external assets);
3. accepts `POST /pair {"pubkey","name"}` → `{deviceId, deviceToken, relayUrl,
   kid}`, storing the key in the next free slot;
4. reboots into normal mode on `POST /finish`.

The HTTP server and `/pair` exist **only** in setup mode — normal mode never
starts a listener, so the pairing endpoint is unreachable from the home network
and from the relay.

## Crypto note
ESP-IDF's mbedtls (3.x) has no Ed25519/EdDSA support — `MBEDTLS_PK` covers RSA
and short-Weierstrass ECC only — so signature verification uses libsodium's
`crypto_sign_verify_detached()` from the `espressif/libsodium` managed
component. SHA-256 for the SoftAP passphrase still comes from mbedtls.

## Tests
`firmware/test_apps/auth_test` is a standalone IDF project that compiles
`main/auth.c`, `main/canon_json.c` and `main/b64url.c` directly and runs Unity
tests over the five shared vectors in `docs/test-vectors.json`:

```sh
cd firmware/test_apps/auth_test
idf.py set-target esp32s3 && idf.py build flash monitor
```

It asserts byte-exact signing strings (including args key sorting from a
deliberately unsorted envelope), successful verification of all five vectors,
and rejection of replayed counters, stale/future timestamps, tampered
signatures, tampered args, wrong `dev`, wrong `v`, unregistered `kid` and
missing `args` — plus that the counter high-water mark is persisted *before*
the action check, so a refused action can never be replayed.

## Response shapes
- `ping` → `{"uptime": <s>, "usb": "mounted"|"suspended"|"down"}`
- `status` → `{"fw": "1.0.0", "rssi": <dBm>, "usb": …, "uptime": <s>}`
- `wake` / `type` / `keys` → `{}` on success
- `power_tap` / `power_hold` → `{"ms": <actual>}`
- errors: `bad_request`, `version`, `dev_mismatch`, `unknown_key`, `sig`,
  `stale`, `replay`, `unknown_act`, `bad_args`, `busy`, `usb_down`, `locked`,
  `internal`

## Watchdog & timing notes
The relay task subscribes to the task watchdog and feeds it around every
dispatch. `power_hold` can legitimately block that task for up to 12 s, so the
project sets `CONFIG_ESP_TASK_WDT_TIMEOUT_S=30`; do not lower it below ~20 s.
Commands are executed one at a time from the single relay task, so an
overlapping `power_tap`/`power_hold` can only come from a genuinely concurrent
source and is answered `busy`.

## Relay keepalive
Cloudflare Workers cannot emit RFC 6455 control frames, so the relay's 25 s
keepalive is an application-layer text frame (`{"v":1,"ping":<unix>}`). The
device ignores any frame without a string `id` and `act`, sends the literal
text frame `ping` every 25 s (answered `pong`), and reconnects if nothing at
all arrives for 60 s. A close with status **4001** means another websocket took
over this `deviceId`; the device then backs off to the 60 s cap instead of
fighting it.

## Production hardening
Off by default for development. For deployment enable, in this order:
`CONFIG_SECURE_BOOT_V2_ENABLED`, `CONFIG_SECURE_FLASH_ENC_ENABLED`
(release mode), and `CONFIG_NVS_ENCRYPTION` with an `nvs_keys` partition — the
device token and the operator public keys live in NVS. See `docs/security.md`.
