/* Compile the production implementation, not a second implementation of it. */
#include "mock_sdk.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../main/usb_hid.c"

struct mock_mutex { bool held; } mutex;
static int64_t now_us, due_us, ready_at_us, resume_at_us;
static bool mounted = true, suspended, remote_allowed = true, pending;
static uint8_t pending_report[9], held_report[9];
static uint8_t submitted[2048][9], completed[2048][9];
static int nsubmit, ncomplete, ndown, reject_neutral, short_transfer_at, failed_transfer_at;
static int stall_down_at, disconnect_down_at, suspend_down_at;
static int quick_reset_down_at, disconnect_count;
static int stall_transfer_at;
static bool auto_reenumerate = true;
static int report_latency_us = 10000, delayed_down_us;
static bool task_fail, install_fail, mutex_fail;
static void (*created_task)(void *);

static bool neutral(const uint8_t report[9])
{
    for (int i = 1; i < 9; ++i) if (report[i]) return false;
    return true;
}

static void pump(void)
{
    if (resume_at_us && now_us >= resume_at_us) {
        suspended = false;
        tud_resume_cb();
        resume_at_us = 0;
    }
    if (pending && mounted && !suspended && now_us >= due_us) {
        pending = false;
        if (nsubmit == failed_transfer_at) {
            tud_hid_report_failed_cb(0, HID_REPORT_TYPE_INPUT, pending_report, 9);
        } else if (nsubmit != short_transfer_at) {
            memcpy(held_report, pending_report, 9);
            memcpy(completed[ncomplete++], pending_report, 9);
            tud_hid_report_complete_cb(0, pending_report, 9);
        } else {
            tud_hid_report_complete_cb(0, pending_report, 0);
        }
    }
}

int64_t esp_timer_get_time(void) { return now_us; }
void vTaskDelay(TickType_t ticks)
{
    assert(ticks > 0); /* catches the original default-100-Hz busy loop */
    now_us += (int64_t)ticks * 1000000 / CONFIG_FREERTOS_HZ;
    pump();
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return mutex_fail ? NULL : &mutex; }
int xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait)
{
    assert(m);
    if (m->held) { if (wait) vTaskDelay(wait); return 0; }
    m->held = true;
    return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t m) { assert(m->held); m->held = false; }
void vSemaphoreDelete(SemaphoreHandle_t m) { assert(!m->held); }
int xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                unsigned priority, TaskHandle_t *task)
{
    (void)name; (void)stack; (void)arg; (void)priority;
    created_task = fn;
    *task = (void *)1;
    return task_fail ? 0 : pdPASS;
}
void vTaskDelete(TaskHandle_t task) { assert(task); created_task = NULL; }
esp_err_t tinyusb_driver_install(const tinyusb_config_t *config)
{
    assert(config->device_descriptor && config->configuration_descriptor);
    return install_fail ? ESP_FAIL : ESP_OK;
}
const char *rw_config_device_id(void) { return "0123456789abcdef"; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "mock"; }
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);
    if (size) { size_t copy = n < size - 1 ? n : size - 1;
        memcpy(dst, src, copy); dst[copy] = 0; }
    return n;
}
bool tud_mounted(void) { return mounted; }
bool tud_suspended(void) { return suspended; }
bool tud_remote_wakeup(void)
{
    if (remote_allowed && resume_at_us == 0) resume_at_us = now_us + 70000;
    return remote_allowed;
}
bool tud_hid_ready(void)
{
    return mounted && !suspended && !pending && now_us >= ready_at_us;
}
bool tud_disconnect(void)
{
    ++disconnect_count;
    mounted = false;
    pending = false;
    memset(held_report, 0, sizeof(held_report)); /* host removes keyboard */
    tud_umount_cb();
    return true;
}
bool tud_connect(void)
{
    if (auto_reenumerate) {
        mounted = true;
        tud_mount_cb();
    }
    return true;
}
bool tud_hid_keyboard_report(uint8_t id, uint8_t modifier, uint8_t keys[6])
{
    assert(tud_hid_ready());
    uint8_t report[9] = {id, modifier, 0};
    memcpy(report + 3, keys, 6);
    if (neutral(report) && ndown && reject_neutral) {
        --reject_neutral;
        return false;
    }
    assert(nsubmit < 2048);
    memcpy(submitted[nsubmit++], report, 9);
    memcpy(pending_report, report, 9);
    pending = true;
    due_us = now_us + report_latency_us;
    if (nsubmit == stall_transfer_at) due_us = INT64_MAX;
    if (!neutral(report)) {
        ++ndown;
        if (ndown == stall_down_at) due_us = INT64_MAX;
        if (delayed_down_us) due_us = now_us + delayed_down_us;
        if (ndown == disconnect_down_at) {
            mounted = false; pending = false; tud_umount_cb();
        }
        if (ndown == quick_reset_down_at) {
            mounted = false; pending = false; tud_umount_cb();
            mounted = true; tud_mount_cb(); /* neither state is polled */
        }
        if (ndown == suspend_down_at) { suspended = true; tud_suspend_cb(true); }
    }
    return true;
}

