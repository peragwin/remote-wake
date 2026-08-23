#include "provisioning.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "b64url.h"
#include "cJSON.h"
#include "config.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "qrcode.h"
#include "relay_client.h"
#include "sdkconfig.h"

static const char *TAG = "rw.prov";

#define MAX_BODY 2048
#define AP_CHANNEL 6
#define AP_MAX_CONN 2

extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[]   asm("_binary_setup_html_end");

static httpd_handle_t  s_httpd;
static esp_netif_t    *s_ap_netif;
static bool            s_active;

bool rw_prov_active(void)
{
    return s_active;
}

/* ---------------------------------------------------------------- util -- */

/* Read the whole request body (bounded) into a NUL-terminated heap buffer. */
static char *read_body(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > MAX_BODY) {
        return NULL;
    }
    char *buf = malloc((size_t)total + 1);
    if (!buf) {
        return NULL;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[total] = '\0';
    return buf;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *obj)
{
    char *txt = obj ? cJSON_PrintUnformatted(obj) : NULL;
    cJSON_Delete(obj);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, txt ? txt : "{}");
    if (txt) {
        cJSON_free(txt);
    }
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status,
                            const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "err", msg);
    ESP_LOGW(TAG, "%s: %s", status, msg);
    return send_json(req, status, o);
}

/* ------------------------------------------------------------- handlers -- */

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, setup_html_start,
                           (ssize_t)(setup_html_end - setup_html_start - 1));
}

static esp_err_t h_info(httpd_req_t *req)
{
    char ssid[RW_SSID_MAXLEN] = {0};
    char pass[RW_PASS_MAXLEN] = {0};
    char relay[RW_RELAY_URL_MAXLEN] = {0};
    rw_config_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass));
    rw_config_get_relay_url(relay, sizeof(relay));

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "deviceId", rw_config_device_id());
    cJSON_AddNumberToObject(o, "keys", rw_config_key_count());
    cJSON_AddNumberToObject(o, "maxKeys", RW_MAX_KEY_SLOTS);
    cJSON_AddStringToObject(o, "ssid", ssid);
    cJSON_AddStringToObject(o, "relayUrl", relay);
    cJSON_AddStringToObject(o, "fw", RW_FW_VERSION);
    /* Deliberately NOT the device token: that is only handed out by /pair. */
    return send_json(req, "200 OK", o);
}

static esp_err_t h_config(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        return send_error(req, "400 Bad Request", "bad_body");
    }
    cJSON *in = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsObject(in)) {
        cJSON_Delete(in);
        return send_error(req, "400 Bad Request", "bad_json");
    }

    const cJSON *j_ssid  = cJSON_GetObjectItemCaseSensitive(in, "ssid");
    const cJSON *j_pass  = cJSON_GetObjectItemCaseSensitive(in, "pass");
    const cJSON *j_relay = cJSON_GetObjectItemCaseSensitive(in, "relay");

    esp_err_t err = ESP_OK;
    if (cJSON_IsString(j_ssid) && j_ssid->valuestring[0]) {
        err = rw_config_set_wifi(j_ssid->valuestring,
                                 cJSON_IsString(j_pass) ? j_pass->valuestring : "");
    } else {
        err = ESP_ERR_INVALID_ARG;
    }
    if (err == ESP_OK && cJSON_IsString(j_relay) && j_relay->valuestring[0]) {
        err = rw_config_set_relay_url(j_relay->valuestring);
    }
    cJSON_Delete(in);

    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", esp_err_to_name(err));
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    ESP_LOGI(TAG, "network settings updated");
    return send_json(req, "200 OK", o);
}

