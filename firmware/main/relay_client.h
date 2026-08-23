/* relay_client — wss link to the relay, command dispatch, responses.
 *
 * Owns one FreeRTOS task that:
 *   - connects to <relay>/v1/device/<deviceId> with a bearer header
 *   - sends the hello frame
 *   - verifies every inbound envelope through auth.c, dispatches, replies
 *   - reconnects with 1 s -> 60 s exponential backoff, +/-20 % jitter
 *   - feeds the task watchdog
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RW_FW_VERSION "1.0.0"

/** Start the relay task. Call once Wi-Fi is up (or about to be). */
esp_err_t rw_relay_start(void);

/** True while the websocket is connected and the hello has been sent. */
bool rw_relay_connected(void);

/** Notify the client that the station got/lost an IP. */
void rw_relay_notify_network(bool up);

/** Seconds remaining in the auth-failure lockout window, 0 when not locked. */
int rw_relay_lockout_remaining(void);

#ifdef __cplusplus
}
#endif
