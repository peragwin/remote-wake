/* usb_hid — TinyUSB HID keyboard (ESP32-S3 USB-OTG).
 *
 * Compiled to no-op stubs returning ESP_ERR_NOT_SUPPORTED when
 * CONFIG_RW_PROFILE_POWER_ONLY is set; the dispatcher turns that into the
 * protocol's "usb_down" error.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RW_TYPE_MAX_CHARS 256   /* docs/protocol.md */
#define RW_KEYS_MAX_CHORDS 32
#define RW_KEYS_MAX_PER_CHORD 6

/** Install the TinyUSB driver and the HID interface. */
esp_err_t rw_hid_init(void);

/** True when the driver is built in (i.e. not the power-only profile). */
bool rw_hid_available(void);
/** True when the USB host has configured us. */
bool rw_hid_mounted(void);
/** True when the bus is suspended (host asleep). */
bool rw_hid_suspended(void);

/**
 * Wake the host: Left-Ctrl press+release twice, 50 ms apart.
 * Issues a USB remote-wakeup first when the bus is suspended.
 */
esp_err_t rw_hid_wake(void);

/**
 * Type @p text (US layout, incl. shifted ASCII) and optionally press Enter.
 * Rejects input longer than RW_TYPE_MAX_CHARS or containing characters with
 * no US-layout keycode.
 */
esp_err_t rw_hid_type(const char *text, bool enter);

/**
 * Execute a `keys` sequence: @p seq is the protocol's array of chords, each a
 * JSON array of key-name strings pressed together then released, 30 ms apart.
 */
esp_err_t rw_hid_keys(const cJSON *seq);

#ifdef __cplusplus
}
#endif
