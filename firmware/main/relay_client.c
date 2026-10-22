#include "relay_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "auth.h"
#include "cJSON.h"
#include "config.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led.h"
#include "power_btn.h"
#include "ota.h"
#include "sdkconfig.h"
#include "usb_hid.h"

static const char *TAG = "rw.relay";

/* Transport teardown has no bounded stop API in esp_websocket_client.
 * A logging-only watchdog cannot recover it. Keep this invariant explicit for
 * existing sdkconfigs as well as fresh builds using sdkconfig.defaults. */
#if !CONFIG_ESP_TASK_WDT_INIT || !CONFIG_ESP_TASK_WDT_PANIC
#error "Relay recovery requires initialized, panic-enabled task watchdog"
#endif

#define RX_QUEUE_DEPTH   4
#define BACKOFF_MIN_MS   1000
#define BACKOFF_MAX_MS   60000
#define WS_PING_SEC      25
#define RX_SILENCE_MS    60000 /* nothing at all from the relay for this long
                                * means the link is dead even if TCP thinks
                                * otherwise — reconnect. */
#define WS_CLOSE_SUPERSEDED 4001 /* relay closed us: another device WS took
                                  * over this deviceId */
#define CLOCK_SANE_EPOCH 1700000000LL /* 2023-11 — anything below is unsynced */

/* ESP32 is 32-bit: volatile does not make timestamps atomic or protect
 * multi-field link transitions between the Wi-Fi, WebSocket and relay tasks. */
typedef struct {
    bool net_up;
    bool connected;
    bool ready;
    bool failed;
    uint32_t network_generation;
    uint32_t session_network_generation;
    uint32_t session;
    int64_t last_rx_us;
    int close_code;
} rw_link_state_t;

typedef struct {
    char *data;
    size_t len;
    uint32_t session;
} rw_rx_msg_t;

static esp_websocket_client_handle_t s_client; /* relay task owns this handle */
static QueueHandle_t s_rx_queue;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static rw_link_state_t s_link;
static uint32_t s_next_session; /* relay task only; zero means no session */
static bool s_clock_synchronized; /* protected by s_state_mux */
static bool s_sntp_started;     /* relay task only */

static rw_link_state_t link_snapshot(void)
{
    portENTER_CRITICAL(&s_state_mux);
    rw_link_state_t state = s_link;
    portEXIT_CRITICAL(&s_state_mux);
    return state;
}

static bool session_valid(uint32_t session)
{
    rw_link_state_t state = link_snapshot();
    return session != 0 && state.session == session && state.net_up &&
           state.session_network_generation == state.network_generation &&
           state.connected && !state.failed;
}

static void link_failed(void)
{
    portENTER_CRITICAL(&s_state_mux);
    s_link.connected = false;
    s_link.ready = false;
    s_link.failed = true;
    portEXIT_CRITICAL(&s_state_mux);
}

static uint32_t begin_session(void)
{
    if (++s_next_session == 0) {
        ++s_next_session;
    }
    portENTER_CRITICAL(&s_state_mux);
    s_link.session = s_next_session;
    s_link.session_network_generation = s_link.network_generation;
    s_link.connected = false;
    s_link.ready = false;
    s_link.failed = !s_link.net_up;
    s_link.close_code = 0;
    s_link.last_rx_us = 0;
    portEXIT_CRITICAL(&s_state_mux);
    return s_next_session;
}

/* Only the WebSocket event task touches assembly while the client runs.
 * The relay task resets it only after stop/destroy have joined that task. */
static char *s_asm;
static size_t s_asm_len;
static size_t s_asm_cap;
static bool s_asm_active;
static bool s_asm_dropping;
static size_t s_frame_offset;
static size_t s_frame_len;
static uint8_t s_frame_opcode;
static bool s_frame_fin;

/* Auth-failure rate limiting. */
static int64_t s_fail_ts[CONFIG_RW_AUTH_FAIL_THRESHOLD];
static int     s_fail_head;
static int64_t s_lock_until_us;

/* ---------------------------------------------------------------- time -- */

static int64_t now_unix(void)
{
    return (int64_t)time(NULL);
}

bool rw_relay_clock_valid(void)
{
    portENTER_CRITICAL(&s_state_mux);
    bool synchronized = s_clock_synchronized;
    portEXIT_CRITICAL(&s_state_mux);
    return synchronized && now_unix() > CLOCK_SANE_EPOCH;
}

