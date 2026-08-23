/* config — NVS-backed device settings and operator key slots.
 *
 * Namespace "rw_cfg" holds:
 *   dev_id    str   16 hex chars  (8 random bytes, first boot)
 *   dev_tok   str   43 chars      (32 random bytes, base64url unpadded)
 *   ssid      str
 *   pass      str
 *   relay     str   base URL, e.g. "wss://relay.example.com"
 *   k1..k4    blob  rw_keyslot_t
 *   c1..c4    u64   per-slot counter high-water mark
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RW_DEVICE_ID_HEXLEN 16
#define RW_DEVICE_TOKEN_LEN 43   /* base64url(32 bytes), unpadded */
#define RW_MAX_KEY_SLOTS    4
#define RW_KEY_NAME_MAXLEN  32
#define RW_SSID_MAXLEN      33
#define RW_PASS_MAXLEN      64
#define RW_RELAY_URL_MAXLEN 128

typedef struct {
    uint8_t pubkey[32];
    char    name[RW_KEY_NAME_MAXLEN];
    int64_t added_at;         /**< Unix seconds, 0 if the clock was unset */
} rw_keyslot_t;

/**
 * Open NVS, generating deviceId/deviceToken on first boot.
 * Must be called after nvs_flash_init().
 */
esp_err_t rw_config_init(void);

/** True when the device has Wi-Fi credentials, a relay URL and >= 1 key. */
bool rw_config_is_provisioned(void);

const char *rw_config_device_id(void);     /**< 16 hex chars, never NULL */
const char *rw_config_device_token(void);  /**< 43 chars, never NULL */

esp_err_t rw_config_get_wifi(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz);
esp_err_t rw_config_set_wifi(const char *ssid, const char *pass);

/** Configured base URL, falling back to CONFIG_RW_RELAY_URL. */
esp_err_t rw_config_get_relay_url(char *out, size_t out_sz);
esp_err_t rw_config_set_relay_url(const char *url);

/** Build the full device websocket endpoint: <base>/v1/device/<deviceId>. */
esp_err_t rw_config_relay_ws_url(char *out, size_t out_sz);

/** kid ("p1".."p4") -> slot index 0..3, or -1. */
int rw_config_kid_index(const char *kid);

/** Fetch the pubkey registered in @p kid. */
bool rw_config_get_key(const char *kid, uint8_t out[32]);

/** Number of occupied key slots. */
int rw_config_key_count(void);

/** Read slot @p idx (0-based). Returns ESP_ERR_NVS_NOT_FOUND when empty. */
esp_err_t rw_config_get_slot(int idx, rw_keyslot_t *out);

/**
 * Register @p pubkey in the next free slot.
 * Re-registering an identical key is idempotent and returns that slot.
 * @param[out] kid_out  receives "pN" (needs >= 4 bytes)
 * @return ESP_ERR_NO_MEM when all slots are taken.
 */
esp_err_t rw_config_add_key(const uint8_t pubkey[32], const char *name,
                            char kid_out[4]);

/** Forget slot @p idx and reset its counter. */
esp_err_t rw_config_remove_slot(int idx);

bool      rw_config_get_ctr(const char *kid, uint64_t *out);
esp_err_t rw_config_set_ctr(const char *kid, uint64_t ctr);

/** Erase the whole "rw_cfg" namespace (deviceId/token are regenerated). */
esp_err_t rw_config_factory_reset(void);

/** WPA2 passphrase for setup-mode SoftAP, derived from the device token. */
void rw_config_softap_password(char *out, size_t out_sz);
/** SoftAP SSID: "remote-wake-<first 4 of deviceId>". */
void rw_config_softap_ssid(char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif
