#include "config.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "b64url.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "rw.cfg";

#define NVS_NS "rw_cfg"

#define K_DEV_ID  "dev_id"
#define K_DEV_TOK "dev_tok"
#define K_SSID    "ssid"
#define K_PASS    "pass"
#define K_RELAY   "relay"

/* Cached so that hot paths (auth, relay URL building) never touch NVS. */
static char s_device_id[RW_DEVICE_ID_HEXLEN + 1];
static char s_device_token[RW_DEVICE_TOKEN_LEN + 1];
static bool s_ready;

/* ------------------------------------------------------------- nvs util -- */

static esp_err_t nvs_open_rw(nvs_handle_t *h, nvs_open_mode_t mode)
{
    return nvs_open(NVS_NS, mode, h);
}

static esp_err_t get_str(const char *key, char *out, size_t out_sz)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_rw(&h, NVS_READONLY);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = out_sz;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err;
}

static esp_err_t set_str(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_rw(&h, NVS_READWRITE);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* Slot indices are 0..3; the % 4 keeps gcc's value-range analysis (and thus
 * -Werror=format-truncation) certain the output fits "x1".."x4" + NUL. */
static void slot_key(int idx, char out[4])
{
    snprintf(out, 4, "k%u", (unsigned)idx % 4u + 1u);
}

static void ctr_key(int idx, char out[4])
{
    snprintf(out, 4, "c%u", (unsigned)idx % 4u + 1u);
}

/* ---------------------------------------------------------------- init -- */

static esp_err_t ensure_identity(void)
{
    esp_err_t err = get_str(K_DEV_ID, s_device_id, sizeof(s_device_id));
    if (err != ESP_OK || strlen(s_device_id) != RW_DEVICE_ID_HEXLEN) {
        uint8_t raw[8];
        esp_fill_random(raw, sizeof(raw));
        for (int i = 0; i < 8; i++) {
            snprintf(&s_device_id[i * 2], 3, "%02x", raw[i]);
        }
        ESP_RETURN_ON_ERROR(set_str(K_DEV_ID, s_device_id), TAG,
                            "persist deviceId");
        ESP_LOGW(TAG, "generated deviceId %s", s_device_id);
    }

    err = get_str(K_DEV_TOK, s_device_token, sizeof(s_device_token));
    if (err != ESP_OK || strlen(s_device_token) != RW_DEVICE_TOKEN_LEN) {
        uint8_t raw[32];
        esp_fill_random(raw, sizeof(raw));
        if (rw_b64url_encode(raw, sizeof(raw), s_device_token,
                             sizeof(s_device_token)) != RW_DEVICE_TOKEN_LEN) {
            return ESP_FAIL;
        }
        ESP_RETURN_ON_ERROR(set_str(K_DEV_TOK, s_device_token), TAG,
                            "persist deviceToken");
        ESP_LOGW(TAG, "generated a fresh deviceToken");
    }
    return ESP_OK;
}

esp_err_t rw_config_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_rw(&h, NVS_READWRITE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s): %s", NVS_NS, esp_err_to_name(err));
        return err;
    }
    nvs_close(h);

    err = ensure_identity();
    if (err != ESP_OK) {
        return err;
    }
    s_ready = true;
    ESP_LOGI(TAG, "deviceId=%s keys=%d provisioned=%d", s_device_id,
             rw_config_key_count(), (int)rw_config_is_provisioned());
    return ESP_OK;
}

const char *rw_config_device_id(void)
{
    return s_ready ? s_device_id : "";
}

const char *rw_config_device_token(void)
{
    return s_ready ? s_device_token : "";
}

/* ---------------------------------------------------------------- wifi -- */

esp_err_t rw_config_get_wifi(char *ssid, size_t ssid_sz, char *pass,
                             size_t pass_sz)
{
    if (ssid) {
        ssid[0] = '\0';
    }
    if (pass) {
        pass[0] = '\0';
    }
    esp_err_t err = get_str(K_SSID, ssid, ssid_sz);
    if (err != ESP_OK) {
        return err;
    }
    /* An open network legitimately has no passphrase. */
    if (get_str(K_PASS, pass, pass_sz) != ESP_OK && pass) {
        pass[0] = '\0';
    }
    return ESP_OK;
}

esp_err_t rw_config_set_wifi(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0' || strlen(ssid) >= RW_SSID_MAXLEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pass && strlen(pass) >= RW_PASS_MAXLEN) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = set_str(K_SSID, ssid);
    if (err != ESP_OK) {
        return err;
    }
    return set_str(K_PASS, pass ? pass : "");
}

/* --------------------------------------------------------------- relay -- */

