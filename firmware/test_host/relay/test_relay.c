/* Compile the production translation unit, not a copy of its state machine.
 * ESP-IDF/FreeRTOS, JSON and authentication are mocked at their boundaries. */
#include <assert.h>
#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include "../../../main/relay_client.c"

static int64_t mock_now = 1000000;
static int64_t mock_stop_at;
static int mock_send = INT_MAX;
static int mock_send_calls;
static int mock_stop_calls;
static int mock_destroy_calls;
static int mock_wdt_feeds;
static int mock_restarts;
static int mock_actions;
static esp_err_t mock_power_result;
static bool mock_power_dispatch;
static char mock_error[32];
static esp_err_t mock_wdt_add_result;
static bool mock_auto_connect = true;
static bool mock_drop_in_verify;
static bool mock_drop_on_receive;
static bool mock_stall_stop;
static bool mock_stall_send;
static int64_t mock_last_feed;
static bool mock_subscribed;
static void *mock_event_arg;
static jmp_buf mock_exit;
static rw_rx_msg_t mock_queue[RX_QUEUE_DEPTH];
static size_t mock_queued;
static int mock_verify_calls;
static struct mock_ws { int unused; } mock_client;

static void emit(uint32_t session, int id, esp_websocket_event_data_t *data)
{
    ws_event_handler((void *)(uintptr_t)session, NULL, id, data);
}

static void frame(uint32_t session, int op, bool fin, const char *text,
                  int len, int total, int offset)
{
    esp_websocket_event_data_t data = {
        .op_code = op, .fin = fin, .data_ptr = text,
        .data_len = len, .payload_len = total, .payload_offset = offset,
    };
    emit(session, WEBSOCKET_EVENT_DATA, &data);
}

static void reset(void)
{
    drain_queue();
    asm_reset();
    s_link = (rw_link_state_t){0};
    s_client = NULL;
    s_rx_queue = &mock_queue;
    s_lock_until_us = 0;
    memset(s_fail_ts, 0, sizeof(s_fail_ts));
    s_fail_head = 0;
    s_relay_down_since_us = 0;
    mock_now = 1000000;
    mock_stop_at = 0;
    mock_send = INT_MAX;
    mock_send_calls = mock_stop_calls = mock_destroy_calls = 0;
    mock_wdt_feeds = mock_restarts = mock_actions = mock_verify_calls = 0;
    mock_wdt_add_result = ESP_OK;
    mock_auto_connect = true;
    mock_drop_in_verify = mock_drop_on_receive = false;
    mock_stall_stop = mock_stall_send = false;
    mock_last_feed = mock_now;
    mock_subscribed = false;
    rw_relay_notify_network(true);
}

static uint32_t connect(void)
{
    uint32_t session = begin_session();
    s_client = &mock_client;
    emit(session, WEBSOCKET_EVENT_CONNECTED, NULL);
    assert(session_valid(session));
    assert(!rw_relay_connected()); /* hello has not been sent */
    return session;
}

static void expect_message(uint32_t session, const char *expected)
{
    rw_rx_msg_t msg;
    assert(xQueueReceive(s_rx_queue, &msg, 0) == pdTRUE);
    assert(msg.session == session);
    assert(msg.len == strlen(expected));
    assert(strcmp(msg.data, expected) == 0);
    free(msg.data);
    assert(mock_queued == 0);
}

static void test_fragmentation(void)
{
    reset();
    uint32_t session = connect();
    frame(session, 1, false, "ab", 2, 4, 0);
    frame(session, 1, false, "cd", 2, 4, 2);
    assert(mock_queued == 0);
    frame(session, 9, true, "p", 1, 1, 0); /* interleaved control frame */
    frame(session, 0, false, "ef", 2, 2, 0);
    assert(mock_queued == 0);
    frame(session, 0, true, "g", 1, 2, 0);
    assert(mock_queued == 0); /* FIN applies only after the last chunk */
    frame(session, 0, true, "h", 1, 2, 1);
    expect_message(session, "abcdefgh");

    frame(session, 1, false, "ab", 2, 2, 0);
    frame(session, 0, true, NULL, 0, 0, 0); /* empty final continuation */
    expect_message(session, "ab");
    frame(session, 1, true, "ping", 4, 4, 0);
    expect_message(session, "ping");
}