static void reset(void)
{
    assert(!mutex.held);
    now_us = due_us = ready_at_us = resume_at_us = 0;
    mounted = true; suspended = false; remote_allowed = true; pending = false;
    memset(held_report, 0, sizeof(held_report));
    memset(submitted, 0, sizeof(submitted));
    memset(completed, 0, sizeof(completed));
    nsubmit = ncomplete = ndown = reject_neutral = short_transfer_at = 0;
    failed_transfer_at = 0;
    stall_down_at = disconnect_down_at = suspend_down_at = 0;
    quick_reset_down_at = disconnect_count = 0; auto_reenumerate = true;
    stall_transfer_at = 0;
    report_latency_us = 10000; delayed_down_us = 0;
    s_need_release = true;
    s_waiting_report = s_report_complete = s_neutral_completed = false;
    s_action_deadline = 0;
    s_action_epoch_valid = false;
    s_reenumerating = false;
    tud_mount_cb();
}

static cJSON chord(const char **names, int count, cJSON *items)
{
    for (int i = 0; i < count; ++i) {
        items[i] = (cJSON){.type = cJSON_String, .valuestring = (char *)names[i],
            .next = i + 1 < count ? &items[i + 1] : NULL};
    }
    return (cJSON){.type = cJSON_Array, .child = items};
}

static void test_init(void)
{
    size_t descriptor_len = 0;
    assert(rw_hid_report_descriptor(&descriptor_len) == s_hid_report_desc);
    assert(descriptor_len == sizeof(s_hid_report_desc));
#ifdef TEST_EXTERNAL_INIT
    /* Installing must never be attempted, even when the mock would fail. */
    install_fail = true;
    mutex_fail = true;
    assert(rw_hid_init_external() == ESP_ERR_NO_MEM);
    mutex_fail = false;
    task_fail = true;
    assert(rw_hid_init_external() == ESP_ERR_NO_MEM);
    task_fail = false;
    assert(rw_hid_init_external() == ESP_OK);
    assert(created_task == recovery_task && rw_hid_available());
    assert(rw_hid_init_external() == ESP_OK);
    return;
#endif
    mutex_fail = true;
    assert(rw_hid_init() == ESP_ERR_NO_MEM);
    mutex_fail = false; task_fail = true;
    assert(rw_hid_init() == ESP_ERR_NO_MEM);
    task_fail = false; install_fail = true;
    assert(rw_hid_init() == ESP_FAIL);
    assert(!created_task && !rw_hid_available());
    install_fail = false;
    assert(rw_hid_init() == ESP_OK);
    assert(created_task == recovery_task && rw_hid_available());
    assert(rw_hid_init() == ESP_OK);
}

static void test_typing(void)
{
    reset();
    assert(rw_hid_type("aaA11!", true) == ESP_OK);
    assert(nsubmit == 15 && ncomplete == nsubmit && !s_need_release);
    assert(neutral(completed[0]));
    for (int i = 1; i < ncomplete; ++i) assert(neutral(completed[i]) == !(i % 2));
    assert(completed[1][3] == HID_KEY_A && completed[3][3] == HID_KEY_A);
    assert(completed[5][1] == KEYBOARD_MODIFIER_LEFTSHIFT);
    assert(completed[11][1] == KEYBOARD_MODIFIER_LEFTSHIFT);
    assert(completed[13][3] == HID_KEY_ENTER && neutral(held_report));
    reset();
    char max[RW_TYPE_MAX_CHARS + 1]; memset(max, 'a', sizeof(max) - 1); max[256] = 0;
    assert(rw_hid_type(max, true) == ESP_OK);
    assert(ndown == 257 && nsubmit == 515 && now_us < 6000000);
}

