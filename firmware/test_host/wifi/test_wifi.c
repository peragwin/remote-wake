/* Compile the actual station adapter and policy against a deterministic SDK.
 * Driver and event callbacks run as they do in firmware; only time, scheduler,
 * netif, and radio outcomes are mocked. */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "../../main/wifi_station.c"

typedef enum {
    NO_EVENTS,
    DHCP_STALL,
    HEALTHY_STALE_EVENT,
    LOST_LINK_NO_EVENT,
    LOST_IP_NO_EVENT,
    CONNECT_FAIL,
    START_FAIL,
    STOP_FAIL,
    ADDRESS_CHANGE,
    FAIL_THEN_GOT_IP,
    NETIF_DOWN_CACHED_IP,
    BRIEF_OUTAGE,
} scenario_t;

static scenario_t scenario;
static uint64_t clock_ms, finish_ms;
static bool associated, netif_up, driver_up, in_callback, rebooted, wdt_registered;
static unsigned wdt_resets;
static bool relay_online, old_session_valid, session_started;
static uint32_t address;
static unsigned connects, starts, stops, disconnects, up_notifications,
                down_notifications, notifications;
static uint64_t connect_at[1024];
/* Any driver start/connect call; the largest gap bounds how long recovery
 * can go quiet during an outage. */
static uint64_t last_attempt_ms, max_attempt_gap_ms;
static bool attempted;
static jmp_buf task_exit;
static void (*created_task)(void *);
static void *created_arg;
static esp_netif_t netif;

static void event(int32_t id)
{
    unsigned calls = starts + stops + connects + disconnects;
    in_callback = true;
    esp_event_base_t base = (id == IP_EVENT_STA_GOT_IP || id == IP_EVENT_STA_LOST_IP)
        ? IP_EVENT : WIFI_EVENT;
    wifi_event_handler(&netif, base, id, NULL);
    in_callback = false;
    assert(calls == starts + stops + connects + disconnects);
}

static void advance(uint64_t ms)
{
    uint64_t previous = clock_ms;
    clock_ms += ms;
    if (scenario == FAIL_THEN_GOT_IP && clock_ms >= 1000) {
        associated = netif_up = true;
        address = 1;
        if (previous < 1000) event(IP_EVENT_STA_GOT_IP);
    }
    if (scenario == ADDRESS_CHANGE && clock_ms >= 10000) {
        address = 2;
    }
    if (scenario == HEALTHY_STALE_EVENT) {
        if (clock_ms >= 3000) {
            associated = netif_up = true;
            address = 1;
        }
        if (previous < 4000 && clock_ms >= 4000) {
            /* An old disconnect delivered after current driver/netif state
             * already reflects GOT_IP must fence the relay, but must not
             * trigger another radio connect or restart. */
            event(WIFI_EVENT_STA_DISCONNECTED);
        }
    }
    if (scenario == BRIEF_OUTAGE && previous < 4000 && clock_ms >= 4000) {
        assert(relay_online && old_session_valid);
        associated = netif_up = false;
        event(WIFI_EVENT_STA_DISCONNECTED);
        assert(!relay_online && !old_session_valid); /* fenced inside callback */
        associated = netif_up = true; /* same address, before the next sample */
        event(IP_EVENT_STA_GOT_IP);
        assert(!relay_online); /* only the station task restores current IP */
    }
    if (scenario == LOST_LINK_NO_EVENT || scenario == LOST_IP_NO_EVENT) {
        if (clock_ms >= 60000 && clock_ms < 100000) {
            if (scenario == LOST_LINK_NO_EVENT) {
                associated = netif_up = false;
                /* Cached address remains nonzero after disconnect. */
                address = 1;
            } else {
                associated = netif_up = true;
                address = 0;
            }
        }
        if (clock_ms >= 100000) {
            associated = netif_up = true;
            address = 1;
        }
    }
    if (clock_ms >= finish_ms) {
        longjmp(task_exit, 1);
    }
}

static void note_attempt(void)
{
    if (attempted && clock_ms - last_attempt_ms > max_attempt_gap_ms) {
        max_attempt_gap_ms = clock_ms - last_attempt_ms;
    }
    attempted = true;
    last_attempt_ms = clock_ms;
}

