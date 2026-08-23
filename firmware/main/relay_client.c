#include "relay_client.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth.h"
#include "cJSON.h"
#include "config.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led.h"
#include "power_btn.h"
#include "sdkconfig.h"
#include "usb_hid.h"

static const char *TAG = "rw.relay";

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

typedef struct {
    char  *data;
    size_t len;
} rw_rx_msg_t;

static esp_websocket_client_handle_t s_client;
static QueueHandle_t s_rx_queue;
static volatile bool s_net_up;
static volatile bool s_connected;
static volatile bool s_disconnected_evt;
static volatile bool s_sntp_started;
static volatile int64_t s_last_rx_us;    /* any inbound frame, for liveness */
static volatile int  s_close_code;       /* last websocket close status code */

/* Reassembly buffer for fragmented text messages. */
static char   *s_asm;
static size_t  s_asm_len;
static size_t  s_asm_cap;

/* Auth-failure rate limiting. */
static int64_t s_fail_ts[CONFIG_RW_AUTH_FAIL_THRESHOLD];
static int     s_fail_head;
static int64_t s_lock_until_us;

/* ---------------------------------------------------------------- time -- */

static int64_t now_unix(void)
{
    return (int64_t)time(NULL);
}

static bool clock_valid(void)
{
    return now_unix() > CLOCK_SANE_EPOCH;
}

static void sntp_start_once(void)
{
    if (s_sntp_started) {
        return;
    }
    /* The default config already sets start = true and smooth sync off, which
     * is what we want: the ts window needs a step-corrected clock fast. */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
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
    int64_t left = s_lock_until_us - esp_timer_get_time();
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
        s_lock_until_us = now + (int64_t)CONFIG_RW_AUTH_LOCKOUT_SEC * 1000000LL;
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

static void ws_send_json(cJSON *obj)
{
    if (!obj) {
        return;
    }
    char *txt = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!txt) {
        return;
    }
    if (s_client && esp_websocket_client_is_connected(s_client)) {
        int n = esp_websocket_client_send_text(s_client, txt, (int)strlen(txt),
                                               pdMS_TO_TICKS(5000));
        if (n < 0) {
            ESP_LOGW(TAG, "send failed");
        }
    }
    cJSON_free(txt);
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
        const char *pe = esp_err_to_proto(e);
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
 * lacks a string `id` and a string `act` — is silently ignored; it still
 * counts as liveness evidence (recorded by the caller).
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

static void handle_envelope(const char *json, size_t len)
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
        .clock_valid = clock_valid(),
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
    dispatch(&cmd);
    rw_cmd_free(&cmd);
}

/* ----------------------------------------------------------- ws events -- */

static void asm_reset(void)
{
    s_asm_len = 0;
}

static bool asm_append(const char *data, size_t n)
{
    if (s_asm_len + n > RW_MAX_ENVELOPE_BYTES) {
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
    memcpy(s_asm + s_asm_len, data, n);
    s_asm_len += n;
    s_asm[s_asm_len] = '\0';
    return true;
}

static void queue_message(const char *data, size_t len)
{
    rw_rx_msg_t msg = {.data = malloc(len + 1), .len = len};
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

static void ws_event_handler(void *args, esp_event_base_t base, int32_t id,
                             void *event_data)
{
    (void)args;
    (void)base;
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "websocket connected");
        asm_reset();
        s_close_code = 0;
        s_last_rx_us = esp_timer_get_time();
        s_connected = true;
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "websocket disconnected");
        s_connected = false;
        s_disconnected_evt = true;
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "websocket error");
        s_connected = false;
        s_disconnected_evt = true;
        break;

    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "websocket closed by peer");
        s_connected = false;
        s_disconnected_evt = true;
        break;

    case WEBSOCKET_EVENT_DATA:
        if (!d) {
            break;
        }
        s_last_rx_us = esp_timer_get_time();
        /* 0x08 close, 0x09 ping, 0x0A pong are handled by the client itself. */
        if (d->op_code == 0x08) {
            /* Close payload starts with a big-endian status code. */
            if (d->data_len >= 2 && d->data_ptr) {
                s_close_code = ((unsigned char)d->data_ptr[0] << 8) |
                               (unsigned char)d->data_ptr[1];
                ESP_LOGW(TAG, "close frame, code %d", s_close_code);
            }
            s_connected = false;
            s_disconnected_evt = true;
            break;
        }
        if (d->op_code != 0x01 && d->op_code != 0x00) {
            break;  /* binary / control: the protocol is text-only */
        }
        if (d->payload_offset == 0) {
            asm_reset();
        }
        if (d->data_len > 0 && !asm_append(d->data_ptr, (size_t)d->data_len)) {
            asm_reset();
            break;
        }
        if (d->payload_len > 0 &&
            (size_t)(d->payload_offset + d->data_len) >= (size_t)d->payload_len) {
            if (s_asm_len > 0) {
                queue_message(s_asm, s_asm_len);
            }
            asm_reset();
        }
        break;

    default:
        break;
    }
}

