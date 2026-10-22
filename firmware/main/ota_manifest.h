/* Pure bounded manifest/identity validation, shared with host tests. */
#pragma once
#include "ota.h"
#define RW_OTA_MANIFEST_MAX 6144
#define RW_OTA_PAYLOAD_MAX 3072
#define RW_OTA_SIGNATURE_DOMAIN "remote-wake-firmware-v1\n"
typedef struct {
    rw_ota_image_identity_t identity;
    char url[RW_OTA_URL_MAX + 1];
    uint8_t sha256[32];
    uint32_t size, min_flash, min_psram, published, expires, key_generation;
} rw_ota_manifest_t;
bool rw_ota_identity_valid(const rw_ota_image_identity_t *identity);
bool rw_ota_identity_tuple_equal(const rw_ota_image_identity_t *a, const rw_ota_image_identity_t *b);
bool rw_ota_url_in_source(const char *url, const char *prefix);
bool rw_ota_https_host(const char *url, char *host, size_t capacity);
esp_err_t rw_ota_manifest_verify(const rw_ota_config_t *config, const char *json, size_t length,
    bool clock_valid, int64_t now, uint32_t flash_size, uint32_t psram_size,
    uint32_t release_floor, rw_ota_manifest_t *out);
