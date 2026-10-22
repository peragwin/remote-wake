#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "esp_err.h"
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_OTA_VALIDATE_FAILED 0x1503
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 0x10
#define ESP_PARTITION_SUBTYPE_APP_OTA_1 0x11
#define ESP_PARTITION_SUBTYPE_DATA_OTA 0
#define ESP_IMAGE_HEADER_MAGIC 0xe9
#define ESP_APP_DESC_MAGIC_WORD 0xabcd5432
#define OTA_WITH_SEQUENTIAL_WRITES 0xfffffffe
#define NVS_READWRITE 1
#define ESP_LOGE(tag,...) ((void)(tag))
#define CONFIG_IDF_TARGET "esp32s3"
typedef struct { uint32_t address,size; int type,subtype; } esp_partition_t;
typedef enum { ESP_OTA_IMG_NEW,ESP_OTA_IMG_PENDING_VERIFY,ESP_OTA_IMG_VALID,ESP_OTA_IMG_INVALID,ESP_OTA_IMG_ABORTED,ESP_OTA_IMG_UNDEFINED } esp_ota_img_states_t;
typedef struct { uint32_t magic_word,secure_version,reserv1[2];char version[32],project_name[32],time[16],date[16],idf_ver[32];uint8_t app_elf_sha256[32];uint32_t reserv2[20]; } esp_app_desc_t;
typedef struct { uint8_t magic,segment_count,rest[22]; } esp_image_header_t;
typedef struct { uint32_t load_addr,data_len; } esp_image_segment_header_t;
typedef unsigned esp_ota_handle_t;
typedef unsigned nvs_handle_t;
typedef void *SemaphoreHandle_t;
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef uint32_t TickType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdMS_TO_TICKS(ms) ((ms)/10)
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t m,TickType_t t);
void xSemaphoreGive(SemaphoreHandle_t m);
void vSemaphoreDelete(SemaphoreHandle_t m);
QueueHandle_t xQueueCreate(unsigned n,size_t size);
void vQueueDelete(QueueHandle_t q);
int xQueueSend(QueueHandle_t q,const void *data,TickType_t t);
int xQueueReceive(QueueHandle_t q,void *data,TickType_t t);
int xTaskCreate(void (*fn)(void*),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *task);
void vTaskDelay(TickType_t t);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
int64_t esp_timer_get_time(void);
uint32_t esp_random(void);
void esp_restart(void);
esp_err_t esp_flash_get_size(void *chip,uint32_t *size);
const esp_app_desc_t *esp_app_get_description(void);
const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_boot_partition(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start);
const esp_partition_t *esp_partition_find_first(int type,int subtype,const char *label);
esp_err_t esp_partition_read(const esp_partition_t *part,size_t offset,void *out,size_t size);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *part,esp_ota_img_states_t *state);
esp_err_t esp_ota_get_partition_description(const esp_partition_t *part,esp_app_desc_t *desc);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void);
esp_err_t esp_ota_begin(const esp_partition_t *part,size_t size,esp_ota_handle_t *handle);
esp_err_t esp_ota_write(esp_ota_handle_t handle,const void *data,size_t size);
esp_err_t esp_ota_end(esp_ota_handle_t handle);
esp_err_t esp_ota_abort(esp_ota_handle_t handle);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *part);
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_get_blob(nvs_handle_t handle,const char *key,void *out,size_t *size);
esp_err_t nvs_set_blob(nvs_handle_t handle,const char *key,const void *data,size_t size);
esp_err_t nvs_commit(nvs_handle_t handle);
typedef struct http_mock *esp_http_client_handle_t;
typedef struct { const char *url;int timeout_ms;esp_err_t (*crt_bundle_attach)(void*);bool disable_auto_redirect;int max_redirection_count,buffer_size,buffer_size_tx;const char *user_agent; } esp_http_client_config_t;
esp_err_t esp_crt_bundle_attach(void *config);
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c,const char *key,const char *value);
esp_err_t esp_http_client_open(esp_http_client_handle_t c,int length);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c);
int esp_http_client_get_status_code(esp_http_client_handle_t c);
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t c);
esp_err_t esp_http_client_get_url(esp_http_client_handle_t c,char *out,int size);
esp_err_t esp_http_client_close(esp_http_client_handle_t c);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c);
int esp_http_client_read(esp_http_client_handle_t c,char *out,int size);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c);
