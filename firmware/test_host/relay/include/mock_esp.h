#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define ESP_ERR_NO_MEM 4
#define ESP_ERR_NOT_SUPPORTED 5
#define ESP_ERR_NOT_FOUND 6
#define ESP_ERR_INVALID_SIZE 7
const char *esp_err_to_name(esp_err_t err);
#define CONFIG_ESP_TASK_WDT_INIT 1
#define CONFIG_ESP_TASK_WDT_PANIC 1
#define CONFIG_RW_AUTH_FAIL_THRESHOLD 3
#define CONFIG_RW_AUTH_LOCKOUT_SEC 60
#define CONFIG_RW_TS_WINDOW_SEC 90

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
typedef unsigned TickType_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(m) pthread_mutex_lock(m)
#define portEXIT_CRITICAL(m) pthread_mutex_unlock(m)
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX

typedef void *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned depth, unsigned size);
int xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout);
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout);
void vQueueDelete(QueueHandle_t queue);
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *arg,
                unsigned prio, void *handle);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(void *task);
esp_err_t esp_task_wdt_add(void *task);
esp_err_t esp_task_wdt_reset(void);
int64_t esp_timer_get_time(void);
void esp_restart(void);
uint32_t esp_random(void);
esp_err_t esp_crt_bundle_attach(void *conf);

typedef struct { int unused; } esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(server) ((esp_sntp_config_t){0})
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config);
typedef struct { int rssi; } wifi_ap_record_t;
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap);

typedef struct mock_ws *esp_websocket_client_handle_t;
typedef const char *esp_event_base_t;
enum { WEBSOCKET_EVENT_ANY = -1, WEBSOCKET_EVENT_ERROR,
       WEBSOCKET_EVENT_CONNECTED, WEBSOCKET_EVENT_DISCONNECTED,
       WEBSOCKET_EVENT_DATA, WEBSOCKET_EVENT_CLOSED };
typedef struct {
    const char *data_ptr;
    int data_len;
    bool fin;
    uint8_t op_code;
    esp_websocket_client_handle_t client;
    void *user_context;
    int payload_len;
    int payload_offset;
} esp_websocket_event_data_t;
typedef struct {
    const char *uri;
    const char *headers;
    bool disable_auto_reconnect;
    int task_stack;
    int task_prio;
    int buffer_size;
    int network_timeout_ms;
    int ping_interval_sec;
    bool disable_pingpong_discon;
    esp_err_t (*crt_bundle_attach)(void *);
} esp_websocket_client_config_t;
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *cfg);
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t client, int event,
                    void (*handler)(void *, esp_event_base_t, int32_t, void *), void *arg);
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t client);
esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t client);
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t client);
bool esp_websocket_client_is_connected(esp_websocket_client_handle_t client);
int esp_websocket_client_send_text(esp_websocket_client_handle_t client, const char *text,
                                 int len, TickType_t timeout);

typedef struct cJSON { double valuedouble; char *valuestring; int type; } cJSON;
cJSON *cJSON_CreateObject(void);
void cJSON_Delete(cJSON *item);
void cJSON_free(void *item);
char *cJSON_PrintUnformatted(const cJSON *item);
cJSON *cJSON_ParseWithLength(const char *text, size_t len);
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *item, const char *name);
bool cJSON_IsObject(const cJSON *item);
bool cJSON_IsString(const cJSON *item);
bool cJSON_IsNumber(const cJSON *item);
bool cJSON_IsBool(const cJSON *item);
bool cJSON_IsTrue(const cJSON *item);
bool cJSON_IsArray(const cJSON *item);
void cJSON_AddNumberToObject(cJSON *item, const char *name, double value);
void cJSON_AddStringToObject(cJSON *item, const char *name, const char *value);
void cJSON_AddBoolToObject(cJSON *item, const char *name, bool value);
void cJSON_AddItemToObject(cJSON *item, const char *name, cJSON *value);
