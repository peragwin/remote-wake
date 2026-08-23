/* led — WS2812 status indicator.
 *
 * Patterns (hardware/README.md):
 *   RW_LED_BOOT       dim white, brief
 *   RW_LED_SETUP      slow breathing cyan
 *   RW_LED_CONNECTING slow amber pulse
 *   RW_LED_CONNECTED  dim green, solid
 *   RW_LED_BUSY       green blink while a command executes
 *   RW_LED_LOCKOUT    red, during the auth-failure lockout window
 *   RW_LED_FAULT      red slow blink (no Wi-Fi / fatal)
 *
 * All calls are safe when CONFIG_RW_LED_ENABLED is off (they compile to
 * no-ops) and safe from any task.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RW_LED_OFF = 0,
    RW_LED_BOOT,
    RW_LED_SETUP,
    RW_LED_CONNECTING,
    RW_LED_CONNECTED,
    RW_LED_BUSY,
    RW_LED_LOCKOUT,
    RW_LED_FAULT,
} rw_led_state_t;

esp_err_t rw_led_init(void);

/** Set the steady-state pattern. */
void rw_led_set(rw_led_state_t state);

/**
 * Show @p state for @p ms, then fall back to whatever rw_led_set() last
 * selected. Used for the per-command blink.
 */
void rw_led_flash(rw_led_state_t state, uint32_t ms);

#ifdef __cplusplus
}
#endif
