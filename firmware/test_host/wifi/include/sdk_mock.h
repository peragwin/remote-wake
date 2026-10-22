#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_WIFI_NOT_CONNECT 3
#define ESP_ERR_WIFI_NOT_STARTED 4
#define ESP_ERROR_CHECK(expr) assert((expr) == ESP_OK)
const char *esp_err_to_name(esp_err_t err);
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))

typedef const char *esp_event_base_t;
static const char mock_wifi_event[] = "wifi";
static const char mock_ip_event[] = "ip";
#define WIFI_EVENT mock_wifi_event
#define IP_EVENT mock_ip_event
#define ESP_EVENT_ANY_ID -1
#define IP_EVENT_STA_GOT_IP 1
#define IP_EVENT_STA_LOST_IP 2
#define WIFI_EVENT_STA_DISCONNECTED 3
#define WIFI_EVENT_STA_STOP 4
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
    void (*handler)(void *, esp_event_base_t, int32_t, void *), void *arg,
    void *instance);

typedef struct { int unused; } esp_netif_t;
typedef struct { struct { uint32_t addr; } ip; } esp_netif_ip_info_t;
esp_netif_t *esp_netif_create_default_wifi_sta(void);
bool esp_netif_is_netif_up(esp_netif_t *sta);
esp_err_t esp_netif_get_ip_info(esp_netif_t *sta, esp_netif_ip_info_t *ip);

typedef struct { int unused; } wifi_ap_record_t;
typedef struct {
    struct {
        uint8_t ssid[32];
        uint8_t password[64];
        struct { int authmode; } threshold;
        struct { bool capable; } pmf_cfg;
    } sta;
} wifi_config_t;
#define WIFI_MODE_STA 1
#define WIFI_IF_STA 1
#define WIFI_AUTH_WPA2_PSK 1
#define WIFI_AUTH_OPEN 0
#define WIFI_PS_NONE 0
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_set_mode(int mode);
esp_err_t esp_wifi_set_config(int interface, wifi_config_t *config);
esp_err_t esp_wifi_set_ps(int ps);
int64_t esp_timer_get_time(void);
void esp_restart(void);

typedef void *TaskHandle_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY UINT32_MAX
uint32_t ulTaskNotifyTake(int clear, TickType_t ticks);
void xTaskNotifyGive(TaskHandle_t task);
int xTaskCreate(void (*task)(void *), const char *name, uint32_t stack,
                void *arg, unsigned priority, TaskHandle_t *handle);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task);

esp_err_t esp_task_wdt_add(TaskHandle_t task);
esp_err_t esp_task_wdt_reset(void);
