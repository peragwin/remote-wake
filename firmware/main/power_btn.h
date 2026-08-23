/* power_btn — front-panel PWR_SW driver through a PC817 optocoupler.
 *
 * Idle state is "not pressed" (GPIO low, opto LED dark) and is established
 * before anything else in app_main(), plus re-established from a shutdown
 * handler, so a crash or reset can never latch the host's power button.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bounds from docs/protocol.md. */
#define RW_TAP_MS_MIN   50
#define RW_TAP_MS_MAX   1000
#define RW_TAP_MS_DEF   200
#define RW_HOLD_MS_MIN  3000
#define RW_HOLD_MS_MAX  12000
#define RW_HOLD_MS_DEF  6000

/** Configure the GPIO to its safe idle level. Call this first in app_main(). */
esp_err_t rw_power_btn_init(void);

/**
 * Drive PWR_SW for @p ms.
 * @return ESP_OK, ESP_ERR_INVALID_ARG when @p ms is out of the caller's range,
 *         or ESP_ERR_INVALID_STATE when another pulse is already running
 *         (the caller answers "busy").
 */
esp_err_t rw_power_tap(uint32_t ms);
esp_err_t rw_power_hold(uint32_t ms);

/** True while a pulse is in flight. */
bool rw_power_busy(void);

#ifdef __cplusplus
}
#endif