static void test_drop_entire_bad_message(void)
{
    reset();
    uint32_t session = connect();
    char huge[RW_MAX_ENVELOPE_BYTES];
    memset(huge, 'x', sizeof(huge));
    frame(session, 1, false, huge, sizeof(huge), sizeof(huge), 0);
    frame(session, 0, true, "x", 1, 5, 0); /* over the whole-message cap */
    frame(session, 0, true, "tail", 4, 5, 1);
    assert(mock_queued == 0); /* never accept a suffix after overflow */
    frame(session, 0, true, "orphan", 6, 6, 0);
    assert(mock_queued == 0);
    frame(session, 1, true, "good", 4, 4, 0);
    expect_message(session, "good");

    frame(session, 1, true, "a", 1, 3, 0);
    frame(session, 1, true, "c", 1, 3, 2); /* missing chunk */
    assert(mock_queued == 0);
    frame(session, 1, false, "a", 1, 3, 0);
    frame(session, 0, true, "tail", 4, 4, 0); /* prior frame incomplete */
    assert(mock_queued == 0);
    frame(session, 1, false, "text", 4, 4, 0);
    frame(session, 2, false, "bin", 3, 3, 0);
    frame(session, 0, true, "tail", 4, 4, 0);
    assert(mock_queued == 0);
    frame(session, 1, true, "new", 3, 3, 0);
    expect_message(session, "new");

    frame(session, 1, true, NULL, 1, 1, 0); /* malformed callback */
    frame(session, 0, true, "tail", 4, 4, 0);
    assert(mock_queued == 0);
    frame(session, 1, true, "next", 4, 4, 0);
    expect_message(session, "next");
}

static void test_session_fence(void)
{
    reset();
    uint32_t old = connect();
    frame(old, 1, true, "queued", 6, 6, 0);
    rw_relay_notify_network(false);
    rw_relay_notify_network(true); /* brief outage between relay task polls */
    emit(old, WEBSOCKET_EVENT_CONNECTED, NULL);
    frame(old, 1, true, "stale", 5, 5, 0);
    assert(!session_valid(old));
    assert(!rw_relay_connected());
    assert(mock_queued == 1);
    rw_rx_msg_t msg;
    assert(xQueueReceive(s_rx_queue, &msg, 0));
    assert(!session_valid(msg.session));
    free(msg.data);
    uint32_t fresh = connect();
    emit(old, WEBSOCKET_EVENT_DISCONNECTED, NULL);
    assert(session_valid(fresh));
    frame(fresh, 1, true, "fresh", 5, 5, 0);
    expect_message(fresh, "fresh");
    teardown_client();
    emit(fresh, WEBSOCKET_EVENT_CONNECTED, NULL);
    assert(!session_valid(fresh));
    assert(mock_stop_calls == 1 && mock_destroy_calls == 1);
}

static void test_sends_and_dispatch_race(void)
{
    for (int result = -1; result < 4; ++result) {
        reset();
        connect();
        mock_send = result;
        assert(!ws_send_text("ping", 4, 2000));
        assert(link_snapshot().failed);
    }
    reset();
    uint32_t session = connect();
    assert(ws_send_text("ping", 4, 2000));
    assert(session_valid(session));
    mock_drop_in_verify = true;
    handle_envelope("{}", 2, session);
    assert(mock_verify_calls == 1 && mock_actions == 0);
    assert(!session_valid(session));
}

static void test_task_recovery(void)
{
    reset();
    mock_auto_connect = false;
    mock_stop_at = mock_now + 18000000;
    if (setjmp(mock_exit) == 0) {
        relay_task(NULL);
        assert(false);
    }
    assert(mock_stop_calls >= 1); /* handshake timeout cleans up and retries */
    assert(mock_destroy_calls == mock_stop_calls);
    assert(mock_wdt_feeds > 60);
    assert(mock_restarts == 0);
    teardown_client();

    reset();
    mock_stop_at = mock_now + 63000000;
    if (setjmp(mock_exit) == 0) {
        relay_task(NULL);
        assert(false);
    }
    assert(mock_send_calls >= 3); /* hello, 25 s probe, 50 s probe */
    assert(mock_stop_calls >= 1); /* 60 s without traffic reconnects */
    assert(mock_restarts == 0);
    teardown_client();

    reset();
    mock_wdt_add_result = ESP_ERR_INVALID_STATE;
    if (setjmp(mock_exit) == 0) {
        relay_task(NULL);
        assert(false);
    }
    assert(mock_restarts == 1 && mock_stop_calls == 0);

    reset();
    mock_auto_connect = false;
    mock_stall_stop = true;
    if (setjmp(mock_exit) == 0) {
        relay_task(NULL);
        assert(false);
    }
    assert(mock_restarts == 1 && mock_stop_calls == 1);
    assert(mock_now - mock_last_feed == 30000000);
    s_client = NULL; /* mock simulates reset while stop was blocked */

    reset();
    mock_stall_send = true;
    if (setjmp(mock_exit) == 0) {
        relay_task(NULL);
        assert(false);
    }
    assert(mock_restarts == 1 && mock_send_calls == 1);
    s_client = NULL;
}