const char *esp_err_to_name(esp_err_t err) { (void)err; return "mock"; }
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
    void (*handler)(void *, esp_event_base_t, int32_t, void *), void *arg,
    void *instance)
{
    (void)base; (void)id; (void)handler; (void)arg; (void)instance;
    return ESP_OK;
}
esp_netif_t *esp_netif_create_default_wifi_sta(void) { return &netif; }
bool esp_netif_is_netif_up(esp_netif_t *sta) { assert(sta == &netif); return netif_up; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *sta, esp_netif_ip_info_t *ip)
{ assert(sta == &netif); ip->ip.addr = address; return ESP_OK; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap)
{ (void)ap; return driver_up && associated ? ESP_OK : ESP_ERR_WIFI_NOT_CONNECT; }
esp_err_t esp_wifi_disconnect(void)
{
    assert(!in_callback && wdt_registered);
    disconnects++;
    associated = netif_up = false;
    event(WIFI_EVENT_STA_DISCONNECTED);
    return ESP_OK;
}
esp_err_t esp_wifi_stop(void)
{
    assert(!in_callback && wdt_registered);
    stops++;
    if (scenario == STOP_FAIL) return ESP_FAIL;
    driver_up = false;
    return ESP_OK;
}
esp_err_t esp_wifi_start(void)
{
    assert(!in_callback && wdt_registered);
    starts++;
    note_attempt();
    if (scenario == START_FAIL) return ESP_FAIL;
    driver_up = true;
    event(0);
    return ESP_OK;
}
esp_err_t esp_wifi_connect(void)
{
    assert(!in_callback && driver_up && wdt_registered);
    assert(connects < sizeof(connect_at) / sizeof(connect_at[0]));
    connect_at[connects++] = clock_ms;
    note_attempt();
    if (scenario == CONNECT_FAIL || scenario == STOP_FAIL || scenario == FAIL_THEN_GOT_IP) return ESP_FAIL;
    if (scenario == DHCP_STALL) associated = netif_up = true;
    if (scenario == NETIF_DOWN_CACHED_IP) {
        associated = true;
        netif_up = false;
        address = 1;
    }
    if ((scenario == LOST_LINK_NO_EVENT || scenario == LOST_IP_NO_EVENT || scenario == ADDRESS_CHANGE || scenario == BRIEF_OUTAGE) && clock_ms < 60000) {
        associated = netif_up = true;
        address = 1;
    }
    return ESP_OK;
}
esp_err_t esp_wifi_set_mode(int mode) { (void)mode; return ESP_OK; }
esp_err_t esp_wifi_set_config(int interface, wifi_config_t *config)
{ (void)interface; assert(config->sta.ssid[0] == 'x'); return ESP_OK; }
esp_err_t esp_wifi_set_ps(int ps) { (void)ps; return ESP_OK; }
esp_err_t esp_task_wdt_add(TaskHandle_t task)
{ (void)task; assert(!wdt_registered); wdt_registered = true; return ESP_OK; }
esp_err_t esp_task_wdt_reset(void)
{ assert(wdt_registered); wdt_resets++; return ESP_OK; }
int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
/* Recovery must never restart the controller; a call is recorded and fails
 * every scenario. */
void esp_restart(void) { rebooted = true; longjmp(task_exit, 1); }
uint32_t ulTaskNotifyTake(int clear, TickType_t ticks)
{
    (void)clear;
    if (ticks != portMAX_DELAY) advance(ticks);
    return 1;
}
void xTaskNotifyGive(TaskHandle_t task) { assert(task != NULL); notifications++; }
int xTaskCreate(void (*task)(void *), const char *name, uint32_t stack,
                void *arg, unsigned priority, TaskHandle_t *handle)
{
    (void)name; (void)stack; (void)priority;
    created_task = task; created_arg = arg; *handle = &netif;
    return pdPASS;
}
void vTaskDelay(TickType_t ticks) { assert(!in_callback); advance(ticks); }
void vTaskDelete(TaskHandle_t task) { (void)task; assert(false); }
esp_err_t rw_config_get_wifi(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    (void)pass_sz; assert(ssid_sz >= 2); strcpy(ssid, "x"); pass[0] = '\0';
    return ESP_OK;
}
void rw_relay_notify_network(bool up)
{
    relay_online = up;
    if (up) {
        up_notifications++;
        if (!session_started) { session_started = old_session_valid = true; }
    } else {
        down_notifications++;
        old_session_valid = false;
    }
}
void rw_led_set(rw_led_state_t state) { assert(state == RW_LED_FAULT); }