static void clock_synchronized(struct timeval *tv)
{
    portENTER_CRITICAL(&s_state_mux);
    s_clock_synchronized = tv && tv->tv_sec > CLOCK_SANE_EPOCH;
    portEXIT_CRITICAL(&s_state_mux);
}

static void sntp_start_once(void)
{
    if (s_sntp_started) {
        return;
    }
    /* The default config already sets start = true and smooth sync off, which
     * is what we want: the ts window needs a step-corrected clock fast. */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = clock_synchronized;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_sntp_started = true;
        ESP_LOGI(TAG, "SNTP started");
    } else {
        ESP_LOGW(TAG, "esp_netif_sntp_init: %s", esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------- lockout -- */

int rw_relay_lockout_remaining(void)
{
    portENTER_CRITICAL(&s_state_mux);
    int64_t until = s_lock_until_us;
    portEXIT_CRITICAL(&s_state_mux);
    int64_t left = until - esp_timer_get_time();
    return left > 0 ? (int)((left + 999999) / 1000000) : 0;
}

static bool locked_out(void)
{
    return rw_relay_lockout_remaining() > 0;
}

static void note_auth_failure(void)
{
    int64_t now = esp_timer_get_time();
    s_fail_ts[s_fail_head] = now;
    s_fail_head = (s_fail_head + 1) % CONFIG_RW_AUTH_FAIL_THRESHOLD;

    /* The slot we are about to overwrite holds the Nth-most-recent failure;
     * if it happened within the last minute we have hit the threshold. */
    int64_t oldest = s_fail_ts[s_fail_head];
    if (oldest != 0 && (now - oldest) <= 60LL * 1000000LL) {
        portENTER_CRITICAL(&s_state_mux);
        s_lock_until_us = now + (int64_t)CONFIG_RW_AUTH_LOCKOUT_SEC * 1000000LL;
        portEXIT_CRITICAL(&s_state_mux);
        memset(s_fail_ts, 0, sizeof(s_fail_ts));
        s_fail_head = 0;
        ESP_LOGE(TAG, "auth-failure lockout for %d s", CONFIG_RW_AUTH_LOCKOUT_SEC);
        rw_led_set(RW_LED_LOCKOUT);
    }
}

/* --------------------------------------------------------------- auth ops -- */

static bool ops_get_key(void *ctx, const char *kid, uint8_t out[32])
{
    (void)ctx;
    return rw_config_get_key(kid, out);
}

static bool ops_get_ctr(void *ctx, const char *kid, uint64_t *out)
{
    (void)ctx;
    return rw_config_get_ctr(kid, out);
}

static esp_err_t ops_set_ctr(void *ctx, const char *kid, uint64_t ctr)
{
    (void)ctx;
    return rw_config_set_ctr(kid, ctr);
}

static bool ops_act_enabled(void *ctx, const char *act)
{
    (void)ctx;
    if (!strcmp(act, "ping") || !strcmp(act, "status") ||
        !strcmp(act, "power_tap") || !strcmp(act, "power_hold")) {
        return true;
    }
    /* wake/type/keys are known actions even without USB — they exist but fail
     * with usb_down, which is a more useful answer than unknown_act. */
    return !strcmp(act, "wake") || !strcmp(act, "type") || !strcmp(act, "keys");
}

/* ------------------------------------------------------------ responses -- */

static bool ws_send_text(const char *text, size_t len, TickType_t timeout)
{
    rw_link_state_t state = link_snapshot();
    if (!s_client || !session_valid(state.session) ||
        !esp_websocket_client_is_connected(s_client)) {
        return false;
    }
    int sent = esp_websocket_client_send_text(s_client, text, (int)len, timeout);
    if (sent != (int)len) {
        ESP_LOGW(TAG, "incomplete send (%d/%u) — marking link disconnected",
                 sent, (unsigned)len);
        link_failed();
        return false;
    }
    return session_valid(state.session);
}

static bool ws_send_json(cJSON *obj)
{
    if (!obj) {
        return false;
    }
    char *txt = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!txt) {
        return false;
    }
    bool sent = ws_send_text(txt, strlen(txt), pdMS_TO_TICKS(5000));
    cJSON_free(txt);
    return sent;
}

static void respond_err(const char *id, const char *err)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return;
    }
    cJSON_AddNumberToObject(o, "v", RW_PROTO_VERSION);
    cJSON_AddStringToObject(o, "id", id ? id : "");
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "err", err);
    ESP_LOGW(TAG, "-> err %s (id=%s)", err, id ? id : "");
    ws_send_json(o);
}

