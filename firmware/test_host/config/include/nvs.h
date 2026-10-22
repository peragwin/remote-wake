#pragma once
/* In-memory NVS with ESP-IDF semantics for the calls config.c makes. Writes
 * are visible at once but survive fake_nvs_reboot() only once committed. */
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out);
void nvs_close(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len);
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *val);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len);
esp_err_t nvs_get_u64(nvs_handle_t h, const char *key, uint64_t *out);
esp_err_t nvs_set_u64(nvs_handle_t h, const char *key, uint64_t val);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
esp_err_t nvs_erase_all(nvs_handle_t h);

/* Test control. */
void fake_nvs_wipe(void);                       /* blank flash */
void fake_nvs_reboot(void);                     /* drop uncommitted writes */
void fake_nvs_preload_str(const char *key, const char *val); /* committed */
/* Committed (durable) string, or NULL when the key is absent. */
const char *fake_nvs_durable_str(const char *key);
