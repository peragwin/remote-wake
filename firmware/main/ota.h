/* Optional whole-application OTA; applications own identity, trust and health. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
#define RW_OTA_API_VERSION 1
#define RW_OTA_URL_MAX 511
#define RW_OTA_MAX_KEYS 4
#define RW_OTA_IDENTITY_MAGIC "RWOTA-IDENTITY1!"
#define RW_OTA_IDENTITY_MAGIC_BYTES {'R','W','O','T','A','-','I','D','E','N','T','I','T','Y','1','!'}

typedef struct {
    char magic[16];
    uint32_t format_version, updater_api, release;
    char product[32], board[32], profile[48], channel[16], layout[32], version[32], chip[16];
} rw_ota_image_identity_t;
#define RW_OTA_DEFINE_IMAGE_IDENTITY(prod, brd, prof, chan, lay, ver, target, counter) \
    const rw_ota_image_identity_t rw_ota_image_identity \
    __attribute__((used, section(".rodata.rw_ota_identity"))) = { \
        .magic = RW_OTA_IDENTITY_MAGIC_BYTES, .format_version = 1, .updater_api = RW_OTA_API_VERSION, \
        .release = (counter), .product = (prod), .board = (brd), .profile = (prof), \
        .channel = (chan), .layout = (lay), .version = (ver), .chip = (target) }

typedef struct {
    char kid[16];
    uint32_t generation;
    uint8_t public_key[32];
} rw_ota_trusted_key_t;
typedef enum { RW_OTA_NOTIFY, RW_OTA_AUTO, RW_OTA_PINNED } rw_ota_policy_t;
typedef enum {
    RW_OTA_DISABLED, RW_OTA_IDLE, RW_OTA_CHECKING, RW_OTA_AVAILABLE,
    RW_OTA_DOWNLOADING, RW_OTA_VERIFYING, RW_OTA_WAITING_RESTART,
    RW_OTA_RESTARTING, RW_OTA_PENDING_HEALTH, RW_OTA_HEALTHY,
    RW_OTA_ROLLED_BACK, RW_OTA_FAILED,
} rw_ota_phase_t;
typedef struct {
    rw_ota_phase_t phase;
    uint32_t release, downloaded, total;
    esp_err_t error;
    char version[32];
} rw_ota_status_t;
typedef struct {
    char product[32], board[32], profile[48], channel[16], partition_layout[32], version[32];
    uint32_t release_counter, updater_api;
} rw_ota_registration_t;
typedef struct {
    /* All pointers and callback context must remain valid for the application lifetime. */
    const rw_ota_image_identity_t *image_identity;
    const rw_ota_trusted_key_t *trusted_keys;
    size_t trusted_key_count;
    const char *manifest_url; /* HTTPS; <=511 bytes, within source_prefix. */
    const char *source_prefix; /* HTTPS repo/path prefix ending '/', <=383 bytes. */
    const char *const *redirect_hosts; /* Exact HTTPS hosts; max 4. No wildcard. */
    size_t redirect_host_count;
    rw_ota_policy_t policy;
    uint32_t check_interval_sec; /* 0: 6 h; minimum 300 s, plus random jitter. */
    uint32_t health_timeout_sec; /* 0: 60 s. Pending verification only. */
    void *ctx;
    bool (*clock_valid)(void *ctx); /* Required; no manifest accepted before trusted sync. */
    /* Called only by updater worker. Return INVALID_STATE while application is busy.
     * ESP_OK promises app-specific restart preparation is complete. Never render or
     * do network work here; quiesce app USB users before successful return. */
    esp_err_t (*prepare_restart)(void *ctx);
    void (*progress)(void *ctx, const rw_ota_status_t *status); /* worker task only */
} rw_ota_config_t;

/* Disabled builds return NOT_SUPPORTED; runtime opt-in requires identity, keys,
 * HTTPS source, A/B slots + otadata, and bootloader rollback. No implicit trust.
 * With valid identity and zero trusted keys, starts boot-health-only mode. */
esp_err_t rw_ota_init(const rw_ota_config_t *config);
bool rw_ota_get_registration(rw_ota_registration_t *out);
esp_err_t rw_ota_offer_manifest(const char *url); /* bounded copy, no network/callbacks */
void rw_ota_get_status(rw_ota_status_t *out);
/* Call after required local application health checks; Internet/PC presence is
 * not health. Confirms rollback, then persists the floor; current embedded release
 * also enforces the floor across a power failure between those operations.
 * Safe successful no-op on factory/no-pending builds, including disabled OTA. */
esp_err_t rw_ota_confirm_healthy(void);
/* Explicit local health failure; records candidate suppression before rollback. */
esp_err_t rw_ota_reject_unhealthy(void);
const char *rw_ota_phase_name(rw_ota_phase_t phase);
#ifdef __cplusplus
}
#endif