static void test_queue_loss_before_dispatch(void)
{
    reset();
    mock_drop_on_receive = true;
    mock_stop_at = mock_now + 1000000;
    if (setjmp(mock_exit) == 0) {
        relay_task(NULL);
        assert(false);
    }
    assert(mock_verify_calls == 0 && mock_actions == 0);
    assert(mock_stop_calls >= 1);
    teardown_client();
}

static void test_disabled_power_dispatch(void)
{
    reset();
    mock_power_dispatch = true;
    mock_power_result = ESP_ERR_NOT_SUPPORTED;
    rw_cmd_t cmd = {0};
    strcpy(cmd.act, "power_tap");
    dispatch(&cmd);
    assert(mock_actions == 1 && !strcmp(mock_error, "internal"));
    strcpy(cmd.act, "power_hold");
    dispatch(&cmd);
    assert(mock_actions == 2 && !strcmp(mock_error, "internal"));
    mock_power_dispatch = false;
    mock_power_result = ESP_OK;
}

int main(void)
{
    s_rx_queue = &mock_queue;
    test_disabled_power_dispatch();
    test_fragmentation();
    test_drop_entire_bad_message();
    test_session_fence();
    test_sends_and_dispatch_race();
    test_task_recovery();
    test_queue_loss_before_dispatch();
    drain_queue();
    free(s_asm);
    puts("relay host tests passed");
    return 0;
}

/* Deterministic boundary mocks. A blocked transport advances an independently
 * modelled 30 s watchdog, never the relay task's own reboot timer. */
const char *esp_err_to_name(esp_err_t e) { (void)e; return "mock"; }
int64_t esp_timer_get_time(void) { return mock_now; }
void esp_restart(void) { ++mock_restarts; longjmp(mock_exit, 1); }
uint32_t esp_random(void) { return 0; }
esp_err_t esp_task_wdt_add(void *task)
{
    mock_subscribed = mock_wdt_add_result == ESP_OK;
    return mock_wdt_add_result;
}
static void watchdog_stall(void)
{
    assert(mock_subscribed && CONFIG_ESP_TASK_WDT_INIT && CONFIG_ESP_TASK_WDT_PANIC);
    mock_now = mock_last_feed + 30000000;
    esp_restart();
}
esp_err_t esp_task_wdt_reset(void) { ++mock_wdt_feeds; mock_last_feed = mock_now; return ESP_OK; }
void vTaskDelay(TickType_t ticks)
{
    mock_now += (int64_t)ticks * 1000;
    if (ticks && mock_stop_at && mock_now >= mock_stop_at) { longjmp(mock_exit, 1); }
}
void vTaskDelete(void *task) { }
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                void *arg, unsigned prio, void *handle) { return pdPASS; }
QueueHandle_t xQueueCreate(unsigned depth, unsigned size) { return &mock_queue; }
void vQueueDelete(QueueHandle_t queue) { }
int xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout)
{
    if (mock_queued == RX_QUEUE_DEPTH) { return 0; }
    mock_queue[mock_queued++] = *(const rw_rx_msg_t *)item;
    return pdTRUE;
}
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout)
{
    if (mock_drop_on_receive && timeout) {
        mock_drop_on_receive = false;
        queue_message("{command}", 9, s_link.session);
        rw_relay_notify_network(false);
        rw_relay_notify_network(true);
    }
    if (!mock_queued) { vTaskDelay(timeout); return 0; }
    *(rw_rx_msg_t *)item = mock_queue[0];
    memmove(mock_queue, mock_queue + 1, --mock_queued * sizeof(mock_queue[0]));
    return pdTRUE;
}
esp_err_t esp_crt_bundle_attach(void *conf) { return ESP_OK; }
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config) { return ESP_OK; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap) { ap->rssi = -50; return ESP_OK; }
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *cfg)
{
    assert(cfg->disable_auto_reconnect);
    assert(cfg->disable_pingpong_discon);
    assert(cfg->network_timeout_ms == 10000);
    return &mock_client;
}
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t client, int event,
                    void (*handler)(void *, esp_event_base_t, int32_t, void *), void *arg)
{ mock_event_arg = arg; return ESP_OK; }
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t client)
{
    if (mock_auto_connect) { ws_event_handler(mock_event_arg, NULL, WEBSOCKET_EVENT_CONNECTED, NULL); }
    return ESP_OK;
}
esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t client)
{
    ++mock_stop_calls;
    assert(s_link.session == 0); /* callback fence precedes transport teardown */
    if (mock_stall_stop) { watchdog_stall(); }
    return ESP_OK;
}
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t client)
{ ++mock_destroy_calls; return ESP_OK; }
bool esp_websocket_client_is_connected(esp_websocket_client_handle_t client) { return true; }
int esp_websocket_client_send_text(esp_websocket_client_handle_t client, const char *text,
                                 int len, TickType_t timeout)
{
    ++mock_send_calls;
    if (mock_stall_send) { watchdog_stall(); }
    return mock_send == INT_MAX ? len : mock_send;
}

