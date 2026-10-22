#include "usb_hid.h"

#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "rw.hid";

#if CONFIG_RW_PROFILE_POWER_ONLY

/* ------------------------------------------------------------- stubs ---- */

esp_err_t rw_hid_init(void)
{
    ESP_LOGI(TAG, "power-only profile: USB HID disabled");
    return ESP_OK;
}
esp_err_t rw_hid_init_external(void) { return rw_hid_init(); }
const uint8_t *rw_hid_report_descriptor(size_t *length)
{
    if (length) *length = 0;
    return NULL;
}
esp_err_t rw_hid_quiesce(void) { return ESP_OK; }
void rw_hid_resume_actions(void) {}
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
#include <stdatomic.h>
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

const uint8_t *rw_hid_report_descriptor(size_t *length)
{
    if (length) *length = sizeof(s_hid_report_desc);
    return s_hid_report_desc;
}

/* --------------------------------------------------- tinyusb callbacks -- */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return instance == HID_INSTANCE ? s_hid_report_desc : NULL;
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
static atomic_bool       s_quiesced;
static atomic_bool       s_installed; /* publishes initialized state to worker */
static TaskHandle_t      s_recovery_task;
/* Only the action/recovery task holding s_lock may submit reports or change
 * s_need_release. Keep this debt until a neutral report completes, including
 * across disconnects, suspend, failed submissions and completion timeouts. */
static bool s_need_release = true;
static portMUX_TYPE s_report_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_expected_report[9]; /* report ID + keyboard report */
static bool s_waiting_report;
static bool s_report_complete;
static bool s_neutral_completed; /* completed release not yet claimed by owner */
static int64_t s_action_deadline; /* s_lock; zero during background cleanup */
static uint32_t s_bus_epoch; /* callback lifecycle, protected by s_report_mux */
static uint32_t s_action_epoch; /* s_lock */
static bool s_action_epoch_valid;
static bool s_reenumerating; /* protected by s_report_mux */

static void recovery_task(void *arg);

/* This callback runs in TinyUSB's task. Never wait or submit from here. Match
 * the complete report so a late key-down cannot acknowledge a recovery key-up.
 * A short/failed transfer must not count as a completed keyboard report. */
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report,
                                uint16_t len)
{
    portENTER_CRITICAL(&s_report_mux);
    if (instance == HID_INSTANCE && s_waiting_report) {
        s_report_complete = len == sizeof(s_expected_report) &&
                            memcmp(report, s_expected_report, len) == 0;
        s_neutral_completed = s_report_complete;
        for (size_t i = 1; i < sizeof(s_expected_report); ++i) {
            if (s_expected_report[i]) {
                s_neutral_completed = false;
            }
        }
        s_waiting_report = false;
    }
    portEXIT_CRITICAL(&s_report_mux);
}

/* Available in newer TinyUSB versions; an unused application symbol with
 * older versions, where short completions are rejected above. */
void tud_hid_report_failed_cb(uint8_t instance, hid_report_type_t report_type,
                              uint8_t const *report, uint16_t xferred_bytes)
{
    (void)report;
    (void)xferred_bytes;
    if (instance != HID_INSTANCE || report_type != HID_REPORT_TYPE_INPUT) {
        return;
    }
    portENTER_CRITICAL(&s_report_mux);
    s_waiting_report = false;
    s_report_complete = false;
    s_neutral_completed = false;
    portEXIT_CRITICAL(&s_report_mux);
}

/* esp_tinyusb 1.x leaves lifecycle callbacks to the application (except its
 * MSC helper, disabled in our HID-only configuration). Observe actual events,
 * not just polled mounted state: reset/re-enumeration can happen between polls
 * and cancel a report without ever delivering its completion callback. */
static void lifecycle_event(bool endpoint_reset, bool mounted)
{
    portENTER_CRITICAL(&s_report_mux);
    ++s_bus_epoch;
    if (endpoint_reset) {
        s_waiting_report = false;
        s_report_complete = false;
        s_neutral_completed = false;
    }
    if (mounted) {
        s_reenumerating = false;
    }
    portEXIT_CRITICAL(&s_report_mux);
}

