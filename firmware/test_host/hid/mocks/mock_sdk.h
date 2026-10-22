#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_NOT_SUPPORTED 2
#define ESP_ERR_NOT_FOUND 3
#define ESP_ERR_TIMEOUT 4
#define ESP_ERR_INVALID_STATE 5
#define ESP_ERR_INVALID_ARG 6
#define ESP_ERR_INVALID_SIZE 7
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#ifndef CONFIG_RW_PROFILE_POWER_ONLY
#define CONFIG_RW_PROFILE_POWER_ONLY 0
#endif
#ifndef CONFIG_RW_ACT_KEYBOARD_INPUT
#define CONFIG_RW_ACT_KEYBOARD_INPUT 1 /* the fault-injection suite drives type/keys */
#endif
const char *esp_err_to_name(esp_err_t err);
int64_t esp_timer_get_time(void);
size_t strlcpy(char *dst, const char *src, size_t size);

typedef uint32_t TickType_t;
typedef struct mock_mutex *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define pdMS_TO_TICKS(ms) ((TickType_t)((ms) * CONFIG_FREERTOS_HZ / 1000))
#define pdTRUE 1
#define pdPASS 1
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait);
void xSemaphoreGive(SemaphoreHandle_t mutex);
void vSemaphoreDelete(SemaphoreHandle_t mutex);
int xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                unsigned priority, TaskHandle_t *task);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);

typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *child;
    int type;
    char *valuestring;
} cJSON;
#define cJSON_Array 1
#define cJSON_String 2
static inline bool cJSON_IsArray(const cJSON *j) { return j && j->type == cJSON_Array; }
static inline bool cJSON_IsString(const cJSON *j) { return j && j->type == cJSON_String; }
static inline int cJSON_GetArraySize(const cJSON *j) {
    int n = 0;
    for (const cJSON *c = j->child; c; c = c->next) ++n;
    return n;
}
#define cJSON_ArrayForEach(item, array) \
    for ((item) = (array)->child; (item); (item) = (item)->next)

typedef int hid_report_type_t;
#define HID_REPORT_TYPE_INPUT 1
#define HID_ITF_PROTOCOL_KEYBOARD 1
#define TUD_HID_REPORT_DESC_KEYBOARD(...) 0
#define HID_REPORT_ID(id) id
#define TUD_CONFIG_DESC_LEN 9
#define TUD_HID_DESC_LEN 25
#define TUD_CONFIG_DESCRIPTOR(...) 0
#define TUD_HID_DESCRIPTOR(...) 0
#define TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP 0x20
#define TUSB_DESC_DEVICE 1
#define CFG_TUD_ENDPOINT0_SIZE 64
typedef struct {
    uint8_t bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} tusb_desc_device_t;
typedef struct {
    const tusb_desc_device_t *device_descriptor;
    const char **string_descriptor;
    size_t string_descriptor_count;
    bool external_phy;
    const uint8_t *configuration_descriptor;
    bool self_powered;
} tinyusb_config_t;
esp_err_t tinyusb_driver_install(const tinyusb_config_t *config);
bool tud_mounted(void);
bool tud_suspended(void);
bool tud_remote_wakeup(void);
bool tud_hid_ready(void);
bool tud_hid_keyboard_report(uint8_t report_id, uint8_t modifier, uint8_t keys[6]);
bool tud_disconnect(void);
bool tud_connect(void);

#define KEYBOARD_MODIFIER_LEFTCTRL 1
#define KEYBOARD_MODIFIER_LEFTSHIFT 2
#define KEYBOARD_MODIFIER_LEFTALT 4
#define KEYBOARD_MODIFIER_LEFTGUI 8
#define KEYBOARD_MODIFIER_RIGHTCTRL 16
#define KEYBOARD_MODIFIER_RIGHTSHIFT 32
#define KEYBOARD_MODIFIER_RIGHTALT 64
#define KEYBOARD_MODIFIER_RIGHTGUI 128
enum {
    HID_KEY_A = 4, HID_KEY_ENTER = 40, HID_KEY_ESCAPE, HID_KEY_BACKSPACE,
    HID_KEY_TAB, HID_KEY_SPACE, HID_KEY_MINUS, HID_KEY_EQUAL,
    HID_KEY_CAPS_LOCK = 57, HID_KEY_F1 = 58, HID_KEY_PRINT_SCREEN = 70,
    HID_KEY_INSERT = 73, HID_KEY_HOME, HID_KEY_PAGE_UP, HID_KEY_DELETE,
    HID_KEY_END, HID_KEY_PAGE_DOWN, HID_KEY_ARROW_RIGHT, HID_KEY_ARROW_LEFT,
    HID_KEY_ARROW_DOWN, HID_KEY_ARROW_UP
};
/* TinyUSB's table is data, not the behavior under test. Keep only the exact
 * entries used by these tests; production still uses HID_ASCII_TO_KEYCODE. */
#define HID_ASCII_TO_KEYCODE \
    ['a'] = {0, 4}, ['A'] = {1, 4}, ['b'] = {0, 5}, \
    ['1'] = {0, 30}, ['!'] = {1, 30}, ['\n'] = {0, 40}, [' '] = {0, 44}
