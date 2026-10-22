# firmware

ESP-IDF (≥ 5.2) project for ESP32-S3. Implements `docs/protocol.md` verbatim.

## Build & flash

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor   # UART port, not the OTG port
```

Managed components (`../components/remote_wake/idf_component.yml`) are fetched
automatically on the first build: `espressif/libsodium`, `espressif/esp_websocket_client`,
`espressif/esp_tinyusb`, `espressif/led_strip`, `espressif/qrcode`.

Power-button-only profile (classic ESP32 / C3): `idf.py set-target esp32`
builds with `CONFIG_RW_PROFILE_POWER_ONLY`, which is auto-selected on any
target without a USB-OTG device peripheral (`!SOC_USB_OTG_SUPPORTED`). In that
profile `wake` / `type` / `keys` are still *known* actions but answer
`usb_down`; `ping`, `status`, `power_tap` and `power_hold` work normally.
Per-target seeds live in `sdkconfig.defaults.esp32s3` / `.esp32` / `.esp32c3`.

## Embedding

Other ESP-IDF applications can require the `remote_wake` component without
linking this project's `app_main`. See [embedding.md](../docs/embedding.md) for
the import, initialization ownership and current USB limits. This standalone
application retains its boot flow, pins, provisioning and signed protocol.

## Layout

`../components/remote_wake` owns the shared source registration, dependency
manifest and Kconfig menu. Sources remain under `main/` so existing host and
device test projects use the same implementation. `main/CMakeLists.txt` only
registers the standalone `main.c` entry point and its component dependency.

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
- `main/wifi_station.*`, `main/wifi_recovery.*` — task-owned station recovery,
  association/DHCP deadlines and driver restart policy

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
Host regression tests compile the production C against deterministic USB,
Wi-Fi, WebSocket, clock and scheduler mocks. They inject delayed/missing
completions and network events; they do not simulate the electrical bus or
prove overnight reliability. Run with a C compiler, libsodium development
headers/library, and cJSON 1.7.19 source (`cJSON.c` and `cJSON.h`):

```sh
CJSON_DIR=/path/to/cJSON bash firmware/test_host/run.sh
```

The auth runner reuses the device's Unity cases with real cJSON/libsodium and
adds storage-read failure and numeric overflow regressions. The Wi-Fi suite
exercises the actual station task and recovery policy. Relay tests exercise
the actual dispatch/reconnect loop, frame assembly, stale-session rejection,
and mocked watchdog recovery. HID tests run at both 100 Hz and 1000 Hz,
including power-only stubs, repeated characters, key-up failures, late
completion, suspend, reconnect, and slow endpoints. CI also builds the S3 and
C3 profiles.

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
- `status` → `{"fw": "1.0.1", "rssi": <dBm>, "usb": …, "uptime": <s>}`
- `wake` / `type` / `keys` → `{}` on success
- `power_tap` / `power_hold` → `{"ms": <actual>}`
- errors: `bad_request`, `version`, `dev_mismatch`, `unknown_key`, `sig`,
  `stale`, `replay`, `unknown_act`, `bad_args`, `busy`, `usb_down`, `locked`,
  `internal`

## Watchdog & timing notes
The relay and Wi-Fi recovery tasks subscribe to the task watchdog.
`power_hold` can legitimately block the relay task for up to 12 s, so the
project sets `CONFIG_ESP_TASK_WDT_TIMEOUT_S=30`; do not lower it below ~20 s.
`CONFIG_ESP_TASK_WDT_PANIC=y` is required: a logging-only watchdog cannot
recover a blocked driver or WebSocket teardown call. Existing build trees
must enable this in `sdkconfig` too; `sdkconfig.defaults` only seeds new
configurations. The build rejects an uninitialized or logging-only watchdog.
The relay waits up to 20 s for a command response and the app allows 25 s;
deploy those companion changes to support the maximum 12 s power hold.
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

## Reliability audit (October 2026)

Code inspection found the following defects. The reported repeating first
character is consistent with the HID timing/release defects. The exact
trigger for a particular hours-long disconnect still requires device logs
or reproduction; the recovery gaps below are independently observable in
the code and host fault-injection tests.

| Defect | Repair |
| --- | --- |
| HID's 5/8 ms delays round to zero at the default 100 Hz tick; the readiness loop counts iterations instead of elapsed time. An error abandons key-up. | Real elapsed-time deadlines, USB completion confirmation for press and release, and serialized neutral-report cleanup after errors. Never replay an uncertain key-down. |
| Reconnect timer survives successful DHCP; driver operations block the shared timer task; missing events stop the retry chain. | Dedicated Wi-Fi task reconciles association, netif and IP every second, imposes 30 s attempt deadlines, and cancels retries on recovery. |
| WebSocket close/stop may block indefinitely; the old watchdog configuration only logs. | Skip graceful close during fault recovery, fence old sessions, and require a panic-enabled watchdog as a final recovery bound. |
| Older WebSocket clients can free state before their task exits or deadlock during startup teardown. | Require `esp_websocket_client >=1.8.0` (within 1.x), which includes both upstream fixes. |
| Shared 64-bit timestamps can tear on the 32-bit MCU; old callbacks/queued commands can cross network sessions. | Critical-section snapshots and session/network generations. |
| Frame reassembly confuses receive chunks with WebSocket continuation frames; an overflow can leave a suffix eligible for parsing. | Track frame offsets and FIN independently; discard the entire invalid message. |
| Counter-read failure is treated as counter zero, potentially accepting a replay after a storage error. | Reject the command before counter persistence or execution. |
| Numeric envelope fields are cast before checking their range. | Check finiteness, safe-integer range and integrality before conversion. |
| Setup HTTP handler retries receive timeouts forever. | Bound request-body time and stop on a socket timeout. |
| A supported 12 s power hold exceeds the relay's old 10 s response timeout; the app reports all 504s as offline. | Allow 20 s responses and distinguish an unanswered command from an absent device. |

Before considering the hardware issue closed, use a disposable text editor
to test repeated characters, shifted characters, chords and USB suspend/resume;
interrupt USB during a sequence and verify it stops without replay. Cycle the
AP, interrupt the relay path, restore each, and verify commands recover without
manual reset. Finally run a 24–48 hour idle/reconnect soak and retain UART
reset reasons and reconnect logs. Do not use a real password for these tests.

## Production hardening
Off by default for development. For deployment enable, in this order:
`CONFIG_SECURE_BOOT_V2_ENABLED`, `CONFIG_SECURE_FLASH_ENC_ENABLED`
(release mode), and `CONFIG_NVS_ENCRYPTION` with an `nvs_keys` partition — the
device token and the operator public keys live in NVS. See `docs/security.md`.