void tud_mount_cb(void) { lifecycle_event(true, true); }
void tud_umount_cb(void) { lifecycle_event(true, false); }
void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    lifecycle_event(false, false);
}
void tud_resume_cb(void) { lifecycle_event(false, false); }

bool rw_hid_available(void) { return s_installed; }
bool rw_hid_mounted(void)
{
    portENTER_CRITICAL(&s_report_mux);
    bool reenumerating = s_reenumerating;
    portEXIT_CRITICAL(&s_report_mux);
    return s_installed && !reenumerating && tud_mounted();
}
bool rw_hid_suspended(void) { return s_installed && tud_suspended(); }

static esp_err_t init(bool external)
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

    /* Allocate recovery before installing the driver: init must never report
     * success without a way to clear a held key after the caller has returned. */
    if (xTaskCreate(recovery_task, "hid-release", 3072, NULL, 5,
                    &s_recovery_task) != pdPASS) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
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
    esp_err_t err = external ? ESP_OK : tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install: %s", esp_err_to_name(err));
        vTaskDelete(s_recovery_task);
        s_recovery_task = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return err;
    }
    s_installed = true;
    ESP_LOGI(TAG, "USB HID keyboard installed (serial %s)", s_serial);
    return ESP_OK;
}

esp_err_t rw_hid_init(void) { return init(false); }
esp_err_t rw_hid_init_external(void) { return init(true); }

/* ---------------------------------------------------------- report i/o -- */

/* Actual elapsed time, independent of CONFIG_FREERTOS_HZ. In particular,
 * pdMS_TO_TICKS(5) and (8) are zero with IDF's default 100 Hz tick. */
static TickType_t delay_ticks(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks ? ticks : 1;
}

static esp_err_t bus_state(void)
{
    portENTER_CRITICAL(&s_report_mux);
    bool reenumerating = s_reenumerating;
    bool changed = s_action_epoch_valid && s_action_epoch != s_bus_epoch;
    portEXIT_CRITICAL(&s_report_mux);
    if (reenumerating) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!tud_mounted()) {
        /* Unmount resets the endpoint and cancels its transfer. Do not reuse
         * the endpoint after a mere timeout until its callback has run: the
         * TinyUSB callback's report pointer aliases its submission buffer. */
        portENTER_CRITICAL(&s_report_mux);
        s_waiting_report = false;
        s_report_complete = false;
        s_neutral_completed = false;
        portEXIT_CRITICAL(&s_report_mux);
        return ESP_ERR_NOT_FOUND;
    }
    return (tud_suspended() || changed) ? ESP_ERR_TIMEOUT : ESP_OK;
}

/* Ask the host to resume the bus, then give it time to actually do so. */
/* ESP_ERR_NOT_FOUND / ESP_ERR_TIMEOUT both mean "usb_down" to the dispatcher;
 * ESP_ERR_INVALID_STATE is reserved for "another HID action is running". */
