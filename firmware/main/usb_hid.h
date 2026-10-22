/* usb_hid — TinyUSB HID keyboard (ESP32-S3 USB-OTG).
 *
 * Compiled to no-op stubs returning ESP_ERR_NOT_SUPPORTED when
 * CONFIG_RW_PROFILE_POWER_ONLY is set; the dispatcher turns that into the
 * protocol's "usb_down" error.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/** Initialize locks/recovery before the application installs TinyUSB once.
 * Application owns device/configuration/string descriptors. remote-wake owns
 * all tud_hid_* callbacks and tud_mount/umount/suspend/resume_cb. Use exactly
 * one HID instance (0), non-boot keyboard, report ID 1, with the descriptor
 * below; other non-HID classes may coexist. Recovery may detach/reconnect the
 * entire composite device. Do not mix initialization modes or reinstall USB.
 */
esp_err_t rw_hid_init_external(void);

/** Static keyboard report descriptor; length may be NULL.
 * Power-only profile returns NULL and length 0.
 */
const uint8_t *rw_hid_report_descriptor(size_t *length);

/** Reserve HID for maintenance, rejecting an active action without waiting.
 * Completes a neutral report on an active bus (bounded 200 ms), then blocks
 * actions/recovery until reboot or rw_hid_resume_actions(). Uninitialized HID
 * is a successful no-op. The caller must not subsequently initialize HID.
 */
esp_err_t rw_hid_quiesce(void);
/** Cancel a successful maintenance reservation before USB teardown only. */
void rw_hid_resume_actions(void);

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

/* rw_hid_type / rw_hid_keys are defined only when
 * CONFIG_RW_ACT_KEYBOARD_INPUT is set (off by default); calling them from a
 * build without it is a link error. */

/**
 * Type @p text (US layout, incl. shifted ASCII) and optionally press Enter.
 * Rejects input longer than RW_TYPE_MAX_CHARS or containing characters with
 * no US-layout keycode before pressing any keys. Both press and release await
 * USB transfer completion. Actions have an 8 s budget plus bounded cleanup;
 * on failure the remaining characters/Enter are abandoned, never replayed.
 * A serialized worker restores neutral state after reconnect/resume. Failed
 * cleanup detaches/reconnects USB and blocks input until a fresh enumeration.
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