static void run(scenario_t test, uint64_t duration)
{
    scenario = test; clock_ms = 0; finish_ms = duration;
    associated = netif_up = driver_up = in_callback = rebooted = wdt_registered = false;
    wdt_resets = 0;
    relay_online = old_session_valid = session_started = false;
    atomic_store_explicit(&s_outage_generation, 0, memory_order_relaxed);
    address = 0;
    connects = starts = stops = disconnects = up_notifications = down_notifications = notifications = 0;
    last_attempt_ms = max_attempt_gap_ms = 0;
    attempted = false;
    assert(rw_wifi_station_start() == ESP_OK);
    if (setjmp(task_exit) == 0) created_task(created_arg);
    assert(notifications > 0 && wdt_registered && wdt_resets > 1);
    assert(!rebooted);
}

/* One attempt deadline (30 s) plus the capped retry delay (10 s), with slack
 * for the 1 s station poll and the 100 ms disconnect settle. */
#define MAX_RETRY_GAP_MS 42000u
/* Six times the removed ten-minute reboot deadline. */
#define LONG_OUTAGE_MS 3600000u

#define MIN_LONG_ATTEMPTS (LONG_OUTAGE_MS / MAX_RETRY_GAP_MS)

static void assert_still_retrying(void)
{
    assert(attempted && max_attempt_gap_ms <= MAX_RETRY_GAP_MS);
    assert(finish_ms - last_attempt_ms <= MAX_RETRY_GAP_MS);
}

int main(void)
{
    /* Persistent outages never reboot: connects keep coming at a bounded
     * interval and the driver keeps bouncing long after the old deadline. */
    run(NO_EVENTS, LONG_OUTAGE_MS);
    assert(connects >= MIN_LONG_ATTEMPTS && stops >= connects / 6 - 1);
    assert(connect_at[0] == 0 && connect_at[1] >= 31000);
    assert(up_notifications == 0);
    assert_still_retrying();

    run(DHCP_STALL, LONG_OUTAGE_MS);
    assert(connects >= MIN_LONG_ATTEMPTS && stops >= connects / 6 - 1);
    assert(up_notifications == 0);
    assert_still_retrying();

    /* A valid IP supersedes the connection deadline and every pending retry;
     * a stale event cannot undo it. Stay healthy past ten minutes. */
    run(HEALTHY_STALE_EVENT, 650000);
    assert(connects == 1 && stops == 0 && disconnects == 0);
    assert(up_notifications == 2 && down_notifications == 1 && relay_online);

    run(BRIEF_OUTAGE, 20000);
    assert(connects == 1 && stops == 0 && disconnects == 0);
    assert(up_notifications == 2 && down_notifications == 1 && relay_online);
    assert(!old_session_valid);

    run(LOST_LINK_NO_EVENT, 140000);
    assert(connects >= 3);
    assert(connect_at[1] >= 61000 && connect_at[1] < 63000);
    assert(up_notifications == 2 && down_notifications >= 1 && relay_online);

    run(LOST_IP_NO_EVENT, 140000);
    assert(connects >= 3 && disconnects > 0);
    assert(up_notifications == 2 && down_notifications >= 1 && relay_online);

    run(CONNECT_FAIL, LONG_OUTAGE_MS);
    assert(connects >= MIN_LONG_ATTEMPTS && stops >= connects / 6 - 1);
    assert_still_retrying();

    run(START_FAIL, LONG_OUTAGE_MS);
    assert(starts >= MIN_LONG_ATTEMPTS && connects == 0);
    assert_still_retrying();

    run(STOP_FAIL, LONG_OUTAGE_MS);
    assert(connects >= MIN_LONG_ATTEMPTS && stops >= connects / 6 - 1);
    assert_still_retrying();

    run(ADDRESS_CHANGE, 20000);
    assert(connects == 1 && stops == 0);
    assert(up_notifications == 2 && down_notifications == 1);

    run(FAIL_THEN_GOT_IP, 650000);
    assert(connects == 1 && stops == 0 && disconnects == 0);
    assert(up_notifications == 1 && down_notifications == 0);

    run(NETIF_DOWN_CACHED_IP, LONG_OUTAGE_MS);
    assert(connects >= MIN_LONG_ATTEMPTS && stops >= connects / 6 - 1);
    assert(up_notifications == 0);
    assert_still_retrying();

    puts("Wi-Fi recovery: 12 production adapter/policy scenarios passed");
    return 0;
}
