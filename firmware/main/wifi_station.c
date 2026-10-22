#include <stdatomic.h>
#include <string.h>

#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led.h"
#include "relay_client.h"
#include "wifi_recovery.h"
#include "wifi_station.h"

static const char *TAG = "rw.wifi";

static atomic_uint s_outage_generation;

/* Fence the old relay session immediately, even when disconnection and DHCP
 * recovery happen between station-task samples. Event callbacks only publish
 * state; driver calls and delays remain exclusively in the station task. */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *data)
{
    (void)data;
    if ((base == WIFI_EVENT &&
         (id == WIFI_EVENT_STA_DISCONNECTED || id == WIFI_EVENT_STA_STOP)) ||
        (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)) {
        rw_relay_notify_network(false);
        /* Publish after the fence. An acquire snapshot taken before the IP
         * sample cannot consume this outage before its false publication. */
        atomic_fetch_add_explicit(&s_outage_generation, 1, memory_order_release);
    }
    xTaskNotifyGive((TaskHandle_t)arg);
}

static uint64_t now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000;
}

static bool station_has_ip(esp_netif_t *sta, uint32_t *address)
{
    wifi_ap_record_t ap;
    esp_netif_ip_info_t ip;
    /* get_ip_info alone may return a cached address after disassociation. Both
     * the driver association and the live netif must agree that it is usable. */
    bool up = esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
              esp_netif_is_netif_up(sta) &&
              esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0;
    *address = up ? ip.ip.addr : 0;
    return up;
}

static void station_task(void *arg)
{
    esp_netif_t *sta = arg;
    /* The creator publishes our handle before allowing driver startup. */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    /* A blocked driver/netif API must also recover: policy deadlines cannot
     * run while this task is stuck inside such a call. */
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    rw_wifi_recovery_t recovery;
    rw_wifi_recovery_init(&recovery, now_ms());
    bool driver_started = false;
    bool network_up = false;
    uint32_t last_address = 0;
    unsigned last_outage = atomic_load_explicit(&s_outage_generation, memory_order_acquire);

    for (;;) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        /* Read the generation before sampling driver/netif state. If an
         * event races this sample, its newer generation forces another pass. */
        unsigned outage = atomic_load_explicit(&s_outage_generation, memory_order_acquire);
        uint32_t address;
        bool up = station_has_ip(sta, &address);
        if (up && outage != last_outage) {
            /* The callback already fenced false. Restore a currently usable
             * IP even after a brief outage or a delayed disconnect event. */
            network_up = false;
        }
        last_outage = outage;
        if (up && network_up && address != last_address) {
            /* DHCP can replace the address without an observed down interval.
             * The relay must discard the session bound to the old address. */
            rw_relay_notify_network(false);
            network_up = false;
        }
        last_address = address;
        if (up != network_up) {
            network_up = up;
            rw_relay_notify_network(up);
            if (up) {
                ESP_LOGI(TAG, "station has an IP address");
            } else {
                ESP_LOGW(TAG, "station lost usable IP; recovering");
                rw_led_set(RW_LED_FAULT);
            }
        }

        rw_wifi_action_t action = rw_wifi_recovery_step(&recovery, now_ms(), up);
        if (action == RW_WIFI_CONNECT || action == RW_WIFI_BOUNCE) {
            ESP_LOGW(TAG, "Wi-Fi recovery attempt %lu%s",
                     (unsigned long)recovery.attempts,
                     action == RW_WIFI_BOUNCE ? " (driver restart)" : "");
            if (driver_started && recovery.attempts > 1) {
                esp_err_t err = esp_wifi_disconnect();
                if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT &&
                    err != ESP_ERR_WIFI_NOT_STARTED) {
                    ESP_LOGW(TAG, "disconnect failed: %s", esp_err_to_name(err));
                }
                if (err == ESP_ERR_WIFI_NOT_STARTED) {
                    driver_started = false;
                }
                if (action == RW_WIFI_BOUNCE && driver_started) {
                    err = esp_wifi_stop();
                    if (err == ESP_OK || err == ESP_ERR_WIFI_NOT_STARTED) {
                        driver_started = false;
                    } else {
                        ESP_LOGW(TAG, "stop failed: %s", esp_err_to_name(err));
                    }
                }
                /* This delay only blocks our task, never ESP's timer/event
                 * task. Let disconnect/stop finish before reconnecting. */
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            esp_err_t err = ESP_OK;
            if (!driver_started) {
                err = esp_wifi_start();
                if (err == ESP_OK) {
                    driver_started = true;
                } else {
                    ESP_LOGW(TAG, "start failed: %s", esp_err_to_name(err));
                }
            }
            if (err == ESP_OK) {
                err = esp_wifi_connect();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
                    if (err == ESP_ERR_WIFI_NOT_STARTED) {
                        driver_started = false;
                    }
                }
            }
            if (err != ESP_OK) {
                rw_wifi_recovery_failed(&recovery, now_ms());
            }
        }
        /* Poll even without SDK events: an accepted connect call, missed
         * disconnect, or DHCP stall must never disable recovery deadlines. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
}

esp_err_t rw_wifi_station_start(void)
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

    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, ssid, strnlen(ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, pass, strnlen(pass, sizeof(wc.sta.password)));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    TaskHandle_t wifi_task;
    if (xTaskCreate(station_task, "rw_wifi", 4096, sta, 5, &wifi_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, wifi_task, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, wifi_task, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_LOST_IP, wifi_event_handler, wifi_task, NULL));

    xTaskNotifyGive(wifi_task);
    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    return ESP_OK;
}