/* ---------------------------------------------------------------- task -- */

static void send_hello(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *h = cJSON_CreateObject();
    if (!o || !h) {
        cJSON_Delete(o);
        cJSON_Delete(h);
        return;
    }
    cJSON_AddStringToObject(h, "fw", RW_FW_VERSION);
    cJSON_AddItemToObject(o, "hello", h);
    ws_send_json(o);
    ESP_LOGI(TAG, "-> hello fw=%s", RW_FW_VERSION);
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

static void relay_task(void *arg)
{
    (void)arg;
    uint32_t backoff = BACKOFF_MIN_MS;

    esp_err_t wdt = esp_task_wdt_add(NULL);
    if (wdt != ESP_OK && wdt != ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "task wdt subscribe: %s", esp_err_to_name(wdt));
    }

    char url[RW_RELAY_URL_MAXLEN + 64];
    char auth_hdr[RW_DEVICE_TOKEN_LEN + 32];

    while (true) {
        int64_t next_probe_us = 0;
        esp_task_wdt_reset();

        if (!s_net_up) {
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
        s_connected = false;
        s_disconnected_evt = false;
        drain_queue();

        s_client = esp_websocket_client_init(&cfg);
        if (!s_client) {
            ESP_LOGE(TAG, "client init failed");
            goto backoff_delay;
        }
        esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                      ws_event_handler, NULL);
        if (esp_websocket_client_start(s_client) != ESP_OK) {
            ESP_LOGE(TAG, "client start failed");
            goto teardown;
        }

        /* Wait for the handshake. */
        for (int i = 0; i < 60 && !s_connected && !s_disconnected_evt; i++) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        if (!s_connected) {
            ESP_LOGW(TAG, "handshake timed out");
            goto teardown;
        }

        send_hello();
        backoff = BACKOFF_MIN_MS;   /* a good connection resets the ramp */
        rw_led_set(locked_out() ? RW_LED_LOCKOUT : RW_LED_CONNECTED);
        s_last_rx_us = esp_timer_get_time();
        next_probe_us = s_last_rx_us + (int64_t)WS_PING_SEC * 1000000LL;

        while (s_net_up && s_connected && !s_disconnected_evt) {
            esp_task_wdt_reset();
            rw_rx_msg_t msg;
            if (xQueueReceive(s_rx_queue, &msg, pdMS_TO_TICKS(250)) == pdTRUE) {
                if (looks_like_command(msg.data, msg.len)) {
                    handle_envelope(msg.data, msg.len);
                } else {
                    /* Relay keepalive ({"v":1,"ping":...}) or our own "pong".
                     * Not a command; s_last_rx_us already counted it. */
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
                if (s_client && esp_websocket_client_is_connected(s_client)) {
                    esp_websocket_client_send_text(s_client, "ping", 4,
                                                   pdMS_TO_TICKS(2000));
                }
            }
            if (now_us - s_last_rx_us > (int64_t)RX_SILENCE_MS * 1000LL) {
                ESP_LOGW(TAG, "no frames for %d ms — treating link as dead",
                         RX_SILENCE_MS);
                break;
            }

            if (!locked_out() && s_connected) {
                rw_led_set(RW_LED_CONNECTED);
            }
        }

        if (s_close_code == WS_CLOSE_SUPERSEDED) {
            /* Another websocket for this deviceId took over. Reconnecting
             * immediately would just fight it, so go straight to the cap. */
            ESP_LOGW(TAG, "superseded by another connection (4001)");
            backoff = BACKOFF_MAX_MS;
        }

    teardown:
        rw_led_set(RW_LED_FAULT);
        if (s_client) {
            esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
            esp_websocket_client_destroy(s_client);
            s_client = NULL;
        }
        s_connected = false;

    backoff_delay: {
        uint32_t wait_ms = jitter(backoff);
        ESP_LOGI(TAG, "reconnecting in %u ms", (unsigned)wait_ms);
        for (uint32_t waited = 0; waited < wait_ms; waited += 250) {
            esp_task_wdt_reset();
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
    return s_connected;
}

void rw_relay_notify_network(bool up)
{
    s_net_up = up;
    if (!up) {
        s_connected = false;
        s_disconnected_evt = true;
    }
}