esp_err_t rw_config_get_relay_url(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';  /* nvs_get_str leaves the buffer untouched on failure */
    if (get_str(K_RELAY, out, out_sz) == ESP_OK && out[0] != '\0') {
        return ESP_OK;
    }
    if (strlen(CONFIG_RW_RELAY_URL) >= out_sz) {
        return ESP_ERR_INVALID_SIZE;
    }
    strcpy(out, CONFIG_RW_RELAY_URL);
    return ESP_OK;
}

esp_err_t rw_config_set_relay_url(const char *url)
{
    if (!url || url[0] == '\0' || strlen(url) >= RW_RELAY_URL_MAXLEN) {
        return ESP_ERR_INVALID_ARG;
    }
    return set_str(K_RELAY, url);
}

esp_err_t rw_config_relay_ws_url(char *out, size_t out_sz)
{
    char base[RW_RELAY_URL_MAXLEN];
    esp_err_t err = rw_config_get_relay_url(base, sizeof(base));
    if (err != ESP_OK) {
        return err;
    }

    /* Strip a trailing slash so we never emit "//v1/device/...". */
    size_t bl = strlen(base);
    while (bl > 0 && base[bl - 1] == '/') {
        base[--bl] = '\0';
    }
    if (bl == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Accept https:// / http:// / bare host and normalize to ws(s)://. */
    const char *host = base;
    const char *scheme = "wss://";
    if (strncmp(base, "wss://", 6) == 0) {
        host = base + 6;
    } else if (strncmp(base, "ws://", 5) == 0) {
        host = base + 5;
        scheme = "ws://";
    } else if (strncmp(base, "https://", 8) == 0) {
        host = base + 8;
    } else if (strncmp(base, "http://", 7) == 0) {
        host = base + 7;
        scheme = "ws://";
    }

    int n = snprintf(out, out_sz, "%s%s/v1/device/%s", scheme, host,
                     rw_config_device_id());
    if (n < 0 || (size_t)n >= out_sz) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* ----------------------------------------------------------- key slots -- */

int rw_config_kid_index(const char *kid)
{
    if (!kid || kid[0] != 'p' || kid[1] < '1' ||
        kid[1] > ('0' + RW_MAX_KEY_SLOTS) || kid[2] != '\0') {
        return -1;
    }
    return kid[1] - '1';
}

esp_err_t rw_config_get_slot(int idx, rw_keyslot_t *out)
{
    if (idx < 0 || idx >= RW_MAX_KEY_SLOTS || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open_rw(&h, NVS_READONLY);
    if (err != ESP_OK) {
        return err;
    }
    char key[4];
    slot_key(idx, key);
    size_t sz = sizeof(*out);
    err = nvs_get_blob(h, key, out, &sz);
    nvs_close(h);
    if (err == ESP_OK && sz != sizeof(*out)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

bool rw_config_get_key(const char *kid, uint8_t out[32])
{
    int idx = rw_config_kid_index(kid);
    if (idx < 0) {
        return false;
    }
    rw_keyslot_t slot;
    if (rw_config_get_slot(idx, &slot) != ESP_OK) {
        return false;
    }
    memcpy(out, slot.pubkey, 32);
    return true;
}

int rw_config_key_count(void)
{
    int n = 0;
    rw_keyslot_t slot;
    for (int i = 0; i < RW_MAX_KEY_SLOTS; i++) {
        if (rw_config_get_slot(i, &slot) == ESP_OK) {
            n++;
        }
    }
    return n;
}

esp_err_t rw_config_add_key(const uint8_t pubkey[32], const char *name,
                            char kid_out[4])
{
    if (!pubkey || !kid_out) {
        return ESP_ERR_INVALID_ARG;
    }
    /* An all-zero key is the canonical "not a key" value; refuse it. */
    uint8_t zero[32] = {0};
    if (memcmp(pubkey, zero, 32) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    rw_keyslot_t slot;
    int free_idx = -1;
    for (int i = 0; i < RW_MAX_KEY_SLOTS; i++) {
        esp_err_t e = rw_config_get_slot(i, &slot);
        if (e == ESP_OK) {
            if (memcmp(slot.pubkey, pubkey, 32) == 0) {
                snprintf(kid_out, 4, "p%d", i + 1);
                ESP_LOGI(TAG, "key already registered in %s", kid_out);
                return ESP_OK;
            }
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }
    if (free_idx < 0) {
        return ESP_ERR_NO_MEM;
    }

    memset(&slot, 0, sizeof(slot));
    memcpy(slot.pubkey, pubkey, 32);
    if (name && name[0]) {
        strlcpy(slot.name, name, sizeof(slot.name));
    } else {
        strlcpy(slot.name, "operator", sizeof(slot.name));
    }
    slot.added_at = (int64_t)time(NULL);

    nvs_handle_t h;
    esp_err_t err = nvs_open_rw(&h, NVS_READWRITE);
    if (err != ESP_OK) {
        return err;
    }
    char key[4], ck[4];
    slot_key(free_idx, key);
    ctr_key(free_idx, ck);
    err = nvs_set_blob(h, key, &slot, sizeof(slot));
    if (err == ESP_OK) {
        /* A brand-new key starts from counter 0 so its first command (ctr=1)
         * is accepted even if the slot was previously used. */
        err = nvs_set_u64(h, ck, 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        snprintf(kid_out, 4, "p%u", (unsigned)free_idx % 4u + 1u);
        ESP_LOGI(TAG, "registered operator key in slot %s (%s)", kid_out,
                 slot.name);
    }
    return err;
}

esp_err_t rw_config_remove_slot(int idx)
{
    if (idx < 0 || idx >= RW_MAX_KEY_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open_rw(&h, NVS_READWRITE);
    if (err != ESP_OK) {
        return err;
    }
    char key[4], ck[4];
    slot_key(idx, key);
    ctr_key(idx, ck);
    esp_err_t e1 = nvs_erase_key(h, key);
    esp_err_t e2 = nvs_erase_key(h, ck);
    err = nvs_commit(h);
    nvs_close(h);
    if (e1 != ESP_OK && e1 != ESP_ERR_NVS_NOT_FOUND) {
        return e1;
    }
    if (e2 != ESP_OK && e2 != ESP_ERR_NVS_NOT_FOUND) {
        return e2;
    }
    return err;
}

/* ------------------------------------------------------------ counters -- */

bool rw_config_get_ctr(const char *kid, uint64_t *out)
{
    int idx = rw_config_kid_index(kid);
    if (idx < 0 || !out) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open_rw(&h, NVS_READONLY) != ESP_OK) {
        return false;
    }
    char key[4];
    ctr_key(idx, key);
    uint64_t v = 0;
    esp_err_t err = nvs_get_u64(h, key, &v);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out = 0;
        return true;
    }
    if (err != ESP_OK) {
        return false;
    }
    *out = v;
    return true;
}

esp_err_t rw_config_set_ctr(const char *kid, uint64_t ctr)
{
    int idx = rw_config_kid_index(kid);
    if (idx < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open_rw(&h, NVS_READWRITE);
    if (err != ESP_OK) {
        return err;
    }
    char key[4];
    ctr_key(idx, key);
    err = nvs_set_u64(h, key, ctr);
    if (err == ESP_OK) {
        /* nvs_commit() is what makes this durable — the protocol requires the
         * counter to hit flash before the action runs. */
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* -------------------------------------------------------------- status -- */

bool rw_config_is_provisioned(void)
{
    char ssid[RW_SSID_MAXLEN] = {0};
    char pass[RW_PASS_MAXLEN] = {0};
    if (rw_config_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK ||
        ssid[0] == '\0') {
        return false;
    }
    char url[RW_RELAY_URL_MAXLEN] = {0};
    if (rw_config_get_relay_url(url, sizeof(url)) != ESP_OK || url[0] == '\0') {
        return false;
    }
    return rw_config_key_count() > 0;
}

esp_err_t rw_config_factory_reset(void)
{
    ESP_LOGW(TAG, "FACTORY RESET — erasing namespace %s", NVS_NS);
    nvs_handle_t h;
    esp_err_t err = nvs_open_rw(&h, NVS_READWRITE);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    s_ready = false;
    memset(s_device_id, 0, sizeof(s_device_id));
    memset(s_device_token, 0, sizeof(s_device_token));
    return err;
}

/* -------------------------------------------------------------- softap -- */

void rw_config_softap_ssid(char *out, size_t out_sz)
{
    snprintf(out, out_sz, "remote-wake-%.4s", rw_config_device_id());
}

void rw_config_softap_password(char *out, size_t out_sz)
{
    /* Derived from the token rather than being a prefix of it, so that anyone
     * who is told the AP password does not thereby learn relay credentials. */
    if (!out || out_sz == 0) {
        return;
    }
    const char *tok = rw_config_device_token();
    uint8_t digest[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const uint8_t *)"remote-wake-softap-v1", 21);
    mbedtls_sha256_update(&ctx, (const uint8_t *)tok, strlen(tok));
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    char b64[RW_B64URL_ENC_LEN(32) + 1];
    rw_b64url_encode(digest, sizeof(digest), b64, sizeof(b64));

    /* 12 chars of base64url ~= 72 bits; well inside the WPA2 8..63 range. */
    size_t n = 12;
    if (out_sz < n + 1) {
        n = out_sz - 1;
    }
    memcpy(out, b64, n);
    out[n] = '\0';
}
