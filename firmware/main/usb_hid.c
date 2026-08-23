#include "usb_hid.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "rw.hid";

#if CONFIG_RW_PROFILE_POWER_ONLY

/* ------------------------------------------------------------- stubs ---- */

esp_err_t rw_hid_init(void)
{
    ESP_LOGI(TAG, "power-only profile: USB HID disabled");
    return ESP_OK;
}
bool rw_hid_available(void) { return false; }
bool rw_hid_mounted(void)   { return false; }
bool rw_hid_suspended(void) { return false; }
esp_err_t rw_hid_wake(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t rw_hid_type(const char *text, bool enter)
{
    (void)text; (void)enter;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t rw_hid_keys(const cJSON *seq)
{
    (void)seq;
    return ESP_ERR_NOT_SUPPORTED;
}

#else /* full profile ------------------------------------------------------ */

#include "class/hid/hid_device.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tinyusb.h"

#define HID_INSTANCE  0
#define KBD_REPORT_ID HID_ITF_PROTOCOL_KEYBOARD /* == 1 */

/* --------------------------------------------------------- descriptors -- */

static const uint8_t s_hid_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(KBD_REPORT_ID)),
};

#define RW_CFG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
#define RW_EP_IN         0x81

static const uint8_t s_cfg_desc[] = {
    /* 1 interface, bus-powered, remote wakeup capable (required so the host
     * lets us wake it from S3), 100 mA. */
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, RW_CFG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    /* itf 0, string idx 4, non-boot, report desc len, EP IN, 16 B, 10 ms */
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(s_hid_report_desc), RW_EP_IN, 16, 10),
};

static const tusb_desc_device_t s_dev_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,  /* Espressif */
    .idProduct          = 0x4004,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

static char s_serial[RW_DEVICE_ID_HEXLEN + 1] = "000000000000000";
static const char *s_str_desc[5] = {
    (const char[]){0x09, 0x04},  /* 0: en-US */
    "remote-wake",               /* 1: manufacturer */
    "remote-wake keyboard",      /* 2: product */
    s_serial,                    /* 3: serial (deviceId) */
    "remote-wake HID",           /* 4: HID interface */
};

/* --------------------------------------------------- tinyusb callbacks -- */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize)
{
    /* Host LED state (caps/num lock). We do not care. */
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}

/* -------------------------------------------------------------- state --- */

static SemaphoreHandle_t s_lock;
static bool              s_installed;

bool rw_hid_available(void) { return s_installed; }
bool rw_hid_mounted(void)   { return s_installed && tud_mounted(); }
bool rw_hid_suspended(void) { return s_installed && tud_suspended(); }

esp_err_t rw_hid_init(void)
{
    if (s_installed) {
        return ESP_OK;
    }
    strlcpy(s_serial, rw_config_device_id(), sizeof(s_serial));
    if (s_serial[0] == '\0') {
        strlcpy(s_serial, "0000000000000000", sizeof(s_serial));
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    const tinyusb_config_t tusb_cfg = {
        .device_descriptor        = &s_dev_desc,
        .string_descriptor        = s_str_desc,
        .string_descriptor_count  = sizeof(s_str_desc) / sizeof(s_str_desc[0]),
        .external_phy             = false,
        .configuration_descriptor = s_cfg_desc,
        .self_powered             = false,
    };
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return err;
    }
    s_installed = true;
    ESP_LOGI(TAG, "USB HID keyboard installed (serial %s)", s_serial);
    return ESP_OK;
}

/* ---------------------------------------------------------- report i/o -- */

/* Wait for the HID endpoint to be free. */
static bool wait_ready(uint32_t timeout_ms)
{
    const uint32_t step = 5;
    for (uint32_t waited = 0; waited <= timeout_ms; waited += step) {
        if (tud_hid_ready()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(step));
    }
    return false;
}

/* Ask the host to resume the bus, then give it time to actually do so. */
/* ESP_ERR_NOT_FOUND / ESP_ERR_TIMEOUT both mean "usb_down" to the dispatcher;
 * ESP_ERR_INVALID_STATE is reserved for "another HID action is running". */
static esp_err_t ensure_awake(void)
{
    if (!tud_mounted()) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!tud_suspended()) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "bus suspended — issuing remote wakeup");
    if (!tud_remote_wakeup()) {
        /* Host disabled remote wakeup; reports would be dropped. */
        ESP_LOGW(TAG, "remote wakeup refused by host");
        return ESP_ERR_TIMEOUT;
    }
    for (int i = 0; i < 100 && tud_suspended(); i++) {  /* up to 1 s */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return tud_suspended() ? ESP_ERR_TIMEOUT : ESP_OK;
}