/* Takes ownership of @p res (may be NULL for an empty result object). */
static void respond_ok(const char *id, cJSON *res)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        cJSON_Delete(res);
        return;
    }
    cJSON_AddNumberToObject(o, "v", RW_PROTO_VERSION);
    cJSON_AddStringToObject(o, "id", id ? id : "");
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddItemToObject(o, "res", res ? res : cJSON_CreateObject());
    ws_send_json(o);
}

/* ------------------------------------------------------------- dispatch -- */

static const char *esp_err_to_proto(esp_err_t e)
{
    switch (e) {
    case ESP_OK:                  return NULL;
    case ESP_ERR_NOT_SUPPORTED:   return "usb_down"; /* power-only build */
    case ESP_ERR_NOT_FOUND:       return "usb_down"; /* host has not enumerated us */
    case ESP_ERR_TIMEOUT:         return "usb_down"; /* suspended / endpoint stuck */
    case ESP_ERR_INVALID_STATE:   return "busy";
    case ESP_ERR_INVALID_ARG:     return "bad_args";
    case ESP_ERR_INVALID_SIZE:    return "bad_args";
    default:                      return "internal";
    }
}

static bool arg_uint(const cJSON *args, const char *name, uint32_t *out,
                     uint32_t def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(args, name);
    if (!it) {
        *out = def;
        return true;
    }
    if (!cJSON_IsNumber(it)) {
        return false;
    }
    double d = it->valuedouble;
    if (d < 0 || d > 4294967295.0 || d != (double)(uint32_t)d) {
        return false;
    }
    *out = (uint32_t)d;
    return true;
}

static int64_t uptime_seconds(void)
{
    return esp_timer_get_time() / 1000000;
}

/* "mounted" | "suspended" | "down" — "down" also covers the power-only
 * profile, where there is no USB device peripheral at all. */
static const char *usb_state_str(void)
{
    if (!rw_hid_available() || !rw_hid_mounted()) {
        return "down";
    }
    return rw_hid_suspended() ? "suspended" : "mounted";
}

static void dispatch(const rw_cmd_t *cmd)
{
    rw_led_flash(RW_LED_BUSY, 400);

    if (!strcmp(cmd->act, "ping")) {
        cJSON *res = cJSON_CreateObject();
        cJSON_AddNumberToObject(res, "uptime", (double)uptime_seconds());
        cJSON_AddStringToObject(res, "usb", usb_state_str());
        respond_ok(cmd->id, res);
        return;
    }

    if (!strcmp(cmd->act, "status")) {
        /* Exactly the keys docs/protocol.md specifies for `status`. */
        cJSON *res = cJSON_CreateObject();
        cJSON_AddStringToObject(res, "fw", RW_FW_VERSION);

        wifi_ap_record_t ap;
        int rssi = 0;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            rssi = ap.rssi;
        }
        cJSON_AddNumberToObject(res, "rssi", rssi);
        cJSON_AddStringToObject(res, "usb", usb_state_str());
        cJSON_AddNumberToObject(res, "uptime", (double)uptime_seconds());
        respond_ok(cmd->id, res);
        return;
    }

    if (!strcmp(cmd->act, "wake")) {
        esp_err_t e = rw_hid_wake();
        const char *pe = esp_err_to_proto(e);
        if (pe) {
            respond_err(cmd->id, pe);
        } else {
            respond_ok(cmd->id, NULL);
        }
        return;
    }

    if (!strcmp(cmd->act, "type")) {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(cmd->args, "text");
        const cJSON *en = cJSON_GetObjectItemCaseSensitive(cmd->args, "enter");
        if (!cJSON_IsString(t) || !t->valuestring ||
            (en && !cJSON_IsBool(en))) {
            respond_err(cmd->id, "bad_args");
            return;
        }
        esp_err_t e = rw_hid_type(t->valuestring, en ? cJSON_IsTrue(en) : false);
        const char *pe = esp_err_to_proto(e);
        if (pe) {
            respond_err(cmd->id, pe);
        } else {
            respond_ok(cmd->id, NULL);
        }
        return;
    }

    if (!strcmp(cmd->act, "keys")) {
        const cJSON *seq = cJSON_GetObjectItemCaseSensitive(cmd->args, "seq");
        if (!cJSON_IsArray(seq)) {
            respond_err(cmd->id, "bad_args");
            return;
        }
        esp_err_t e = rw_hid_keys(seq);
        const char *pe = esp_err_to_proto(e);
        if (pe) {
            respond_err(cmd->id, pe);
        } else {
            respond_ok(cmd->id, NULL);
        }
        return;
    }

    if (!strcmp(cmd->act, "power_tap") || !strcmp(cmd->act, "power_hold")) {
        bool hold = cmd->act[6] == 'h';
        uint32_t ms = 0;
        if (!arg_uint(cmd->args, "ms", &ms,
                      hold ? RW_HOLD_MS_DEF : RW_TAP_MS_DEF)) {
            respond_err(cmd->id, "bad_args");
            return;
        }
        esp_err_t e = hold ? rw_power_hold(ms) : rw_power_tap(ms);
        /* Protocol v1 has no power-disabled error; do not mislabel it usb_down. */
        const char *pe = e == ESP_ERR_NOT_SUPPORTED ? "internal" : esp_err_to_proto(e);
        if (pe) {
            respond_err(cmd->id, pe);
        } else {
            cJSON *res = cJSON_CreateObject();
            cJSON_AddNumberToObject(res, "ms", ms);
            respond_ok(cmd->id, res);
        }
        return;
    }

    respond_err(cmd->id, "unknown_act");
}

