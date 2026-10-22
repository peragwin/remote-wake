# Embedding remote-wake in an ESP-IDF application

`components/remote_wake` is the shared ESP-IDF component. It does not define
`app_main`; `firmware/main/main.c` remains the standalone application. An
embedding application owns its boot flow, board configuration, display,
telemetry and presentation. The component retains the existing protocol v1,
operator keys, NVS counter persistence, provisioning and command dispatch.

## Import

Keep a checkout of this repository as a sibling or submodule. Add the component
before ESP-IDF's project include in the application's root `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
# Example: the checkout is a sibling of this application.
set(REMOTE_WAKE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../remote-wake")
list(APPEND EXTRA_COMPONENT_DIRS "${REMOTE_WAKE_ROOT}/components/remote_wake")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(my_application)
```

Then register the application's own entry point:

```cmake
idf_component_register(SRCS "main.c" INCLUDE_DIRS "."
                       REQUIRES remote_wake)
```

The dependency manifest and `menuconfig → remote-wake` options live with the
component. Do not copy them into each application. The component references
production sources and headers under `firmware/main/` and embeds
`firmware/main/www/setup.html`; retain the whole repository checkout. It is
not yet a self-contained directory suitable for independent registry upload.
The auth device test remains an isolated crypto-only test project with its
own minimal test manifest.

Applications should depend on `remote_wake`, not its source files or managed
library names. Its existing public headers provide `rw_config_*`, `rw_auth_*`,
`rw_power_btn_*`, `rw_led_*`, `rw_hid_*`, `rw_prov_*`, `rw_relay_*` and
`rw_wifi_station_start`. JSON is public because existing auth/HID headers use
`cJSON`; platform and managed implementation requirements are declared by
the component. An application that directly calls ESP-IDF APIs must declare
those API dependencies in its own component. Internal module headers remain
visible for compatibility; this extraction does not introduce a new facade
or promise every internal function as a stable extension point.

## Initialization and ownership

Use `firmware/main/main.c` as the reference ordering, with the application's
own board and boot policy. The existing initialization functions do not form
a general-purpose start/stop service; call startup functions once.

1. Configure the power output safely with `rw_power_btn_init()` before other
   initialization can fail. Initialize the optional status LED only if its
   configured GPIO belongs to remote-wake.
2. The application initializes NVS once, then calls `rw_config_init()` and
   `rw_auth_init()`. Config owns namespace `rw_cfg`, including identity,
   credentials, operator keys and replay counters. Preserve this namespace
   and its write-before-execute semantics. The standalone application's NVS
   error recovery erases the entire default NVS partition; an embedding
   application must choose its own recovery policy for shared storage.
3. The application initializes `esp_netif`, the default event loop and Wi-Fi
   once, then selects RAM Wi-Fi storage as in the standalone entry point.
   Do not duplicate these initializations in another application module.
4. Choose provisioning or normal mode. `rw_prov_start()` owns the setup
   SoftAP and HTTP server and may restart the device through its setup
   endpoints. The standalone application never starts the relay or HID in
   setup mode. Preserve this separation so pairing stays off the home
   network and relay command execution stays disabled during setup.
5. In normal mode initialize HID, start the relay, then call
   `rw_wifi_station_start()`. The station module owns the default STA netif,
   Wi-Fi mode/configuration, recovery task and network notifications to the
   relay. Other application features may observe connectivity but must not
   run a competing Wi-Fi reconnect manager. A host-managed Wi-Fi adapter is
   not provided by this extraction.

Set `CONFIG_RW_PWR_ENABLED=n` when the board has no PWR_SW circuit.
`rw_power_btn_init()` then succeeds without touching GPIOs, registering shutdown
handlers or allocating locks; pulses return `ESP_ERR_NOT_SUPPORTED` and busy is
always false. Protocol v1 has no disabled-power error, so signed `power_tap` and
`power_hold` commands return `internal`, never the misleading `usb_down`.
The default remains enabled for existing standalone installations.

The existing status LED is an optional component service, not a display
extension API. Display drivers, rendering, themes, board presets and host
telemetry belong to the embedding application. Avoid pin conflicts by setting
`CONFIG_RW_PWR_GPIO`, `CONFIG_RW_BOOT_GPIO` for an application that uses the
reference button logic, and either configuring or disabling
`CONFIG_RW_LED_ENABLED` in the application's sdkconfig defaults.

Copy the relevant platform settings from `firmware/sdkconfig.defaults` and the
target defaults into the application deliberately: enable the TLS certificate
bundle, WebSocket transport, a panic-enabled initialized task watchdog with a
30 s timeout, adequate main stack, and enough flash/partition space. The
component does not replace the application's partition table, watchdog policy
or console settings. Existing compile-time watchdog checks still apply.

## USB ownership

`rw_hid_init()` installs TinyUSB with the standalone keyboard-only descriptors.
For a composite device, call `rw_hid_init_external()` instead, **before** the
application installs TinyUSB. It initializes the same action lock, recovery
worker and lifecycle state without installing a driver. Call startup once and
do not mix these modes. Initialize config/auth first as in the normal boot flow.

The application owns device, configuration and string descriptors and the sole
TinyUSB installation. Include exactly one HID interface (HID instance 0), using
non-boot protocol, report ID 1, an interrupt IN endpoint with at least 9 bytes,
and the bytes returned by `rw_hid_report_descriptor(&length)`. The descriptor
has static lifetime. Other non-HID classes, such as a vendor telemetry interface,
may coexist with their own endpoints and callbacks. Enable remote wakeup in the
configuration descriptor when host wake is required. Configure TinyUSB class
counts and endpoint allocation in the application.

remote-wake owns all `tud_hid_*` callbacks and `tud_mount_cb`, `tud_umount_cb`,
`tud_suspend_cb`, `tud_resume_cb`; the application must not define competing
callbacks. These lifecycle callbacks are essential to prevent stale reports
from being accepted after a bus reset. Recovery may disconnect/reconnect the
**whole composite device** after a failed neutral report; other interfaces
must tolerate that. External initialization does not implement stack teardown
or runtime reconfiguration. Validate composition and host wake on hardware.

`CONFIG_RW_PROFILE_POWER_ONLY` retains no-HID behavior: both initialization
functions succeed, the report descriptor is NULL with length zero, and HID
commands return protocol `usb_down`. USB Serial/JTAG is a separate peripheral
where supported.

## Optional ROM download maintenance

Applications may expose their own local maintenance command using
`maintenance.h`. `rw_maintenance_bootloader_supported()` currently reports
support only on ESP32-S3 with ESP-IDF 5.4.x, the default TinyUSB task and existing
eFuses that permit USB Serial/JTAG ROM download with the internal PHY. Other
targets/SDK versions return false and requests return `ESP_ERR_NOT_SUPPORTED`.
This helper does not change any eFuse. Reserve native USB GPIO19/20 for USB;
external PHY and USB host applications are unsupported.

Call `rw_maintenance_enter_bootloader()` from the application's task, outside
TinyUSB callbacks. It synchronously reserves the power/HID action locks,
rejects active actions with `ESP_ERR_INVALID_STATE`, and completes a neutral
keyboard report on an active bus with the existing bounded transfer deadline.
It never wakes a sleeping host. New power/HID actions and HID recovery stay
blocked while maintenance is accepted; rejected requests restore normal
operation and leave USB attached. No services may be initialized afterward.

`ESP_OK` means a reboot task was admitted. Only then send the application's
success acknowledgement. Flush it during `RW_MAINTENANCE_ACK_GRACE_MS` (500 ms),
then stop all application USB reads/writes before that grace expires. All
composite class owners must honor this boundary. Allocation, shutdown-handler
registration, busy and neutral-transfer errors are returned before acceptance.
The worker detaches and uninstalls TinyUSB, returns the internal PHY to hardware
USB Serial/JTAG, allows a bounded host bus-reset interval, clears USB persistence
flags, and sets volatile forced ROM download at shutdown before restarting.
If TinyUSB teardown fails, it restarts normally rather than changing PHY
ownership while a USB task may still run.

The handoff follows Espressif's
[Arduino S3 USB-to-Serial/JTAG and forced-download implementation](https://github.com/espressif/arduino-esp32/blob/master/cores/esp32/esp32-hal-tinyusb.c),
using the installed IDF's LL APIs and the managed TinyUSB teardown function.
It is an optional application transport feature: the signed relay protocol and
its action list remain unchanged. It writes no flash, NVS, partition table or
OTA boot selection and preserves pairing/replay counters. ROM download can be
left with an EN reset/power cycle or `esptool --after watchdog_reset`; a regular
software reset may retain the forced-download state. Flash only the intended
firmware ranges and retain the NVS partition when updating. ROM entry is separate
from the opt-in whole-application updater described below.

## Whole-application OTA

Enable `CONFIG_RW_OTA_ENABLED` with a product-owned A/B partition layout and
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. Include `ota.h`, retain one
`RW_OTA_DEFINE_IMAGE_IDENTITY(...)` record, and pass its address to `rw_ota_init`.
Use the same version in ESP-IDF `PROJECT_VER`; release counters increase within
the exact product/board/profile/channel/layout/chip scope. The source prefix,
manifest URL, publisher key set, redirect hosts and policy belong to the app.
All configuration pointers must live for the application lifetime. Empty trust
starts boot-health-only mode and never registers or downloads releases.

Call `rw_ota_confirm_healthy()` after required local startup/functional checks,
including setup mode. Never require an online desktop/Internet connection for
health. Pending candidates have a bounded health deadline and can be explicitly
rejected with `rw_ota_reject_unhealthy()`. Confirmation establishes the initial
valid OTA selector when needed and persists the confirmed release journal.
Initialization failures must not erase NVS; keep settings readable by rollback.

`rw_ota_offer_manifest()` only copies a bounded URL. Network/download work belongs
to the updater task. Read `rw_ota_get_status()` from UI code; progress callbacks
run in the worker and must not render or block. `prepare_restart` returns
`ESP_ERR_INVALID_STATE` while the app is busy; only return `ESP_OK` after stopping
app USB users/other tasks that cannot survive restart. The engine reserves shared
HID/power work, selects a verified inactive image, then reboots the controller.
If preparation fails or times out, a failed progress event lets the app resume
its work. Do not reinitialize shared services during accepted maintenance.

See [updates.md](updates.md) and the [publisher tools](../tools/ota/README.md)
for signed schemas, source constraints, release automation and commissioning.

## Validation

The component compiles the same C sources used by the existing host regression
suites; no protocol or command behavior is changed. Run the suites with:

```sh
CJSON_DIR=/path/to/cJSON bash firmware/test_host/run.sh
```

Build the standalone project from `firmware/` with the installed ESP-IDF to
verify the application/component link. Build the embedding application too:
a successful standalone build does not validate another board's pins,
display wiring, USB composition or initialization policy.