static esp_err_t send_report(uint8_t modifier, uint8_t keycode[6])
{
    if (!wait_ready(200)) {
        return ESP_ERR_TIMEOUT;
    }
    if (!tud_hid_keyboard_report(KBD_REPORT_ID, modifier, keycode)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t release_all(void)
{
    uint8_t none[6] = {0};
    return send_report(0, none);
}

/* -------------------------------------------------------------- actions -- */

esp_err_t rw_hid_wake(void)
{
    if (!s_installed) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ensure_awake();
    if (err == ESP_OK) {
        uint8_t none[6] = {0};
        for (int i = 0; i < 2 && err == ESP_OK; i++) {
            if (i) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            /* Left-Ctrl alone has no side effect on any mainstream OS. */
            err = send_report(KEYBOARD_MODIFIER_LEFTCTRL, none);
            if (err == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(15));
                err = release_all();
            }
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

/* US-layout ASCII -> {shift, keycode}, straight from TinyUSB. */
static const uint8_t s_ascii2kc[128][2] = {HID_ASCII_TO_KEYCODE};

esp_err_t rw_hid_type(const char *text, bool enter)
{
    if (!s_installed) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!text) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = strlen(text);
    if (n > RW_TYPE_MAX_CHARS) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* US layout only (a documented v1 non-goal): reject anything non-ASCII up
     * front rather than typing garbage into a login box. */
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 128 || s_ascii2kc[c][1] == 0) {
            ESP_LOGW(TAG, "unmappable character 0x%02x at %u", c, (unsigned)i);
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ensure_awake();

    for (size_t i = 0; i < n && err == ESP_OK; i++) {
        unsigned char c = (unsigned char)text[i];
        uint8_t kc[6] = {0};
        uint8_t mod = s_ascii2kc[c][0] ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
        kc[0] = s_ascii2kc[c][1];

        err = send_report(mod, kc);
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(8));
            err = release_all();
            vTaskDelay(pdMS_TO_TICKS(8));
        }
    }

    if (err == ESP_OK && enter) {
        uint8_t kc[6] = {HID_KEY_ENTER, 0, 0, 0, 0, 0};
        err = send_report(0, kc);
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(8));
            err = release_all();
        }
    }

    xSemaphoreGive(s_lock);
    return err;
}

/* ------------------------------------------------------------ key names -- */

typedef struct {
    const char *name;
    uint8_t     keycode;   /* 0 when this entry is a modifier */
    uint8_t     modifier;  /* KEYBOARD_MODIFIER_* bit, 0 otherwise */
} rw_keyname_t;

static const rw_keyname_t s_keynames[] = {
    /* modifiers */
    {"LCTRL",  0, KEYBOARD_MODIFIER_LEFTCTRL},
    {"LSHIFT", 0, KEYBOARD_MODIFIER_LEFTSHIFT},
    {"LALT",   0, KEYBOARD_MODIFIER_LEFTALT},
    {"LGUI",   0, KEYBOARD_MODIFIER_LEFTGUI},
    {"RCTRL",  0, KEYBOARD_MODIFIER_RIGHTCTRL},
    {"RSHIFT", 0, KEYBOARD_MODIFIER_RIGHTSHIFT},
    {"RALT",   0, KEYBOARD_MODIFIER_RIGHTALT},
    {"RGUI",   0, KEYBOARD_MODIFIER_RIGHTGUI},
    /* named keys */
    {"ENTER",     HID_KEY_ENTER, 0},
    {"RETURN",    HID_KEY_ENTER, 0},
    {"ESC",       HID_KEY_ESCAPE, 0},
    {"ESCAPE",    HID_KEY_ESCAPE, 0},
    {"BACKSPACE", HID_KEY_BACKSPACE, 0},
    {"TAB",       HID_KEY_TAB, 0},
    {"SPACE",     HID_KEY_SPACE, 0},
    {"CAPSLOCK",  HID_KEY_CAPS_LOCK, 0},
    {"PRINTSCREEN", HID_KEY_PRINT_SCREEN, 0},
    {"INSERT",    HID_KEY_INSERT, 0},
    {"HOME",      HID_KEY_HOME, 0},
    {"PAGEUP",    HID_KEY_PAGE_UP, 0},
    {"DELETE",    HID_KEY_DELETE, 0},
    {"END",       HID_KEY_END, 0},
    {"PAGEDOWN",  HID_KEY_PAGE_DOWN, 0},
    {"RIGHT",     HID_KEY_ARROW_RIGHT, 0},
    {"LEFT",      HID_KEY_ARROW_LEFT, 0},
    {"DOWN",      HID_KEY_ARROW_DOWN, 0},
    {"UP",        HID_KEY_ARROW_UP, 0},
    {"MINUS",     HID_KEY_MINUS, 0},
    {"EQUAL",     HID_KEY_EQUAL, 0},
};

