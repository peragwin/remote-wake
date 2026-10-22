/* Setup-mode HTTP contract for the relay URL: /info, /config and /pair as a
 * client sees them. rw_prov_start() registers the production handlers with a
 * fake esp_http_server; requests are dispatched by URI and method, and the
 * JSON replies are parsed with real cJSON. Storage is the production config.c
 * over fake_nvs.c. The compile-time default is a real relay URL using wss://,
 * as Kconfig suggests. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "b64url.h"
#include "cJSON.h"
#include "captive_dns.h"
#include "config.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "nvs.h"
#include "provisioning.h"
#include "qrcode.h"
#include "sdkconfig.h"

#define DEFAULT_URL CONFIG_RW_RELAY_URL

/* ------------------------------------------------------- boundary fakes -- */

const char *esp_err_to_name(esp_err_t err)
{
    static char buf[16];
    snprintf(buf, sizeof buf, "0x%x", (unsigned)err);
    return buf;
}
void esp_fill_random(void *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) ((unsigned char *)buf)[i] = (unsigned char)(0x3c + 7 * i);
}
uint32_t esp_random(void) { return 0x2468aceu; }
void esp_restart(void) { fprintf(stderr, "setup handler restarted the device\n"); abort(); }
int64_t esp_timer_get_time(void) { return 0; }
int xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                unsigned prio, TaskHandle_t *handle)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)handle; return pdPASS; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }

const char fake_setup_html[] __asm__("_binary_setup_html_start") = "<html></html>";
const char fake_setup_html_end[] __asm__("_binary_setup_html_end") = "";

static int s_netif_obj;
esp_netif_t *esp_netif_create_default_wifi_ap(void) { return (esp_netif_t *)&s_netif_obj; }
void esp_netif_destroy_default_wifi(void *netif) { (void)netif; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *info)
{ (void)n; memset(info, 0, sizeof *info); info->ip.addr = 0x0104a8c0u; return ESP_OK; }
esp_err_t esp_netif_dhcps_stop(esp_netif_t *n) { (void)n; return ESP_OK; }
esp_err_t esp_netif_dhcps_start(esp_netif_t *n) { (void)n; return ESP_OK; }
esp_err_t esp_netif_dhcps_option(esp_netif_t *n, esp_netif_dhcp_option_mode_t op,
                                 esp_netif_dhcp_option_id_t id, void *val, uint32_t len)
{ (void)n; (void)op; (void)id; (void)val; (void)len; return ESP_OK; }
esp_err_t esp_netif_set_dns_info(esp_netif_t *n, esp_netif_dns_type_t t, esp_netif_dns_info_t *d)
{ (void)n; (void)t; (void)d; return ESP_OK; }
esp_err_t esp_wifi_set_mode(wifi_mode_t mode) { (void)mode; return ESP_OK; }
esp_err_t esp_wifi_set_config(wifi_interface_t i, wifi_config_t *c) { (void)i; (void)c; return ESP_OK; }
esp_err_t esp_wifi_start(void) { return ESP_OK; }
esp_err_t esp_wifi_stop(void) { return ESP_OK; }
esp_err_t esp_qrcode_generate(esp_qrcode_config_t *cfg, const char *text)
{ (void)cfg; (void)text; return ESP_OK; }
esp_err_t rw_captive_dns_start(uint32_t ap_address) { (void)ap_address; return ESP_OK; }
void rw_captive_dns_stop(void) {}

/* esp_http_server: a registration table and a captured response. */
static httpd_uri_t s_routes[8];
static size_t s_n_routes;
static int s_server;

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{ (void)config; *handle = &s_server; return ESP_OK; }
esp_err_t httpd_stop(httpd_handle_t handle) { (void)handle; s_n_routes = 0; return ESP_OK; }
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri)
{
    (void)handle;
    assert(s_n_routes < sizeof s_routes / sizeof s_routes[0]);
    s_routes[s_n_routes++] = *uri;
    return ESP_OK;
}
esp_err_t httpd_register_err_handler(httpd_handle_t handle, httpd_err_code_t error,
                                     esp_err_t (*handler)(httpd_req_t *, httpd_err_code_t))
{ (void)handle; (void)error; (void)handler; return ESP_OK; }
int httpd_req_recv(httpd_req_t *req, char *buf, size_t len)
{
    size_t left = req->content_len - req->body_off;
    size_t n = len < left ? len : left;
    memcpy(buf, req->body + req->body_off, n);
    req->body_off += n;
    return (int)n;
}
size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *field)
{ (void)req; (void)field; return 0; }
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *field, char *val, size_t len)
{ (void)req; (void)field; (void)val; (void)len; return ESP_ERR_NOT_FOUND; }
esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status)
{ snprintf(req->status, sizeof req->status, "%s", status); return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type) { (void)req; (void)type; return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *f, const char *v)
{ (void)req; (void)f; (void)v; return ESP_OK; }
esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *str)
{
    assert(!req->sent && strlen(str) < sizeof req->resp);
    strcpy(req->resp, str);
    req->sent = 1;
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, ssize_t len)
{ (void)buf; (void)len; req->sent = 1; return ESP_OK; }
esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t error, const char *msg)
{ (void)msg; snprintf(req->status, sizeof req->status, "%d", (int)error); req->sent = 1; return ESP_OK; }

