/* Optional application-owned maintenance transport; no relay protocol changes. */
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

#define RW_MAINTENANCE_ACK_GRACE_MS 500

/** True for ESP32-S3 on IDF 5.4.x with ROM USJ download permitted by eFuses.
 * This does not check board wiring. Internal USB pads GPIO19/20 are required.
 * Reads eFuses only; never programs them. Other targets return false.
 */
bool rw_maintenance_bootloader_supported(void);

/** Request reboot to ROM USB Serial/JTAG download mode from an application task.
 * On ESP_OK, flush the acknowledgement within RW_MAINTENANCE_ACK_GRACE_MS
 * (500 ms), then stop application USB reads/writes before the grace expires.
 * Do not report success before admission. Never call from a TinyUSB callback, ISR, or while holding an
 * application lock needed by a shutdown handler. USB stack must use the default
 * esp_tinyusb task (or be absent), internal PHY, and no USB host stack.
 *
 * Reserves power/HID and sends neutral state without waking a sleeping host; returns
 * INVALID_STATE if a power pulse, HID action/recovery or maintenance is busy,
 * a transfer error if neutral state cannot complete, NOT_SUPPORTED if unavailable,
 * NO_MEM if task allocation fails, or the shutdown-registration error.
 * ESP_OK means a reboot task was accepted; success never returns from that task.
 * Rejected requests restore HID operation and do not detach USB. Once accepted,
 * only the acknowledgement may use USB during the grace; do not initialize
 * or reconfigure USB, HID or power services before reboot.
 * No flash, NVS, partitions, OTA boot selection or eFuses are written. ROM mode is
 * temporary; leave with an EN reset/power cycle or esptool --after watchdog_reset.
 */
esp_err_t rw_maintenance_enter_bootloader(void);

#ifdef __cplusplus
}
#endif