static void test_validation(void)
{
    reset();
    assert(rw_hid_type(NULL, false) == ESP_ERR_INVALID_ARG);
    assert(rw_hid_type("aa\x80", true) == ESP_ERR_INVALID_ARG);
    assert(rw_hid_type("a\x01", true) == ESP_ERR_INVALID_ARG);
    char long_text[258]; memset(long_text, 'a', 257); long_text[257] = 0;
    assert(rw_hid_type(long_text, false) == ESP_ERR_INVALID_SIZE);
    uint8_t key = 0, mod = 0;
    const char *bad[] = {"F1garbage", "F12x", "F01", "F0", "F13", "F999999999999", "F+1"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(!lookup_keyname(bad[i], &key, &mod));
    }
    assert(lookup_keyname("f12", &key, &mod) && key == HID_KEY_F1 + 11);
    cJSON keys1[1], keys2[1];
    const char *a[] = {"a"}, *bad_key[] = {"F1garbage"};
    cJSON chords[2] = {chord(a, 1, keys1), chord(bad_key, 1, keys2)};
    chords[0].next = &chords[1];
    cJSON seq = {.type = cJSON_Array, .child = chords};
    assert(rw_hid_keys(&seq) == ESP_ERR_INVALID_ARG);
    assert(nsubmit == 0); /* validation of later chords precedes all effects */
    assert(rw_hid_keys(NULL) == ESP_ERR_INVALID_ARG);
    seq.child = NULL;
    assert(rw_hid_keys(&seq) == ESP_ERR_INVALID_SIZE);
    const char *seven[] = {"a", "b", "1", "a", "b", "1", "a"};
    cJSON seven_items[7], seven_chord = chord(seven, 7, seven_items);
    seq.child = &seven_chord;
    assert(rw_hid_keys(&seq) == ESP_ERR_INVALID_SIZE && nsubmit == 0);
}

static void test_keys_wake(void)
{
    reset();
    const char *names[] = {"LGUI", "L"}, *f12[] = {"f12"};
    cJSON items1[2], items2[1];
    cJSON chords[2] = {chord(names, 2, items1), chord(f12, 1, items2)};
    chords[0].next = &chords[1];
    cJSON seq = {.type = cJSON_Array, .child = chords};
    assert(rw_hid_keys(&seq) == ESP_OK);
    assert(nsubmit == 5 && neutral(held_report));
    assert(completed[1][1] == KEYBOARD_MODIFIER_LEFTGUI);
    assert(completed[1][3] == HID_KEY_A + 11); /* Win+L, no unintended Shift */
    assert(completed[3][3] == HID_KEY_F1 + 11);
    reset(); suspended = true;
    assert(rw_hid_wake() == ESP_OK);
    assert(ndown == 2 && ncomplete == 5 && neutral(held_report));
    reset(); suspended = true; remote_allowed = false;
    assert(rw_hid_wake() == ESP_ERR_TIMEOUT && nsubmit == 0);
    reset(); suspended = true; resume_at_us = INT64_MAX;
    assert(rw_hid_wake() == ESP_ERR_TIMEOUT && now_us == 1000000 && nsubmit == 0);
}

static void test_failed_release(void)
{
    reset(); reject_neutral = 2;
    assert(rw_hid_type("ab", true) == ESP_FAIL);
    assert(ndown == 1 && s_need_release && disconnect_count == 1);
    assert(neutral(held_report)); /* failed cleanup detaches held host key */
    recovery_once(); /* actual recovery worker body, no subsequent command */
    assert(!s_need_release && neutral(held_report) && ndown == 1);
    reset(); short_transfer_at = 3; /* first key-up has a short transfer */
    assert(rw_hid_type("ab", true) == ESP_FAIL);
    assert(ndown == 1 && nsubmit == 4 && neutral(held_report));
    reset(); failed_transfer_at = 2; /* uncertain key-down must never replay */
    assert(rw_hid_type("ab", true) == ESP_FAIL);
    assert(ndown == 1 && nsubmit == 3 && neutral(held_report));
    reset(); failed_transfer_at = 3; /* failed key-up, full len despite failure */
    assert(rw_hid_type("ab", true) == ESP_FAIL);
    assert(ndown == 1 && nsubmit == 4 && neutral(held_report));
    reset(); stall_transfer_at = 3; /* host saw down, accepted release never completes */
    assert(rw_hid_type("ab", true) == ESP_ERR_TIMEOUT);
    assert(ndown == 1 && disconnect_count == 1 && neutral(held_report));
    assert(now_us <= 480000);
    recovery_once();
    assert(ndown == 1 && !s_need_release && neutral(held_report));
}

