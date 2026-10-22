/* Relay URL storage and normalization, through the public rw_config_* API of
 * the production config.c over an in-memory NVS (fake_nvs.c).
 *
 * Built twice by run.sh:
 *   - CONFIG_RW_RELAY_URL is a real relay (the common deployment);
 *   - CONFIG_RW_RELAY_URL is the Kconfig placeholder wss://relay.example.com,
 *     with TEST_DEFAULT_IS_PLACEHOLDER=1 (only the default-URL case differs).
 * Expectations come from the override contract: NVS holds only an explicit
 * user override, the exact default clears it, the relay.example.com
 * placeholder host is never stored, and derived URLs map schemes as
 * wss/https -> wss + https, ws/http -> ws + http, bare host -> TLS. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "nvs.h"
#include "sdkconfig.h"

#define DEFAULT_URL CONFIG_RW_RELAY_URL

/* ------------------------------------------------------- boundary stubs -- */

const char *esp_err_to_name(esp_err_t err)
{
    static char buf[16];
    snprintf(buf, sizeof buf, "0x%x", (unsigned)err);
    return buf;
}
void esp_fill_random(void *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) ((unsigned char *)buf)[i] = (unsigned char)(0xa5 ^ i);
}
uint32_t esp_random(void) { return 0x12345678u; }
void esp_restart(void) { fprintf(stderr, "config code restarted the device\n"); abort(); }

/* -------------------------------------------------------------- helpers -- */

static void fail(int line, const char *what, const char *got, const char *want)
{
    fprintf(stderr, "test_config.c:%d: %s\n  got:  %s\n  want: %s\n", line, what,
            got ? got : "(null)", want ? want : "(null)");
    abort();
}
#define EXPECT_STR(what, got, want) \
    do { if (strcmp((got), (want)) != 0) fail(__LINE__, (what), (got), (want)); } while (0)
