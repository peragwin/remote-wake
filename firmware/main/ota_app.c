/* Application-owned identity/trust. The reusable updater lives in remote-wake. */
#include "ota_app.h"
#include "ota.h"
#include "relay_client.h"
#include "sdkconfig.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include <string.h>

#if CONFIG_RW_OTA_ENABLED
RW_OTA_DEFINE_IMAGE_IDENTITY("remote-wake", CONFIG_APP_OTA_BOARD, CONFIG_APP_OTA_PROFILE,
    CONFIG_APP_OTA_CHANNEL, "rw-4m-ab-v1", APP_FIRMWARE_VERSION,
    CONFIG_IDF_TARGET, CONFIG_APP_OTA_RELEASE);

static rw_ota_trusted_key_t publisher;
static const char *const redirect_hosts[] = {
    "release-assets.githubusercontent.com", "objects.githubusercontent.com",
    "github.com", "raw.githubusercontent.com"
};
static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool clock_valid(void *ctx) {
    (void)ctx;
    return rw_relay_clock_valid();
}
#endif

esp_err_t app_ota_start(void) {
#if CONFIG_RW_OTA_ENABLED
    const char *hex = CONFIG_APP_OTA_PUBLIC_KEY_HEX;
    bool configured = hex[0] && CONFIG_APP_OTA_MANIFEST_URL[0] && CONFIG_APP_OTA_SOURCE_PREFIX[0];
    if ((hex[0] || CONFIG_APP_OTA_MANIFEST_URL[0] || CONFIG_APP_OTA_SOURCE_PREFIX[0]) && !configured)
        return ESP_ERR_INVALID_ARG;
    if (strcmp(esp_app_get_description()->version, APP_FIRMWARE_VERSION)) return ESP_ERR_INVALID_STATE;
    if (configured) {
        if (strlen(hex) != 64 || strlen(CONFIG_APP_OTA_KEY_ID) >= sizeof(publisher.kid))
            return ESP_ERR_INVALID_ARG;
        for (unsigned i = 0; i < 32; ++i) {
            int a = hex_value(hex[2*i]), b = hex_value(hex[2*i+1]);
            if (a < 0 || b < 0) return ESP_ERR_INVALID_ARG;
            publisher.public_key[i] = (a << 4) | b;
        }
        strcpy(publisher.kid, CONFIG_APP_OTA_KEY_ID);
        publisher.generation = CONFIG_APP_OTA_KEY_GENERATION;
    }
    static rw_ota_config_t config;
    config = (rw_ota_config_t){
        .image_identity = &rw_ota_image_identity,
        .trusted_keys = configured ? &publisher : NULL,
        .trusted_key_count = configured ? 1 : 0,
        .manifest_url = CONFIG_APP_OTA_MANIFEST_URL,
        .source_prefix = CONFIG_APP_OTA_SOURCE_PREFIX,
        .redirect_hosts = redirect_hosts,
        .redirect_host_count = sizeof(redirect_hosts) / sizeof(redirect_hosts[0]),
#ifdef CONFIG_APP_OTA_AUTO
        .policy = RW_OTA_AUTO,
#else
        .policy = RW_OTA_NOTIFY,
#endif
        .clock_valid = clock_valid,
        .prepare_restart = app_ota_prepare_restart,
        .progress = app_ota_progress,
        .health_timeout_sec = 60,
    };
    esp_err_t err = rw_ota_init(&config);
    if (err == ESP_OK) ESP_LOGI("app.ota", "%s", configured ? "publisher configured" : "boot health only; no publisher configured");
    return err;
#else
    return ESP_OK;
#endif
}
