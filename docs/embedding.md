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