static esp_err_t h_pair(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        return send_error(req, "400 Bad Request", "bad_body");
    }
    cJSON *in = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsObject(in)) {
        cJSON_Delete(in);
        return send_error(req, "400 Bad Request", "bad_json");
    }

    const cJSON *j_pub  = cJSON_GetObjectItemCaseSensitive(in, "pubkey");
    const cJSON *j_name = cJSON_GetObjectItemCaseSensitive(in, "name");
    if (!cJSON_IsString(j_pub) || !j_pub->valuestring) {
        cJSON_Delete(in);
        return send_error(req, "400 Bad Request", "bad_pubkey");
    }

    uint8_t pub[32];
    int n = rw_b64url_decode(j_pub->valuestring, strlen(j_pub->valuestring), pub,
                             sizeof(pub));
    if (n != 32) {
        cJSON_Delete(in);
        return send_error(req, "400 Bad Request", "bad_pubkey");
    }

    char kid[4] = {0};
    esp_err_t err = rw_config_add_key(
        pub, cJSON_IsString(j_name) ? j_name->valuestring : NULL, kid);
    cJSON_Delete(in);

    if (err == ESP_ERR_NO_MEM) {
        return send_error(req, "409 Conflict", "no_free_slot");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }

    char relay[RW_RELAY_URL_MAXLEN] = {0};
    rw_config_get_relay_url(relay, sizeof(relay));

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "deviceId", rw_config_device_id());
    cJSON_AddStringToObject(o, "deviceToken", rw_config_device_token());
    cJSON_AddStringToObject(o, "relayUrl", relay);
    cJSON_AddStringToObject(o, "kid", kid);
    ESP_LOGI(TAG, "paired new operator key as %s", kid);
    return send_json(req, "200 OK", o);
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200));
    ESP_LOGW(TAG, "rebooting into normal mode");
    esp_restart();
}

static esp_err_t h_finish(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "rebooting", true);
    esp_err_t err = send_json(req, "200 OK", o);
    xTaskCreate(reboot_task, "rw_reboot", 2048, NULL, 5, NULL);
    return err;
}

/* --------------------------------------------------------------- softap -- */

void rw_prov_print_credentials(void)
{
    char ssid[40], pass[16];
    rw_config_softap_ssid(ssid, sizeof(ssid));
    rw_config_softap_password(pass, sizeof(pass));

    char qr[128];
    snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:%s;P:%s;;", ssid, pass);

    printf("\n");
    printf("======================================================\n");
    printf("  remote-wake SETUP MODE\n");
    printf("  Join this Wi-Fi network, then open http://192.168.4.1\n");
    printf("\n");
    printf("    SSID     : %s\n", ssid);
    printf("    Password : %s\n", pass);
    printf("    deviceId : %s\n", rw_config_device_id());
    printf("\n");
    printf("  QR payload: %s\n", qr);
    printf("======================================================\n\n");

    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    esp_qrcode_generate(&cfg, qr);
    printf("\n");
}

static esp_err_t start_ap(void)
{
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            return ESP_FAIL;
        }
    }

    char ssid[40], pass[16];
    rw_config_softap_ssid(ssid, sizeof(ssid));
    rw_config_softap_password(pass, sizeof(pass));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.ap.ssid, ssid, sizeof(wc.ap.ssid));
    wc.ap.ssid_len       = (uint8_t)strlen(ssid);
    strlcpy((char *)wc.ap.password, pass, sizeof(wc.ap.password));
    wc.ap.channel        = AP_CHANNEL;
    wc.ap.max_connection = AP_MAX_CONN;
    wc.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    wc.ap.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wc), TAG, "set_config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi_start");
    ESP_LOGI(TAG, "SoftAP \"%s\" up on channel %d", ssid, AP_CHANNEL);
    return ESP_OK;
}

/* ------------------------------------------------------------------ api -- */

esp_err_t rw_prov_start(void)
{
    if (s_active) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(start_ap(), TAG, "softap");

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.server_port      = 80;
    hc.max_uri_handlers = 8;
    hc.lru_purge_enable = true;
    hc.stack_size       = 5120;

    esp_err_t err = httpd_start(&s_httpd, &hc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t uris[] = {
        {.uri = "/",        .method = HTTP_GET,  .handler = h_root},
        {.uri = "/info",    .method = HTTP_GET,  .handler = h_info},
        {.uri = "/config",  .method = HTTP_POST, .handler = h_config},
        {.uri = "/pair",    .method = HTTP_POST, .handler = h_pair},
        {.uri = "/finish",  .method = HTTP_POST, .handler = h_finish},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &uris[i]), TAG,
                            "register %s", uris[i].uri);
    }

    s_active = true;
    rw_prov_print_credentials();
    return ESP_OK;
}

esp_err_t rw_prov_stop(void)
{
    if (!s_active) {
        return ESP_OK;
    }
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    esp_wifi_stop();
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    s_active = false;
    ESP_LOGI(TAG, "setup mode stopped");
    return ESP_OK;
}