/*
 * The relay's 25 s keepalive is an application-layer text frame
 * ({"v":1,"ping":<unix>}), not an RFC 6455 control ping, because Cloudflare
 * Workers cannot emit those. Anything that is not a command envelope — i.e.
 * lacks a string `id` and a string `act` — cannot enter command dispatch.
 * Bounded update hints are handled separately; other frames are ignored and
 * still count as liveness evidence (recorded by the caller).
 */
static bool looks_like_command(const char *json, size_t len)
{
    if (!json || len == 0 || json[0] != '{') {
        return false;   /* e.g. the literal "pong" answer to our own probe */
    }
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *id  = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *act = cJSON_GetObjectItemCaseSensitive(root, "act");
    bool ok = cJSON_IsString(id) && cJSON_IsString(act);
    cJSON_Delete(root);
    return ok;
}

/* Hints never enter authenticated keyboard dispatch. This runs on the relay
 * task after the existing bounded/session-fenced RX queue, not the network
 * callback. The OTA API copies a hint to its own bounded worker queue. */
static bool unique_object(const cJSON *object)
{
    if (!cJSON_IsObject(object)) return false;
    for (const cJSON *a = object->child; a; a = a->next) {
        if (!a->string) return false;
        for (const cJSON *b = a->next; b; b = b->next) {
            if (!b->string || !strcmp(a->string, b->string)) return false;
        }
    }
    return true;
}

static bool hint_matches(const cJSON *hint, const char *field, const char *expected)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(hint, field);
    return cJSON_IsString(item) && item->valuestring && !strcmp(item->valuestring, expected);
}

static bool handle_update_hint(const char *json, size_t len, uint32_t session)
{
    if (!json || !len || json[0] != '{') return false;
    cJSON *root = cJSON_ParseWithLength(json, len);
    const cJSON *hint = cJSON_GetObjectItemCaseSensitive(root, "update_available");
    if (!hint) { cJSON_Delete(root); return false; }
    rw_ota_registration_t device;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(hint, "v");
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(hint, "manifest_url");
    if (unique_object(root) && unique_object(hint) && cJSON_IsNumber(v) && v->valuedouble == 1 &&
        cJSON_IsString(url) && url->valuestring && url->valuestring[0] &&
        strnlen(url->valuestring, RW_OTA_URL_MAX + 1) <= RW_OTA_URL_MAX &&
        rw_ota_get_registration(&device) &&
        hint_matches(hint, "product", device.product) && hint_matches(hint, "board", device.board) &&
        hint_matches(hint, "profile", device.profile) && hint_matches(hint, "channel", device.channel) &&
        hint_matches(hint, "partition_layout", device.partition_layout) && session_valid(session)) {
        // Source allowlist, signature, monotonicity and compatibility live in OTA.
        (void)rw_ota_offer_manifest(url->valuestring);
    }
    cJSON_Delete(root);
    return true;
}