static esp_err_t ensure_awake(void)
{
    if (!tud_mounted()) {
        return bus_state();
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
    const int64_t deadline = esp_timer_get_time() + 1000000;
    while (tud_mounted() && tud_suspended() && esp_timer_get_time() < deadline) {
        vTaskDelay(1);
    }
    return bus_state();
}

static esp_err_t send_report(uint8_t modifier, uint8_t keycode[6])
{
    int64_t deadline = esp_timer_get_time() + 200000;
    if (s_action_deadline && s_action_deadline < deadline) {
        deadline = s_action_deadline;
    }
    esp_err_t err;
    for (;;) {
        err = bus_state();
        portENTER_CRITICAL(&s_report_mux);
        bool pending = s_waiting_report;
        portEXIT_CRITICAL(&s_report_mux);
        if (err != ESP_OK || (!pending && tud_hid_ready()) ||
            esp_timer_get_time() >= deadline) {
            break;
        }
        vTaskDelay(1);
    }
    if (err != ESP_OK) {
        return err;
    }
    if (esp_timer_get_time() >= deadline) {
        return ESP_ERR_TIMEOUT;
    }

    bool neutral = modifier == 0;
    for (size_t i = 0; i < 6; ++i) {
        if (keycode[i]) {
            neutral = false;
        }
    }

    portENTER_CRITICAL(&s_report_mux);
    /* A timed-out release may have completed while this cleanup waited for
     * its callback. Claim that completion instead of queueing a duplicate
     * release that could consume another full timeout on a slow host. */
    if (neutral && s_neutral_completed) {
        s_neutral_completed = false;
        portEXIT_CRITICAL(&s_report_mux);
        return ESP_OK;
    }
    s_expected_report[0] = KBD_REPORT_ID;
    s_expected_report[1] = modifier;
    s_expected_report[2] = 0; /* reserved */
    memcpy(s_expected_report + 3, keycode, 6);
    s_report_complete = false;
    s_neutral_completed = false;
    s_waiting_report = true;
    portEXIT_CRITICAL(&s_report_mux);

    /* Set debt even if submission fails: do not assume what reached the host. */
    s_need_release = true;
    if (!tud_hid_keyboard_report(KBD_REPORT_ID, modifier, keycode)) {
        err = ESP_FAIL;
    } else {
        for (;;) {
            err = bus_state();
            portENTER_CRITICAL(&s_report_mux);
            bool complete = s_report_complete;
            bool pending = s_waiting_report;
            portEXIT_CRITICAL(&s_report_mux);
            if (err != ESP_OK || complete) {
                break;
            }
            if (!pending) {
                err = ESP_FAIL; /* callback reported a short/invalid report */
                break;
            }
            if (esp_timer_get_time() >= deadline) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            vTaskDelay(1);
        }
    }
    if (err == ESP_FAIL) {
        /* A rejected submission has no callback. A timed-out accepted one
         * keeps ownership of TinyUSB's report buffer until completion/reset. */
        portENTER_CRITICAL(&s_report_mux);
        s_waiting_report = false;
        portEXIT_CRITICAL(&s_report_mux);
    }
    return err;
}

static esp_err_t release_all(void)
{
    uint8_t none[6] = {0};
    esp_err_t err = send_report(0, none);
    if (err == ESP_OK) {
        s_need_release = false;
        portENTER_CRITICAL(&s_report_mux);
        s_neutral_completed = false;
        portEXIT_CRITICAL(&s_report_mux);
    }
    return err;
}

static void restart_usb(void)
{
    /* A host that stopped polling can retain the last key indefinitely. After
     * bounded neutral cleanup fails, detach the keyboard to clear host state.
     * Only a genuine new mount callback can permit more reports afterwards. */
    portENTER_CRITICAL(&s_report_mux);
    s_reenumerating = true;
    portEXIT_CRITICAL(&s_report_mux);
    ESP_LOGW(TAG, "neutral release failed; reconnecting USB keyboard");
    if (!tud_disconnect()) {
        ESP_LOGE(TAG, "USB detach failed; HID blocked until next mount");
        return;
    }
    vTaskDelay(delay_ticks(50));
    portENTER_CRITICAL(&s_report_mux);
    s_reenumerating = true;
    portEXIT_CRITICAL(&s_report_mux);
    if (!tud_connect()) {
        ESP_LOGE(TAG, "USB reconnect failed; HID blocked until next mount");
    }
}

/* Cleanup continues after a command error, even with no further commands.
 * Never replay a key-down or wake a sleeping host from this worker. */
static void recovery_once(void)
{
    static bool was_active;
    static uint32_t observed_epoch;
    if (!s_installed || s_quiesced || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    if (s_quiesced) {
        xSemaphoreGive(s_lock);
        return;
    }
    bool active = bus_state() == ESP_OK;
    portENTER_CRITICAL(&s_report_mux);
    uint32_t epoch = s_bus_epoch;
    portEXIT_CRITICAL(&s_report_mux);
    if (!active || !was_active || epoch != observed_epoch) {
        s_need_release = true;
    }
    if (active && s_need_release) {
        if (release_all() != ESP_OK && bus_state() == ESP_OK) {
            restart_usb();
        }
    }
    observed_epoch = epoch;
    was_active = active;
    xSemaphoreGive(s_lock);
}

static void recovery_task(void *arg)
{
    (void)arg;
    for (;;) {
        recovery_once();
        vTaskDelay(delay_ticks(20));
    }
}

static esp_err_t begin_action(void)
{
    esp_err_t err = ensure_awake();
    portENTER_CRITICAL(&s_report_mux);
    s_action_epoch = s_bus_epoch;
    s_action_epoch_valid = true;
    portEXIT_CRITICAL(&s_report_mux);
    /* A neutral baseline also covers reconnects too brief for the worker to
     * observe. No new key-down is allowed before this completion. */
    return err == ESP_OK ? release_all() : err;
}

static esp_err_t lock_action(void)
{
    /* Budget includes lock contention and resume; reserve bounded cleanup
     * after aborting. A slowly progressing endpoint must not block relay
     * servicing indefinitely or continue typing after its command timed out. */
    if (s_quiesced) return ESP_ERR_INVALID_STATE;
    const int64_t deadline = esp_timer_get_time() + 8000000;
    if (xSemaphoreTake(s_lock, delay_ticks(2000)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_quiesced) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_action_deadline = deadline;
    return ESP_OK;
}

esp_err_t rw_hid_quiesce(void)
{
    if (!s_installed) return ESP_OK;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    if (s_quiesced) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    /* Never wake the host for maintenance. A detached/suspended bus will be
     * physically disconnected by the maintenance task. Otherwise complete a
     * neutral report, with the usual bounded transfer deadline. */
    esp_err_t err = bus_state() == ESP_OK ? release_all() : ESP_OK;
    if (err == ESP_OK) s_quiesced = true;
    xSemaphoreGive(s_lock);
    return err;
}

void rw_hid_resume_actions(void)
{
    s_quiesced = false;
}

static esp_err_t finish_action(esp_err_t err)
{
    s_action_deadline = 0; /* cleanup has its own 200 ms report budget */
    s_action_epoch_valid = false;
    if (s_need_release && bus_state() == ESP_OK) {
        esp_err_t release_err = release_all();
        if (err == ESP_OK) {
            err = release_err;
        }
        if (release_err != ESP_OK && bus_state() == ESP_OK) {
            restart_usb();
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

static esp_err_t press_release(uint8_t modifier, uint8_t keycode[6])
{
    esp_err_t err = send_report(modifier, keycode);
    return err == ESP_OK ? release_all() : err;
}

/* -------------------------------------------------------------- actions -- */

esp_err_t rw_hid_wake(void)
{
    if (!s_installed) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (lock_action() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = begin_action();
    if (err == ESP_OK) {
        uint8_t none[6] = {0};
        for (int i = 0; i < 2 && err == ESP_OK; i++) {
            if (i) {
                vTaskDelay(delay_ticks(50));
            }
            /* Left-Ctrl alone has no side effect on any mainstream OS. */
            err = press_release(KEYBOARD_MODIFIER_LEFTCTRL, none);
        }
    }
    return finish_action(err);
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

    if (lock_action() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = begin_action();

    for (size_t i = 0; i < n && err == ESP_OK; i++) {
        unsigned char c = (unsigned char)text[i];
        uint8_t kc[6] = {0};
        uint8_t mod = s_ascii2kc[c][0] ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
        kc[0] = s_ascii2kc[c][1];

        err = press_release(mod, kc);
    }

    if (err == ESP_OK && enter) {
        uint8_t kc[6] = {HID_KEY_ENTER, 0, 0, 0, 0, 0};
        err = press_release(0, kc);
    }

    return finish_action(err);
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
    if (name[0] == 'F' || name[0] == 'f') {
        int n = 0;
        if (name[1] >= '1' && name[1] <= '9' && name[2] == '\0') {
            n = name[1] - '0';
        } else if (name[1] == '1' && name[2] >= '0' && name[2] <= '2' &&
                   name[3] == '\0') {
            n = 10 + name[2] - '0';
        }
        if (n) {
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

    if (lock_action() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = begin_action();

    bool first = true;
    cJSON_ArrayForEach(chord, seq) {
        if (err != ESP_OK) {
            break;
        }
        if (!first) {
            vTaskDelay(delay_ticks(30));
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
        err = press_release(mods, kcs);
    }

    return finish_action(err);
}

#endif /* CONFIG_RW_PROFILE_POWER_ONLY */
