#include "wifi_recovery.h"

#define WIFI_ATTEMPT_MS 30000u
#define WIFI_REBOOT_MS 600000u
#define WIFI_BOUNCE_EVERY 6u

static uint32_t retry_delay_ms(uint32_t attempts)
{
    return attempts < 5 ? 1000 : (attempts < 12 ? 5000 : 10000);
}

void rw_wifi_recovery_init(rw_wifi_recovery_t *s, uint64_t now_ms)
{
    *s = (rw_wifi_recovery_t){
        .phase = RW_WIFI_WAIT,
        .down_since_ms = now_ms,
        .deadline_ms = now_ms,
    };
}

void rw_wifi_recovery_failed(rw_wifi_recovery_t *s, uint64_t now_ms)
{
    s->phase = RW_WIFI_WAIT;
    s->deadline_ms = now_ms + retry_delay_ms(s->attempts);
}

rw_wifi_action_t rw_wifi_recovery_step(rw_wifi_recovery_t *s,
                                       uint64_t now_ms, bool has_ip)
{
    if (has_ip) {
        s->phase = RW_WIFI_ONLINE;
        s->attempts = 0;
        return RW_WIFI_NONE;
    }
    if (s->phase == RW_WIFI_ONLINE) {
        s->down_since_ms = now_ms;
        rw_wifi_recovery_failed(s, now_ms);
    }
    if (now_ms - s->down_since_ms >= WIFI_REBOOT_MS) {
        return RW_WIFI_REBOOT;
    }
    if (now_ms < s->deadline_ms) {
        return RW_WIFI_NONE;
    }
    if (s->phase == RW_WIFI_ATTEMPT) {
        rw_wifi_recovery_failed(s, now_ms);
        return RW_WIFI_NONE;
    }
    /* Advance before issuing any driver operation: a bounce or synchronous
     * failure cannot become stuck forever at the same modulo boundary. */
    s->attempts++;
    s->phase = RW_WIFI_ATTEMPT;
    s->deadline_ms = now_ms + WIFI_ATTEMPT_MS;
    return s->attempts % WIFI_BOUNCE_EVERY == 0
        ? RW_WIFI_BOUNCE : RW_WIFI_CONNECT;
}