static void handle_envelope(const char *json, size_t len, uint32_t session)
{
    char peek_id[RW_ID_MAXLEN] = {0};
    rw_auth_peek_id(json, len, peek_id, sizeof(peek_id));

    if (locked_out()) {
        ESP_LOGW(TAG, "dropping command during lockout (%d s left)",
                 rw_relay_lockout_remaining());
        respond_err(peek_id, "locked");
        return;
    }

    rw_auth_ops_t ops = {
        .device_id   = rw_config_device_id(),
        .now         = now_unix(),
        .ts_window   = CONFIG_RW_TS_WINDOW_SEC,
        .clock_valid = rw_relay_clock_valid(),
        .ctx         = NULL,
        .get_key     = ops_get_key,
        .get_ctr     = ops_get_ctr,
        .set_ctr     = ops_set_ctr,
        .act_enabled = ops_act_enabled,
    };

    rw_cmd_t cmd;
    rw_auth_err_t rc = rw_auth_verify(&ops, json, len, &cmd);
    if (rc != RW_OK) {
        ESP_LOGW(TAG, "verify failed: %s", rw_auth_err_str(rc));
        /* Only signature/identity failures count toward the lockout; a stale
         * clock or an unknown action is a client bug, not an attack. */
        if (rc == RW_ERR_SIG || rc == RW_ERR_UNKNOWN_KEY || rc == RW_ERR_REPLAY ||
            rc == RW_ERR_DEV || rc == RW_ERR_BAD_JSON) {
            note_auth_failure();
        }
        respond_err(peek_id[0] ? peek_id : cmd.id, rw_auth_err_str(rc));
        rw_cmd_free(&cmd);
        return;
    }

    ESP_LOGI(TAG, "<- %s (kid=%s ctr=%llu)", cmd.act, cmd.kid,
             (unsigned long long)cmd.ctr);
    /* A network loss during verification must not execute an old command. */
    if (session_valid(session)) {
        dispatch(&cmd);
    }
    rw_cmd_free(&cmd);
}

/* ----------------------------------------------------------- ws events -- */

static void asm_reset(void)
{
    s_asm_len = 0;
    s_asm_active = false;
    s_asm_dropping = false;
    s_frame_offset = 0;
    s_frame_len = 0;
}

static bool asm_append(const char *data, size_t n)
{
    if (n > RW_MAX_ENVELOPE_BYTES - s_asm_len) {
        ESP_LOGW(TAG, "envelope over %d bytes — dropping", RW_MAX_ENVELOPE_BYTES);
        return false;
    }
    if (s_asm_len + n + 1 > s_asm_cap) {
        size_t cap = s_asm_cap ? s_asm_cap : 512;
        while (cap < s_asm_len + n + 1) {
            cap *= 2;
        }
        char *nb = realloc(s_asm, cap);
        if (!nb) {
            return false;
        }
        s_asm = nb;
        s_asm_cap = cap;
    }
    if (n > 0) {
        memcpy(s_asm + s_asm_len, data, n);
    }
    s_asm_len += n;
    s_asm[s_asm_len] = '\0';
    return true;
}

static void queue_message(const char *data, size_t len, uint32_t session)
{
    rw_rx_msg_t msg = {.data = malloc(len + 1), .len = len, .session = session};
    if (!msg.data) {
        return;
    }
    memcpy(msg.data, data, len);
    msg.data[len] = '\0';
    if (xQueueSend(s_rx_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "rx queue full — dropping command");
        free(msg.data);
    }
}

/* payload_offset/len describe one frame, while FIN terminates the whole
 * message. A continuation frame has offset zero again; a large frame also
 * arrives in multiple callbacks. Keep both boundaries independently. */
