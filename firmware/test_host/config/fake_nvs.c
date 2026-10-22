/* In-memory NVS for the config host tests. It follows the ESP-IDF contract
 * for the calls config.c makes: typed keys, nvs_get_str length handling,
 * NOT_FOUND for absent keys, read-only handles. Writes are visible at once
 * (as on target) but only committed state survives fake_nvs_reboot(). */
#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "nvs.h"

#define ESP_ERR_NVS_READ_ONLY 0x1107
#define MAX_ENTRIES 24
#define MAX_VALUE 4000

typedef enum { T_STR = 1, T_BLOB, T_U64 } entry_type_t;
typedef struct {
    bool used;
    char key[16];
    entry_type_t type;
    size_t len;
    unsigned char data[MAX_VALUE + 1];
} entry_t;

static entry_t s_live[MAX_ENTRIES], s_durable[MAX_ENTRIES];

enum { H_RO = 1, H_RW = 2 };

static entry_t *find(entry_t *store, const char *key)
{
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (store[i].used && strcmp(store[i].key, key) == 0) {
            return &store[i];
        }
    }
    return NULL;
}

static esp_err_t put(entry_t *store, const char *key, entry_type_t type,
                     const void *val, size_t len)
{
    assert(strlen(key) <= 15);
    assert(len <= MAX_VALUE);
    entry_t *e = find(store, key);
    for (int i = 0; !e && i < MAX_ENTRIES; i++) {
        if (!store[i].used) {
            e = &store[i];
        }
    }
    assert(e);
    e->used = true;
    strcpy(e->key, key);
    e->type = type;
    e->len = len;
    memcpy(e->data, val, len);
    return ESP_OK;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    assert(strcmp(ns, "rw_cfg") == 0);
    *out = mode == NVS_READWRITE ? H_RW : H_RO;
    return ESP_OK;
}

void nvs_close(nvs_handle_t h) { assert(h == H_RO || h == H_RW); }

esp_err_t nvs_commit(nvs_handle_t h)
{
    assert(h == H_RW);
    memcpy(s_durable, s_live, sizeof(s_live));
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    assert(h == H_RO || h == H_RW);
    entry_t *e = find(s_live, key);
    if (!e || e->type != T_STR) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (!out) {
        *len = e->len;
        return ESP_OK;
    }
    if (*len < e->len) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, e->data, e->len);
    *len = e->len;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *val)
{
    if (h != H_RW) {
        return ESP_ERR_NVS_READ_ONLY;
    }
    return put(s_live, key, T_STR, val, strlen(val) + 1);
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    assert(h == H_RO || h == H_RW);
    entry_t *e = find(s_live, key);
    if (!e || e->type != T_BLOB) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (!out) {
        *len = e->len;
        return ESP_OK;
    }
    if (*len < e->len) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, e->data, e->len);
    *len = e->len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len)
{
    if (h != H_RW) {
        return ESP_ERR_NVS_READ_ONLY;
    }
    return put(s_live, key, T_BLOB, val, len);
}

esp_err_t nvs_get_u64(nvs_handle_t h, const char *key, uint64_t *out)
{
    assert(h == H_RO || h == H_RW);
    entry_t *e = find(s_live, key);
    if (!e || e->type != T_U64) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    memcpy(out, e->data, sizeof(*out));
    return ESP_OK;
}

esp_err_t nvs_set_u64(nvs_handle_t h, const char *key, uint64_t val)
{
    if (h != H_RW) {
        return ESP_ERR_NVS_READ_ONLY;
    }
    return put(s_live, key, T_U64, &val, sizeof(val));
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    if (h != H_RW) {
        return ESP_ERR_NVS_READ_ONLY;
    }
    entry_t *e = find(s_live, key);
    if (!e) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    e->used = false;
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t h)
{
    if (h != H_RW) {
        return ESP_ERR_NVS_READ_ONLY;
    }
    memset(s_live, 0, sizeof(s_live));
    return ESP_OK;
}

void fake_nvs_wipe(void)
{
    memset(s_live, 0, sizeof(s_live));
    memset(s_durable, 0, sizeof(s_durable));
}

void fake_nvs_reboot(void)
{
    memcpy(s_live, s_durable, sizeof(s_live));
}

void fake_nvs_preload_str(const char *key, const char *val)
{
    put(s_live, key, T_STR, val, strlen(val) + 1);
    memcpy(s_durable, s_live, sizeof(s_live));
}

const char *fake_nvs_durable_str(const char *key)
{
    entry_t *e = find(s_durable, key);
    return e && e->type == T_STR ? (const char *)e->data : NULL;
}