/* The JSON/auth mocks isolate transport policy; production auth has its own
 * signing-vector tests. Only the wake dispatch side effect is observed here. */
cJSON *cJSON_CreateObject(void) { return calloc(1, sizeof(cJSON)); }
void cJSON_Delete(cJSON *item) { free(item); }
void cJSON_free(void *item) { free(item); }
char *cJSON_PrintUnformatted(const cJSON *item) { return strdup("{}"); }
cJSON *cJSON_ParseWithLength(const char *text, size_t len) { return cJSON_CreateObject(); }
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *item, const char *name)
{ static cJSON value = {.valuestring = "mock"}; return mock_power_dispatch ? NULL : &value; }
bool cJSON_IsObject(const cJSON *item) { return item != NULL; }
bool cJSON_IsString(const cJSON *item) { return item != NULL; }
bool cJSON_IsNumber(const cJSON *item) { return false; }
bool cJSON_IsBool(const cJSON *item) { return false; }
bool cJSON_IsTrue(const cJSON *item) { return false; }
bool cJSON_IsArray(const cJSON *item) { return false; }
void cJSON_AddNumberToObject(cJSON *item, const char *name, double value) { }
void cJSON_AddStringToObject(cJSON *item, const char *name, const char *value)
{ if (!strcmp(name, "err")) snprintf(mock_error, sizeof(mock_error), "%s", value); }
void cJSON_AddBoolToObject(cJSON *item, const char *name, bool value) { }
void cJSON_AddItemToObject(cJSON *item, const char *name, cJSON *value) { free(value); }
bool rw_config_get_key(const char *kid, uint8_t out[32]) { return true; }
bool rw_config_get_ctr(const char *kid, uint64_t *out) { *out = 0; return true; }
esp_err_t rw_config_set_ctr(const char *kid, uint64_t ctr) { return ESP_OK; }
const char *rw_config_device_id(void) { return "0123456789abcdef"; }
const char *rw_config_device_token(void) { return "mock-token"; }
esp_err_t rw_config_relay_ws_url(char *out, size_t size) { snprintf(out, size, "wss://mock"); return ESP_OK; }
void rw_auth_peek_id(const char *text, size_t len, char *out, size_t size) { out[0] = '\0'; }
rw_auth_err_t rw_auth_verify(const rw_auth_ops_t *ops, const char *text, size_t len, rw_cmd_t *cmd)
{
    ++mock_verify_calls;
    memset(cmd, 0, sizeof(*cmd));
    strcpy(cmd->act, "wake");
    if (mock_drop_in_verify) { rw_relay_notify_network(false); rw_relay_notify_network(true); }
    return RW_OK;
}
const char *rw_auth_err_str(rw_auth_err_t err) { return "mock"; }
void rw_cmd_free(rw_cmd_t *cmd) { }
void rw_led_set(rw_led_state_t state) { }
void rw_led_flash(rw_led_state_t state, uint32_t ms) { }
bool rw_hid_available(void) { return true; }
bool rw_hid_mounted(void) { return true; }
bool rw_hid_suspended(void) { return false; }
esp_err_t rw_hid_wake(void) { ++mock_actions; return ESP_OK; }
esp_err_t rw_hid_type(const char *text, bool enter) { ++mock_actions; return ESP_OK; }
esp_err_t rw_hid_keys(const cJSON *seq) { ++mock_actions; return ESP_OK; }
esp_err_t rw_power_tap(uint32_t ms) { ++mock_actions; return mock_power_result; }
esp_err_t rw_power_hold(uint32_t ms) { ++mock_actions; return mock_power_result; }