static void assemble_text(const esp_websocket_event_data_t *d, uint32_t session)
{
    if (d->data_len < 0 || d->payload_len < 0 || d->payload_offset < 0 ||
        d->payload_offset > d->payload_len ||
        d->data_len > d->payload_len - d->payload_offset ||
        (d->data_len > 0 && !d->data_ptr)) {
        s_asm_dropping = true;
        return;
    }
    if (d->payload_offset == 0) {
        if (d->op_code == 0x01) {
            asm_reset();
            s_asm_active = true;
        } else if (!s_asm_active || s_frame_offset != s_frame_len) {
            s_asm_dropping = true;
        }
        s_frame_offset = 0;
        s_frame_len = (size_t)d->payload_len;
        s_frame_opcode = d->op_code;
        s_frame_fin = d->fin;
    }
    if (!s_asm_active || s_asm_dropping) {
        return;
    }
    if ((size_t)d->payload_offset != s_frame_offset ||
        (size_t)d->payload_len != s_frame_len ||
        d->op_code != s_frame_opcode || d->fin != s_frame_fin ||
        !asm_append(d->data_ptr, (size_t)d->data_len)) {
        /* Never reinterpret a suffix after malformed input, overflow or OOM. */
        s_asm_dropping = true;
        return;
    }
    s_frame_offset += (size_t)d->data_len;
    if (s_frame_offset == s_frame_len && d->fin) {
        if (s_asm_len > 0) {
            queue_message(s_asm, s_asm_len, session);
        }
        asm_reset();
    }
}

static void ws_event_handler(void *args, esp_event_base_t base, int32_t id,
                             void *event_data)
{
    (void)base;
    uint32_t session = (uint32_t)(uintptr_t)args;
    esp_websocket_event_data_t *d = event_data;

    portENTER_CRITICAL(&s_state_mux);
    bool current = session != 0 && s_link.session == session && s_link.net_up &&
        s_link.session_network_generation == s_link.network_generation &&
        !s_link.failed;
    if (current) {
        switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            s_link.close_code = 0;
            s_link.last_rx_us = esp_timer_get_time();
            s_link.connected = true;
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_ERROR:
        case WEBSOCKET_EVENT_CLOSED:
            s_link.connected = false;
            s_link.ready = false;
            s_link.failed = true;
            break;
        case WEBSOCKET_EVENT_DATA:
            if (d && s_link.connected) {
                s_link.last_rx_us = esp_timer_get_time();
                if (d->op_code == 0x08) {
                    if (d->payload_offset == 0 && d->data_len >= 2 && d->data_ptr) {
                        s_link.close_code = ((unsigned char)d->data_ptr[0] << 8) |
                                            (unsigned char)d->data_ptr[1];
                    }
                    s_link.connected = false;
                    s_link.ready = false;
                    s_link.failed = true;
                }
            }
            break;
        default:
            break;
        }
    }
    portEXIT_CRITICAL(&s_state_mux);
    if (!current) {
        return; /* stale callback after a network loss, even if IP returned */
    }

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "websocket connected");
        asm_reset();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "websocket ended (event %ld)", (long)id);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (!d) {
            break;
        }
        if (d->op_code == 0x08) {
            ESP_LOGW(TAG, "close frame, code %d", link_snapshot().close_code);
        } else if (session_valid(session)) {
            if (d->op_code == 0x01 || d->op_code == 0x00) {
                assemble_text(d, session);
            } else if (d->op_code == 0x02) {
                /* Binary messages and their continuations are not commands. */
                asm_reset();
            }
            /* Control ping/pong may be interleaved with a fragmented message. */
        }
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- task -- */

static bool send_hello(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *h = cJSON_CreateObject();
    if (!o || !h) {
        cJSON_Delete(o);
        cJSON_Delete(h);
        return false;
    }
    cJSON_AddStringToObject(h, "fw", RW_FW_VERSION);
    rw_ota_registration_t identity;
    if (rw_ota_get_registration(&identity)) {
        cJSON *update = cJSON_CreateObject();
        if (!update) { cJSON_Delete(o); cJSON_Delete(h); return false; }
        cJSON_AddNumberToObject(update, "v", 1);
        cJSON_AddNumberToObject(update, "updater_api", identity.updater_api);
        cJSON_AddStringToObject(update, "product", identity.product);
        cJSON_AddStringToObject(update, "board", identity.board);
        cJSON_AddStringToObject(update, "profile", identity.profile);
        cJSON_AddStringToObject(update, "channel", identity.channel);
        cJSON_AddStringToObject(update, "partition_layout", identity.partition_layout);
        cJSON_AddStringToObject(update, "version", identity.version);
        cJSON_AddNumberToObject(update, "release_counter", identity.release_counter);
        cJSON_AddItemToObject(h, "update", update);
    }
    cJSON_AddItemToObject(o, "hello", h);
    bool sent = ws_send_json(o);
    if (sent) {
        ESP_LOGI(TAG, "-> hello fw=%s", RW_FW_VERSION);
    }
    return sent;
}