#define EXPECT(cond, what) \
    do { if (!(cond)) fail(__LINE__, (what), "false", #cond); } while (0)

/* Power cycle: uncommitted writes are lost, then the firmware re-inits. */
static void boot(void)
{
    fake_nvs_reboot();
    EXPECT(rw_config_init() == ESP_OK, "rw_config_init");
}

static void fresh_device(void)
{
    fake_nvs_wipe();
    boot();
}

static const char *effective(void)
{
    static char url[RW_RELAY_URL_MAXLEN];
    memset(url, 'Z', sizeof url);
    EXPECT(rw_config_get_relay_url(url, sizeof url) == ESP_OK, "get_relay_url");
    return url;
}

static void expect_no_override(int line)
{
    if (rw_config_relay_url_is_override()) fail(line, "is_override", "true", "false");
    if (strcmp(effective(), DEFAULT_URL) != 0) fail(line, "effective URL", effective(), DEFAULT_URL);
    /* Not merely hidden: nothing is persisted under the relay key. */
    if (fake_nvs_durable_str("relay")) fail(line, "durable relay key", fake_nvs_durable_str("relay"), "(absent)");
}

static void expect_override(int line, const char *url)
{
    if (!rw_config_relay_url_is_override()) fail(line, "is_override", "false", "true");
    if (strcmp(effective(), url) != 0) fail(line, "effective URL", effective(), url);
    const char *stored = fake_nvs_durable_str("relay");
    if (!stored || strcmp(stored, url) != 0) fail(line, "durable relay key", stored, url);
}

static const char *const PLACEHOLDERS[] = {
    "wss://relay.example.com",
    "https://relay.example.com",
    "ws://relay.example.com/",
    "http://Relay.Example.Com:8787/x",
    "https://RELAY.EXAMPLE.COM",
    "WSS://relay.example.com",
    "relay.example.com",
};
#define N_PLACEHOLDERS (sizeof PLACEHOLDERS / sizeof PLACEHOLDERS[0])

/* Hosts that merely resemble the placeholder are real relays. */
static const char *const LOOKALIKES[] = {
    "wss://relay.example.community",
    "wss://my-relay.example.com",
    "https://relay.example.com.test/r",
};
#define N_LOOKALIKES (sizeof LOOKALIKES / sizeof LOOKALIKES[0])

/* ------------------------------------------------- override persistence -- */

static void test_nothing_stored_uses_default(void)
{
    fresh_device();
    expect_no_override(__LINE__);
}

static void test_override_is_stored_durably(void)
{
    fresh_device();
    EXPECT(rw_config_set_relay_url("https://relay.home.lan") == ESP_OK, "set override");
    expect_override(__LINE__, "https://relay.home.lan");
    boot();
    expect_override(__LINE__, "https://relay.home.lan");
}

static void test_exact_default_clears_override(void)
{
    /* With no override: choosing the default must not freeze a copy of it. */
    fresh_device();
    EXPECT(rw_config_set_relay_url(DEFAULT_URL) == ESP_OK, "set default (no override)");
    expect_no_override(__LINE__);

    /* With an override: the default removes it, durably. */
    EXPECT(rw_config_set_relay_url("wss://other.relay.test") == ESP_OK, "set override");
    expect_override(__LINE__, "wss://other.relay.test");
    EXPECT(rw_config_set_relay_url(DEFAULT_URL) == ESP_OK, "set default (override)");
    expect_no_override(__LINE__);
    boot();
    expect_no_override(__LINE__);
}

static void test_placeholder_host_is_rejected(void)
{
    fresh_device();
    EXPECT(rw_config_set_relay_url("wss://mine.relay.test") == ESP_OK, "set override");
    for (size_t i = 0; i < N_PLACEHOLDERS; i++) {
        if (strcmp(PLACEHOLDERS[i], DEFAULT_URL) == 0) {
            continue; /* the exact default clears instead (tested above) */
        }
        if (rw_config_set_relay_url(PLACEHOLDERS[i]) == ESP_OK)
            fail(__LINE__, "placeholder accepted", PLACEHOLDERS[i], "an error");
        /* A rejected value neither replaces nor clears the user's override. */
        expect_override(__LINE__, "wss://mine.relay.test");
    }
}

static void test_lookalike_hosts_are_accepted(void)
{
    for (size_t i = 0; i < N_LOOKALIKES; i++) {
        fresh_device();
        if (rw_config_set_relay_url(LOOKALIKES[i]) != ESP_OK)
            fail(__LINE__, "real relay rejected", LOOKALIKES[i], "ESP_OK");
        expect_override(__LINE__, LOOKALIKES[i]);
    }
}

static void test_init_erases_stored_placeholder(void)
{
    for (size_t i = 0; i < N_PLACEHOLDERS; i++) {
        fake_nvs_wipe();
        fake_nvs_preload_str("relay", PLACEHOLDERS[i]); /* saved by an old setup page */
        boot();
        expect_no_override(__LINE__);
        boot(); /* the erase was committed, not just applied in RAM */
        expect_no_override(__LINE__);
    }
    /* A real override, including a lookalike host, survives init. */
    for (size_t i = 0; i < N_LOOKALIKES; i++) {
        fake_nvs_wipe();
        fake_nvs_preload_str("relay", LOOKALIKES[i]);
        boot();
        expect_override(__LINE__, LOOKALIKES[i]);
    }
}

static void test_length_limits(void)
{
    char max_ok[RW_RELAY_URL_MAXLEN];       /* RW_RELAY_URL_MAXLEN - 1 chars */
    char too_long[RW_RELAY_URL_MAXLEN + 1]; /* RW_RELAY_URL_MAXLEN chars */
    memset(max_ok, 'a', sizeof max_ok);
    memcpy(max_ok, "wss://", 6);
    max_ok[sizeof max_ok - 1] = '\0';
    memset(too_long, 'b', sizeof too_long);
    memcpy(too_long, "wss://", 6);
    too_long[sizeof too_long - 1] = '\0';

    fresh_device();
    EXPECT(rw_config_set_relay_url(max_ok) == ESP_OK, "longest URL accepted");
    expect_override(__LINE__, max_ok);

    EXPECT(rw_config_set_relay_url(too_long) != ESP_OK, "one char too long rejected");
    EXPECT(rw_config_set_relay_url("") != ESP_OK, "empty rejected");
    EXPECT(rw_config_set_relay_url(NULL) != ESP_OK, "NULL rejected");
    expect_override(__LINE__, max_ok);
}

/* --------------------------------------------------------- normalization -- */

typedef struct {
    const char *stored; /* NULL: no override, the compile-time default applies */
    const char *http;   /* pairing base */
    const char *ws;     /* device socket, before "/v1/device/<id>" */
} norm_case_t;

static void check_norm(const norm_case_t *c)
{
    fresh_device();
    if (c->stored) {
        EXPECT(rw_config_set_relay_url(c->stored) == ESP_OK, c->stored);
    }
    const char *id = rw_config_device_id();
    EXPECT(strlen(id) == RW_DEVICE_ID_HEXLEN, "device id length");

    char got[256], want[256];
    EXPECT(rw_config_relay_http_url(got, sizeof got) == ESP_OK, "http url");
    EXPECT_STR(c->stored ? c->stored : "(default)", got, c->http);

    snprintf(want, sizeof want, "%s/v1/device/%s", c->ws, id);
    EXPECT(rw_config_relay_ws_url(got, sizeof got) == ESP_OK, "ws url");
    EXPECT_STR(c->stored ? c->stored : "(default)", got, want);
}

static void test_scheme_normalization(void)
{
    static const norm_case_t cases[] = {
        {"wss://r.test",          "https://r.test",       "wss://r.test"},
        {"ws://r.test:8787",      "http://r.test:8787",   "ws://r.test:8787"},
        {"https://r.test",        "https://r.test",       "wss://r.test"},
        {"http://192.168.1.5:80", "http://192.168.1.5:80", "ws://192.168.1.5:80"},
        {"r.test",                "https://r.test",       "wss://r.test"},
        /* Trailing slashes never produce "//v1" or a slash-terminated base. */
        {"https://r.test/",       "https://r.test",       "wss://r.test"},
        {"ws://r.test//",         "http://r.test",        "ws://r.test"},
        {"wss://r.test/base/",    "https://r.test/base",  "wss://r.test/base"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        check_norm(&cases[i]);
    }
#ifdef TEST_DEFAULT_IS_PLACEHOLDER
    check_norm(&(norm_case_t){NULL, "https://relay.example.com", "wss://relay.example.com"});
#else
    check_norm(&(norm_case_t){NULL, "https://relay.home.test", "wss://relay.home.test"});
#endif
}

/* Results that do not fit are errors, never a shortened URL. */
static void test_derived_urls_never_truncate(void)
{
    fresh_device();
    EXPECT(rw_config_set_relay_url("ws://r.test") == ESP_OK, "set");
    const char *http = "http://r.test";
    char ws[96];
    snprintf(ws, sizeof ws, "ws://r.test/v1/device/%s", rw_config_device_id());

    char buf[128];
    EXPECT(rw_config_relay_http_url(buf, strlen(http) + 1) == ESP_OK, "http exact fit");
    EXPECT_STR("http exact fit", buf, http);
    EXPECT(rw_config_relay_http_url(buf, strlen(http)) != ESP_OK, "http one short");

    EXPECT(rw_config_relay_ws_url(buf, strlen(ws) + 1) == ESP_OK, "ws exact fit");
    EXPECT_STR("ws exact fit", buf, ws);
    EXPECT(rw_config_relay_ws_url(buf, strlen(ws)) != ESP_OK, "ws one short");

    /* The longest storable bare host gains "https://" without loss. */
    char host[RW_RELAY_URL_MAXLEN];
    memset(host, 'h', sizeof host);
    host[sizeof host - 1] = '\0';
    EXPECT(rw_config_set_relay_url(host) == ESP_OK, "longest bare host");
    char want[RW_RELAY_URL_MAXLEN + 16], got[RW_RELAY_URL_MAXLEN + 16];
    snprintf(want, sizeof want, "https://%s", host);
    EXPECT(rw_config_relay_http_url(got, sizeof got) == ESP_OK, "longest http");
    EXPECT_STR("longest http", got, want);
}

/* URI schemes are case-insensitive (RFC 3986); the placeholder check
 * already treats them so, and the mapping must agree. */
static void test_mixed_case_scheme(void)
{
    static const norm_case_t cases[] = {
        {"Https://r.test", "https://r.test", "wss://r.test"},
        {"WSS://r.test",   "https://r.test", "wss://r.test"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        check_norm(&cases[i]);
    }
}

int main(void)
{
    test_nothing_stored_uses_default();
    test_override_is_stored_durably();
    test_exact_default_clears_override();
    test_placeholder_host_is_rejected();
    test_lookalike_hosts_are_accepted();
    test_init_erases_stored_placeholder();
    test_length_limits();
    test_scheme_normalization();
    test_derived_urls_never_truncate();
    test_mixed_case_scheme();
    printf("config relay URL tests passed (default %s)\n", DEFAULT_URL);
    return 0;
}