static void test_stalls_and_deadlines(void)
{
    reset(); ready_at_us = 50000;
    assert(rw_hid_type("a", false) == ESP_OK && now_us >= 80000);
    reset(); ready_at_us = INT64_MAX;
    assert(rw_hid_type("a", true) == ESP_ERR_TIMEOUT);
    assert(now_us >= 200000 && now_us <= 460000 && ndown == 0);
    reset(); delayed_down_us = 350000;
    assert(rw_hid_type("ab", true) == ESP_ERR_TIMEOUT);
    assert(ndown == 1 && neutral(held_report) && now_us <= 420000);
    reset(); stall_down_at = 1;
    assert(rw_hid_type("ab", true) == ESP_ERR_TIMEOUT && s_need_release);
    assert(now_us <= 470000 && ndown == 1 && disconnect_count == 1);
    assert(neutral(held_report));
    recovery_once();
    assert(ndown == 1 && !s_need_release && neutral(held_report));
    reset(); report_latency_us = 190000;
    char max[257]; memset(max, 'a', 256); max[256] = 0;
    assert(rw_hid_type(max, true) == ESP_ERR_TIMEOUT);
    assert(now_us >= 8000000 && now_us <= 8210000 && ndown < 256);
    assert(neutral(held_report) && !s_need_release);
}

static void test_disconnect_resume_and_ownership(void)
{
    reset(); disconnect_down_at = 1;
    assert(rw_hid_type("ab", true) == ESP_ERR_NOT_FOUND && s_need_release);
    assert(ndown == 1);
    recovery_once(); /* observe unplug before re-enumeration */
    mounted = true; disconnect_down_at = 0;
    tud_mount_cb();
    recovery_once();
    assert(neutral(held_report) && !s_need_release && ndown == 1);
    int baseline = nsubmit;
    assert(rw_hid_type("b", false) == ESP_OK);
    assert(neutral(submitted[baseline]));
    reset(); suspend_down_at = 1;
    assert(rw_hid_type("ab", true) == ESP_ERR_TIMEOUT && s_need_release);
    recovery_once();
    suspended = false; suspend_down_at = 0;
    tud_resume_cb();
    recovery_once();
    assert(neutral(held_report) && ndown == 1 && !s_need_release);
    reset(); quick_reset_down_at = 1;
    assert(rw_hid_type("ab", true) == ESP_ERR_TIMEOUT);
    assert(ndown == 1 && neutral(held_report));
    assert(rw_hid_type("b", false) == ESP_OK && ndown == 2);
    reset(); stall_down_at = 1; auto_reenumerate = false;
    assert(rw_hid_type("ab", true) == ESP_ERR_TIMEOUT && s_reenumerating);
    int submitted_before = nsubmit;
    assert(rw_hid_type("b", false) == ESP_ERR_NOT_FOUND);
    assert(nsubmit == submitted_before && ndown == 1);
    mounted = true; tud_mount_cb(); /* permit input only after actual configure */
    assert(rw_hid_type("b", false) == ESP_OK && ndown == 2);
    reset(); mutex.held = true;
    assert(rw_hid_type("a", true) == ESP_ERR_INVALID_STATE && nsubmit == 0);
    recovery_once();
    assert(nsubmit == 0 && mutex.held);
    mutex.held = false;
}

static void test_maintenance_quiesce(void)
{
    reset();
    mutex.held = true;
    assert(rw_hid_quiesce() == ESP_ERR_INVALID_STATE && nsubmit == 0);
    mutex.held = false;
    held_report[3] = HID_KEY_A;
    assert(rw_hid_quiesce() == ESP_OK);
    assert(neutral(held_report));
    int before = nsubmit;
    assert(rw_hid_type("a", false) == ESP_ERR_INVALID_STATE);
    assert(rw_hid_wake() == ESP_ERR_INVALID_STATE);
    recovery_once();
    assert(nsubmit == before);
    assert(rw_hid_quiesce() == ESP_ERR_INVALID_STATE);
    rw_hid_resume_actions();
    assert(rw_hid_type("a", false) == ESP_OK);
    reset(); suspended = true;
    assert(rw_hid_quiesce() == ESP_OK && nsubmit == 0);
    rw_hid_resume_actions();
    reset(); stall_transfer_at = 1;
    assert(rw_hid_quiesce() == ESP_ERR_TIMEOUT && !s_quiesced);
    assert(now_us <= 300000);
}

int main(void)
{
    test_init();
    test_typing();
    test_validation();
    test_keys_wake();
    test_failed_release();
    test_stalls_and_deadlines();
    test_disconnect_resume_and_ownership();
    test_maintenance_quiesce();
    printf("HID production-C fault injection passed at %d Hz\n", CONFIG_FREERTOS_HZ);
}