/* -------------------------------------------------------------- client -- */

typedef struct {
    int status;   /* numeric HTTP status */
    cJSON *json;  /* parsed reply, owned by the caller */
} reply_t;

static reply_t request(int method, const char *uri, const char *body)
{
    httpd_req_t req = {.method = method, .body = body,
                       .content_len = body ? strlen(body) : 0};
    for (size_t i = 0; i < s_n_routes; i++) {
        if (s_routes[i].method == method && strcmp(s_routes[i].uri, uri) == 0) {
            assert(s_routes[i].handler(&req) == ESP_OK);
            assert(req.sent);
            reply_t r = {atoi(req.status), cJSON_Parse(req.resp)};
            assert(r.json);
            return r;
        }
    }
    fprintf(stderr, "no handler registered for %s\n", uri);
    abort();
}

static void fail(int line, const char *what, const char *got, const char *want)
{
    fprintf(stderr, "test_setup_http.c:%d: %s\n  got:  %s\n  want: %s\n", line, what,
            got ? got : "(null)", want ? want : "(null)");
    abort();
}

static const char *str_field(const reply_t *r, const char *name)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(r->json, name);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* Asserts the /info relay fields as a whole. */
static void expect_info(int line, const char *relay_url, bool override)
{
    reply_t r = request(HTTP_GET, "/info", NULL);
    if (r.status != 200) fail(line, "/info status", "not 200", "200");
    const char *url = str_field(&r, "relayUrl");
    if (!url || strcmp(url, relay_url) != 0) fail(line, "/info relayUrl", url, relay_url);
    const char *def = str_field(&r, "relayDefault");
    if (!def || strcmp(def, DEFAULT_URL) != 0) fail(line, "/info relayDefault", def, DEFAULT_URL);
    const cJSON *ov = cJSON_GetObjectItemCaseSensitive(r.json, "relayOverride");
    if (!cJSON_IsBool(ov) || cJSON_IsTrue(ov) != override)
        fail(line, "/info relayOverride", cJSON_IsBool(ov) ? (cJSON_IsTrue(ov) ? "true" : "false")
                                                           : "(not a bool)",
             override ? "true" : "false");
    cJSON_Delete(r.json);
}

static int post_config(const char *json)
{
    reply_t r = request(HTTP_POST, "/config", json);
    int status = r.status;
    cJSON_Delete(r.json);
    return status;
}

static int post_config_relay(const char *relay)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ssid", "home");
    cJSON_AddStringToObject(o, "pass", "password1");
    cJSON_AddStringToObject(o, "relay", relay);
    char *txt = cJSON_PrintUnformatted(o);
    int status = post_config(txt);
    cJSON_free(txt);
    cJSON_Delete(o);
    return status;
}

/* Setup mode starts once; the handlers read configuration per request. */
static void start_setup_mode(void)
{
    fake_nvs_wipe();
    assert(rw_config_init() == ESP_OK);
    fflush(stdout);
    int saved = dup(STDOUT_FILENO); /* the credential banner is not under test */
    int null = open("/dev/null", O_WRONLY);
    assert(saved >= 0 && null >= 0 && dup2(null, STDOUT_FILENO) >= 0);
    esp_err_t err = rw_prov_start();
    fflush(stdout);
    assert(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);
    close(null);
    assert(err == ESP_OK && rw_prov_active());
}

static void fresh_device(void)
{
    fake_nvs_wipe();
    assert(rw_config_init() == ESP_OK);
}

/* --------------------------------------------------------------- tests -- */

static void test_info_without_override(void)
{
    fresh_device();
    expect_info(__LINE__, DEFAULT_URL, false);
}

