/* remote-wake — ESP-IDF firmware entry point.
 *
 * Boot order is deliberate:
 *   1. PWR_SW GPIO to its safe idle level, before anything can crash.
 *   2. Status LED (so a boot loop is visible).
 *   3. NVS + config (deviceId / deviceToken on first boot).
 *   4. libsodium.
 *   5. BOOT-button watcher (5 s -> setup mode, 15 s -> factory reset).
 *   6. Either setup mode (SoftAP + /pair) or normal mode (STA + relay).
 */
#include <string.h>

#include "auth.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led.h"
#include "nvs_flash.h"
#include "power_btn.h"
#include "provisioning.h"
#include "relay_client.h"
#include "sdkconfig.h"
#include "usb_hid.h"

static const char *TAG = "rw.main";

#define BOOT_GPIO ((gpio_num_t)CONFIG_RW_BOOT_GPIO)
#define SETUP_MAGIC 0x52575355u /* "RWSU" */

/* Survives esp_restart() but not a power cycle — exactly the semantics we
 * want for "reboot into setup mode". */
static RTC_NOINIT_ATTR uint32_t s_boot_to_setup;

static bool s_setup_mode;
static int  s_wifi_retries;
static esp_timer_handle_t s_reconnect_timer;

/* ------------------------------------------------------------- wifi sta -- */

static void reconnect_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        rw_relay_notify_network(false);
        rw_led_set(RW_LED_FAULT);
        /* Retry off the event-loop task: blocking here would stall every
         * other event, including the GOT_IP we are waiting for. */
        int delay_ms = s_wifi_retries < 5 ? 1000 : 10000;
        s_wifi_retries++;
        ESP_LOGW(TAG, "wifi disconnected, retry %d in %d ms", s_wifi_retries,
                 delay_ms);
        if (s_reconnect_timer) {
            esp_timer_stop(s_reconnect_timer);
            esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000);
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
        s_wifi_retries = 0;
        rw_relay_notify_network(true);
        return;
    }
}

static esp_err_t start_station(void)
{
    char ssid[RW_SSID_MAXLEN] = {0};
    char pass[RW_PASS_MAXLEN] = {0};
    if (rw_config_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK ||
        ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    if (!sta) {
        return ESP_FAIL;
    }

    const esp_timer_create_args_t targs = {
        .callback = reconnect_cb,
        .name     = "rw_wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_reconnect_timer));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    /* SSIDs may legitimately fill all 32 bytes with no NUL, so copy by
     * length rather than as a C string. */
    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, ssid, strnlen(ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, pass, strnlen(pass, sizeof(wc.sta.password)));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable    = true;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    /* The relay link matters more than a few mA of idle current. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    return ESP_OK;
}

/* ----------------------------------------------------------- boot button -- */

static void reboot_into_setup(void)
{
    s_boot_to_setup = SETUP_MAGIC;
    ESP_LOGW(TAG, "entering setup mode — rebooting");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void button_task(void *arg)
{
    (void)arg;
    const TickType_t step = pdMS_TO_TICKS(50);
    uint32_t held_ms = 0;
    bool armed_setup = false;

    while (true) {
        if (gpio_get_level(BOOT_GPIO) == 0) {   /* active low */
            held_ms += 50;

            if (!armed_setup && held_ms >= CONFIG_RW_SETUP_HOLD_MS) {
                armed_setup = true;
                ESP_LOGW(TAG, "hold detected — release now for setup mode, "
                              "keep holding %d s for factory reset",
                         (CONFIG_RW_FACTORY_HOLD_MS - CONFIG_RW_SETUP_HOLD_MS) /
                             1000);
                rw_led_set(RW_LED_SETUP);
            }
            if (held_ms >= CONFIG_RW_FACTORY_HOLD_MS) {
                rw_led_set(RW_LED_LOCKOUT);
                ESP_LOGE(TAG, "FACTORY RESET");
                rw_config_factory_reset();
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        } else {
            if (armed_setup && !s_setup_mode) {
                reboot_into_setup();
            }
            held_ms = 0;
            armed_setup = false;
        }
        vTaskDelay(step);
    }
}

static void start_button_watcher(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_RW_BOOT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    xTaskCreate(button_task, "rw_btn", 3072, NULL, 4, NULL);
}

/* ------------------------------------------------------------- app_main -- */

void app_main(void)
{
    /* 1. The power pin must be defined before anything else can fault. */
    ESP_ERROR_CHECK(rw_power_btn_init());

    /* 2. Status LED — best effort, never fatal. */
    rw_led_init();
    rw_led_set(RW_LED_BOOT);

    ESP_LOGI(TAG, "remote-wake %s starting (reset reason %d)", RW_FW_VERSION,
             (int)esp_reset_reason());
    /* relay_client.h owns RW_FW_VERSION; keep it visible in the boot banner. */

    /* 3. NVS + settings. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(rw_config_init());

    /* 4. Crypto. */
    ESP_ERROR_CHECK(rw_auth_init());

    /* Decide the mode before touching the radio. The RTC variable is only
     * meaningful after a software restart — after a cold boot it holds
     * whatever was in RTC RAM. */
    esp_reset_reason_t rr = esp_reset_reason();
    bool requested = (rr == ESP_RST_SW) && (s_boot_to_setup == SETUP_MAGIC);
    s_boot_to_setup = 0;
    s_setup_mode = requested || !rw_config_is_provisioned();

    /* 5. Button watcher (works in both modes). */
    start_button_watcher();

    /* 6. Networking. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    if (s_setup_mode) {
        ESP_LOGW(TAG, "SETUP MODE (%s)",
                 requested ? "button" : "not provisioned");
        rw_led_set(RW_LED_SETUP);
        ESP_ERROR_CHECK(rw_prov_start());
        /* The HTTP server owns the device from here; nothing else runs. In
         * particular the USB HID interface stays down and the relay client is
         * never started, so no command path exists in setup mode. */
        return;
    }

    /* 7. Normal mode. */
    err = rw_hid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB HID init failed: %s — keyboard actions will report "
                      "usb_down", esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(rw_relay_start());

    err = start_station();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no Wi-Fi credentials — hold BOOT for 5 s to re-enter "
                      "setup mode");
        rw_led_set(RW_LED_FAULT);
        return;
    }

    ESP_LOGI(TAG, "deviceId %s, %d operator key(s) registered",
             rw_config_device_id(), rw_config_key_count());
}