static bool lookup_keyname(const char *name, uint8_t *kc, uint8_t *mod)
{
    if (!name || !name[0]) {
        return false;
    }
    /* Single ASCII character: reuse the US layout table (covers 0-9 and
     * punctuation, including the shifted forms). */
    if (name[1] == '\0') {
        unsigned char c = (unsigned char)name[0];
        /* Letters name the physical key, not the character: "L" in
         * [["LGUI","L"]] must be Win+L, never Win+Shift+L. */
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            unsigned char up = (c >= 'a') ? (unsigned char)(c - 32) : c;
            *kc  = (uint8_t)(HID_KEY_A + (up - 'A'));
            *mod = 0;
            return true;
        }
        if (c < 128 && s_ascii2kc[c][1] != 0) {
            *kc  = s_ascii2kc[c][1];
            *mod = s_ascii2kc[c][0] ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
            return true;
        }
        return false;
    }
    /* F1..F12 */
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '0' && name[1] <= '9') {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 12) {
            *kc  = (uint8_t)(HID_KEY_F1 + (n - 1));
            *mod = 0;
            return true;
        }
    }
    for (size_t i = 0; i < sizeof(s_keynames) / sizeof(s_keynames[0]); i++) {
        if (strcasecmp(name, s_keynames[i].name) == 0) {
            *kc  = s_keynames[i].keycode;
            *mod = s_keynames[i].modifier;
            return true;
        }
    }
    return false;
}

esp_err_t rw_hid_keys(const cJSON *seq)
{
    if (!s_installed) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!cJSON_IsArray(seq)) {
        return ESP_ERR_INVALID_ARG;
    }
    int nchords = cJSON_GetArraySize(seq);
    if (nchords < 1 || nchords > RW_KEYS_MAX_CHORDS) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Validate the whole sequence before pressing anything. */
    const cJSON *chord = NULL;
    cJSON_ArrayForEach(chord, seq) {
        if (!cJSON_IsArray(chord)) {
            return ESP_ERR_INVALID_ARG;
        }
        int nk = cJSON_GetArraySize(chord);
        if (nk < 1 || nk > RW_KEYS_MAX_PER_CHORD + 4) {
            return ESP_ERR_INVALID_SIZE;
        }
        const cJSON *k = NULL;
        int normal = 0;
        cJSON_ArrayForEach(k, chord) {
            uint8_t kc = 0, mod = 0;
            if (!cJSON_IsString(k) || !lookup_keyname(k->valuestring, &kc, &mod)) {
                ESP_LOGW(TAG, "unknown key name in chord");
                return ESP_ERR_INVALID_ARG;
            }
            if (kc) {
                normal++;
            }
        }
        if (normal > RW_KEYS_MAX_PER_CHORD) {
            return ESP_ERR_INVALID_SIZE;  /* 6-key rollover limit */
        }
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ensure_awake();

    bool first = true;
    cJSON_ArrayForEach(chord, seq) {
        if (err != ESP_OK) {
            break;
        }
        if (!first) {
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        first = false;

        uint8_t mods = 0;
        uint8_t kcs[6] = {0};
        int ni = 0;
        const cJSON *k = NULL;
        cJSON_ArrayForEach(k, chord) {
            uint8_t kc = 0, mod = 0;
            lookup_keyname(k->valuestring, &kc, &mod);
            mods |= mod;
            if (kc && ni < 6) {
                kcs[ni++] = kc;
            }
        }
        err = send_report(mods, kcs);
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(15));
            err = release_all();
        }
    }

    xSemaphoreGive(s_lock);
    return err;
}

#endif /* CONFIG_RW_PROFILE_POWER_ONLY */