static uint32_t jitter(uint32_t ms)
{
    /* +/- 20 % */
    uint32_t span = ms / 5;
    if (span == 0) {
        return ms;
    }
    return ms - span + (esp_random() % (2 * span + 1));
}

static void drain_queue(void)
{
    rw_rx_msg_t msg;
    while (xQueueReceive(s_rx_queue, &msg, 0) == pdTRUE) {
        free(msg.data);
    }
}

static void teardown_client(void)
{
    /* Fence callbacks before touching the client. stop() joins the event task
     * before we drain its queue and reset its assembly buffer. */
    portENTER_CRITICAL(&s_state_mux);
    s_link.session = 0;
    s_link.connected = false;
    s_link.ready = false;
    s_link.failed = true;
    portEXIT_CRITICAL(&s_state_mux);
    if (s_client) {
        /* Recovery should abort TCP, not attempt a close handshake on a dead
         * link. Even esp_websocket_client close(timeout) can block
         * forever acquiring its lock; stop/destroy can also wait indefinitely.
         * The subscribed relay task + panic-enabled TWDT bounds those waits. */
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    drain_queue();
    asm_reset();
}

/* An unreachable relay never reboots the controller: a restart cannot fix an
 * outside cause (relay down, DNS, URL, rejected token) and re-enumerating USB
 * can wake the host. The loop recreates the client indefinitely at the capped
 * backoff; the task watchdog only bounds a genuinely blocked call. */
static void relay_task(void *arg)
{
    (void)arg;
    uint32_t backoff = BACKOFF_MIN_MS;

    esp_err_t wdt = esp_task_wdt_add(NULL);
    if (wdt != ESP_OK) {
        /* Without this subscription a blocked transport stop could strand the
         * device forever; fail closed instead of silently losing recovery. */
        ESP_LOGE(TAG, "task wdt subscribe failed: %s — rebooting", esp_err_to_name(wdt));
        esp_restart();
        vTaskDelete(NULL);
        return;
    }

    char url[RW_RELAY_URL_MAXLEN + 64];
    char auth_hdr[RW_DEVICE_TOKEN_LEN + 32];

    while (true) {
        int64_t next_probe_us = 0;
        esp_task_wdt_reset();

        if (!link_snapshot().net_up) {
            if (s_client) {
                teardown_client();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        sntp_start_once();

        if (rw_config_relay_ws_url(url, sizeof(url)) != ESP_OK) {
            ESP_LOGE(TAG, "no usable relay URL");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s\r\n",
                 rw_config_device_token());

        esp_websocket_client_config_t cfg = {
            .uri                    = url,
            .headers                = auth_hdr,
            .disable_auto_reconnect = true,
            .task_stack             = 6144,
            .task_prio              = 5,
            .buffer_size            = 2048,
            .network_timeout_ms     = 10000,
            .ping_interval_sec      = WS_PING_SEC,
            /* Cloudflare Workers cannot emit RFC 6455 control frames, so a
             * missing pong means nothing here. Liveness is decided by
             * RX_SILENCE_MS against the relay's application-layer keepalive
             * instead of by the transport. */
            .disable_pingpong_discon = true,
            .crt_bundle_attach      = esp_crt_bundle_attach,
        };

        ESP_LOGI(TAG, "connecting to %s", url);
        rw_led_set(RW_LED_CONNECTING);
        uint32_t session = begin_session();
        drain_queue();

        s_client = esp_websocket_client_init(&cfg);
        if (!s_client) {
            ESP_LOGE(TAG, "client init failed");
            goto backoff_delay;
        }
        if (esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                         ws_event_handler, (void *)(uintptr_t)session) != ESP_OK) {
            ESP_LOGE(TAG, "event registration failed");
            goto teardown;
        }
        if (esp_websocket_client_start(s_client) != ESP_OK) {
            ESP_LOGE(TAG, "client start failed");
            goto teardown;
        }

        /* Wait for the handshake. */
        for (int i = 0; i < 60; i++) {
            rw_link_state_t state = link_snapshot();
            if (state.connected || state.failed || !state.net_up) {
                break;
            }
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        if (!session_valid(session)) {
            ESP_LOGW(TAG, "handshake timed out or aborted");
            goto teardown;
        }

        if (!send_hello()) {
            goto teardown;
        }
        portENTER_CRITICAL(&s_state_mux);
        if (s_link.session == session && s_link.net_up && !s_link.failed) {
            s_link.ready = true;
        }
        portEXIT_CRITICAL(&s_state_mux);
        backoff = BACKOFF_MIN_MS;   /* a good connection resets the ramp */
        rw_led_set(locked_out() ? RW_LED_LOCKOUT : RW_LED_CONNECTED);
        next_probe_us = esp_timer_get_time() + (int64_t)WS_PING_SEC * 1000000LL;

        while (session_valid(session)) {
            esp_task_wdt_reset();
            rw_rx_msg_t msg;
            if (xQueueReceive(s_rx_queue, &msg, pdMS_TO_TICKS(250)) == pdTRUE) {
                if (msg.session != session || !session_valid(msg.session)) {
                    /* A message queued before IP loss cannot run on its return. */
                } else if (looks_like_command(msg.data, msg.len)) {
                    handle_envelope(msg.data, msg.len, msg.session);
                } else if (handle_update_hint(msg.data, msg.len, msg.session)) {
                    /* Unsigned advisory metadata, never a wake/power command. */
                } else {
                    /* Relay keepalive ({"v":1,"ping":...}) or our own "pong".
                     * Not a command; the callback already counted it. */
                    ESP_LOGD(TAG, "non-command frame (%u B) ignored",
                             (unsigned)msg.len);
                }
                free(msg.data);
                esp_task_wdt_reset();
            }

            int64_t now_us = esp_timer_get_time();
            /* Cheap client-side liveness probe: the relay answers the literal
             * text frame "ping" with "pong" without waking the device
             * object. */
            if (now_us >= next_probe_us) {
                next_probe_us = now_us + (int64_t)WS_PING_SEC * 1000000LL;
                if (!ws_send_text("ping", 4, pdMS_TO_TICKS(2000))) {
                    ESP_LOGW(TAG, "ping send failed — link broken");
                    break;
                }
            }
            if (now_us - link_snapshot().last_rx_us > (int64_t)RX_SILENCE_MS * 1000LL) {
                ESP_LOGW(TAG, "no frames for %d ms — treating link as dead",
                         RX_SILENCE_MS);
                break;
            }

            if (!locked_out() && session_valid(session)) {
                rw_led_set(RW_LED_CONNECTED);
            }
        }

        if (link_snapshot().close_code == WS_CLOSE_SUPERSEDED) {
            /* Another websocket for this deviceId took over. Reconnecting
             * immediately would just fight it, so go straight to the cap. */
            ESP_LOGW(TAG, "superseded by another connection (4001)");
            backoff = BACKOFF_MAX_MS;
        }

    teardown:
        rw_led_set(RW_LED_FAULT);
        teardown_client();

    backoff_delay: {
        uint32_t wait_ms = jitter(backoff);
        ESP_LOGI(TAG, "reconnecting in %u ms", (unsigned)wait_ms);
        for (uint32_t waited = 0; waited < wait_ms; waited += 250) {
            esp_task_wdt_reset();
            if (!link_snapshot().net_up) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        backoff = backoff >= BACKOFF_MAX_MS / 2 ? BACKOFF_MAX_MS : backoff * 2;
    }
    }
}

/* ----------------------------------------------------------------- api -- */

esp_err_t rw_relay_start(void)
{
    if (s_rx_queue) {
        return ESP_OK;
    }
    s_rx_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(rw_rx_msg_t));
    if (!s_rx_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(relay_task, "rw_relay", 8192, NULL, 5, NULL) != pdPASS) {
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool rw_relay_connected(void)
{
    return link_snapshot().ready;
}

void rw_relay_notify_network(bool up)
{
    portENTER_CRITICAL(&s_state_mux);
    if (s_link.net_up != up) {
        ++s_link.network_generation;
    }
    s_link.net_up = up;
    if (!up) {
        s_link.connected = false;
        s_link.ready = false;
        s_link.failed = true;
    }
    portEXIT_CRITICAL(&s_state_mux);
}
