/* Wi-Fi recovery policy, owned exclusively by the station task. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RW_WIFI_WAIT,
    RW_WIFI_ATTEMPT,
    RW_WIFI_ONLINE,
} rw_wifi_phase_t;

typedef enum {
    RW_WIFI_NONE,
    RW_WIFI_CONNECT,
    RW_WIFI_BOUNCE,
    RW_WIFI_REBOOT,
} rw_wifi_action_t;

typedef struct {
    rw_wifi_phase_t phase;
    uint32_t attempts;
    uint64_t down_since_ms;
    uint64_t deadline_ms;
} rw_wifi_recovery_t;

void rw_wifi_recovery_init(rw_wifi_recovery_t *state, uint64_t now_ms);
rw_wifi_action_t rw_wifi_recovery_step(rw_wifi_recovery_t *state,
                                       uint64_t now_ms, bool has_ip);
/* Report a synchronous driver failure; successful calls remain under the
 * association/DHCP deadline even when no SDK event is ever delivered. */
void rw_wifi_recovery_failed(rw_wifi_recovery_t *state, uint64_t now_ms);