static void test_config_relay_override_lifecycle(void)
{
    fresh_device();
    if (post_config_relay("https://mine.relay.test") != 200)
        fail(__LINE__, "/config override", "non-200", "200");
    expect_info(__LINE__, "https://mine.relay.test", true);

    /* Missing or empty relay leaves the stored override alone. */
    if (post_config("{\"ssid\":\"home\",\"pass\":\"password1\"}") != 200)
        fail(__LINE__, "/config without relay", "non-200", "200");
    expect_info(__LINE__, "https://mine.relay.test", true);
    if (post_config_relay("") != 200)
        fail(__LINE__, "/config empty relay", "non-200", "200");
    expect_info(__LINE__, "https://mine.relay.test", true);

    /* A placeholder host is refused and changes nothing. */
    if (post_config_relay("https://Relay.Example.com") < 400)
        fail(__LINE__, "/config placeholder", "accepted", "4xx");
    expect_info(__LINE__, "https://mine.relay.test", true);

    /* The exact default returns the device to "no override". */
    if (post_config_relay(DEFAULT_URL) != 200)
        fail(__LINE__, "/config default", "non-200", "200");
    expect_info(__LINE__, DEFAULT_URL, false);
}

static void test_config_relay_length_limit(void)
{
    char url[RW_RELAY_URL_MAXLEN + 1];
    memset(url, 'u', sizeof url);
    memcpy(url, "https://", 8);

    fresh_device();
    url[RW_RELAY_URL_MAXLEN] = '\0'; /* one too long */
    if (post_config_relay(url) < 400) fail(__LINE__, "/config too long", "accepted", "4xx");
    expect_info(__LINE__, DEFAULT_URL, false);

    url[RW_RELAY_URL_MAXLEN - 1] = '\0'; /* longest storable */
    if (post_config_relay(url) != 200) fail(__LINE__, "/config max length", "non-200", "200");
    expect_info(__LINE__, url, true);
}

static const char *pair_relay(int line, char *out, size_t out_sz)
{
    uint8_t pub[32];
    for (int i = 0; i < 32; i++) pub[i] = (uint8_t)(i + 1);
    char b64[64];
    assert(rw_b64url_encode(pub, sizeof pub, b64, sizeof b64) == 43);
    char body[128];
    snprintf(body, sizeof body, "{\"pubkey\":\"%s\",\"name\":\"phone\"}", b64);

    reply_t r = request(HTTP_POST, "/pair", body);
    if (r.status != 200) fail(line, "/pair status", r.json ? str_field(&r, "err") : NULL, "200");
    const char *id = str_field(&r, "deviceId");
    if (!id || strcmp(id, rw_config_device_id()) != 0) fail(line, "/pair deviceId", id, rw_config_device_id());
    const char *url = str_field(&r, "relayUrl");
    if (!url) fail(line, "/pair relayUrl", NULL, "a string");
    snprintf(out, out_sz, "%s", url);
    cJSON_Delete(r.json);
    return out;
}

static void expect_pair_relay(int line, const char *stored, const char *want)
{
    fresh_device();
    if (stored && post_config_relay(stored) != 200) fail(line, "/config", stored, "200");
    char got[512];
    pair_relay(line, got, sizeof got);
    if (strcmp(got, want) != 0) fail(line, stored ? stored : "(default)", got, want);
}

static void test_pair_reply_is_http_base(void)
{
    /* The wss:// compile-time default becomes its https:// base. */
    expect_pair_relay(__LINE__, NULL, "https://relay.home.test");
    expect_pair_relay(__LINE__, "wss://r.test/", "https://r.test");
    expect_pair_relay(__LINE__, "ws://lan.test:8787", "http://lan.test:8787");
    expect_pair_relay(__LINE__, "https://r.test/prefix//", "https://r.test/prefix");
    expect_pair_relay(__LINE__, "http://10.0.0.2:8787/", "http://10.0.0.2:8787");
    expect_pair_relay(__LINE__, "r.test", "https://r.test");

    /* The longest storable bare host still arrives whole. */
    char host[RW_RELAY_URL_MAXLEN], want[RW_RELAY_URL_MAXLEN + 16];
    memset(host, 'h', sizeof host);
    host[sizeof host - 1] = '\0';
    snprintf(want, sizeof want, "https://%s", host);
    expect_pair_relay(__LINE__, host, want);

    /* The longest storable ws:// URL (two characters longer as http://). */
    char ws[RW_RELAY_URL_MAXLEN];
    memset(ws, 'w', sizeof ws);
    memcpy(ws, "ws://", 5);
    ws[sizeof ws - 1] = '\0';
    snprintf(want, sizeof want, "http://%s", ws + 5);
    expect_pair_relay(__LINE__, ws, want);
}

int main(void)
{
    start_setup_mode();
    test_info_without_override();
    test_config_relay_override_lifecycle();
    test_config_relay_length_limit();
    test_pair_reply_is_http_base();
    rw_prov_stop();
    puts("setup HTTP relay URL tests passed");
    return 0;
}
