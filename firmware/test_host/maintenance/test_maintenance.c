#include "maintenance_mock.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include "../../main/maintenance.c"
unsigned registers[2];
static bool fuses[4], power_reserved, hid_reserved, allocation_fail, usb_initialized;
static esp_err_t power_error, hid_error, registration_error, teardown_error;
static void (*shutdown_fn)(void), (*worker_fn)(void *);
static unsigned elapsed, pin_writes, uninstalls, detachments, persist_flags;
static jmp_buf reboot;

static void reset(void)
{
    s_pending = s_download_ready = false;
    power_reserved = hid_reserved = allocation_fail = false;
    power_error = hid_error = registration_error = teardown_error = ESP_OK;
    for (unsigned i = 0; i < 4; ++i) fuses[i] = false;
    registers[0] = 0xffff; registers[1] = 0;
    USB_SERIAL_JTAG.int_raw.usb_bus_reset_int_raw = 1;
    elapsed = pin_writes = uninstalls = detachments = 0;
    persist_flags = 99;
    shutdown_fn = NULL; worker_fn = NULL; usb_initialized = true;
}
static void admissions(void)
{
    for (unsigned i = 0; i < 4; ++i) {
        reset(); fuses[i] = true;
        assert(!rw_maintenance_bootloader_supported());
        assert(rw_maintenance_enter_bootloader() == ESP_ERR_NOT_SUPPORTED);
        assert(!worker_fn && !power_reserved && !pin_writes);
    }
    reset(); power_error = ESP_ERR_INVALID_STATE;
    assert(rw_maintenance_enter_bootloader() == ESP_ERR_INVALID_STATE && !hid_reserved);
    reset(); hid_error = ESP_ERR_TIMEOUT;
    assert(rw_maintenance_enter_bootloader() == ESP_ERR_TIMEOUT && !power_reserved);
    reset(); registration_error = ESP_FAIL;
    assert(rw_maintenance_enter_bootloader() == ESP_FAIL);
    assert(!power_reserved && !hid_reserved && !pin_writes);
    reset(); allocation_fail = true;
    assert(rw_maintenance_enter_bootloader() == ESP_ERR_NO_MEM);
    assert(!shutdown_fn && !power_reserved && !hid_reserved && !s_pending);
}
static void accepted(bool installed, bool fail_teardown)
{
    reset(); usb_initialized = installed;
    if (!installed) USB_SERIAL_JTAG.int_raw.usb_bus_reset_int_raw = 0;
    teardown_error = fail_teardown ? ESP_FAIL : ESP_OK;
    assert(rw_maintenance_enter_bootloader() == ESP_OK);
    assert(power_reserved && hid_reserved && worker_fn && shutdown_fn);
    assert(!pin_writes && !detachments && !elapsed); /* ack still possible */
    assert(rw_maintenance_enter_bootloader() == ESP_ERR_INVALID_STATE);
    shutdown_fn(); /* an unrelated restart during grace must remain normal */
    assert(registers[1] == 0);
    if (!setjmp(reboot)) { worker_fn(NULL); assert(false); }
    assert(elapsed >= RW_MAINTENANCE_ACK_GRACE_MS);
    assert(uninstalls == (unsigned)installed && detachments == (unsigned)installed);
    if (fail_teardown) {
        assert(!pin_writes && !registers[1] && !shutdown_fn);
    } else {
        assert(registers[1] == RTC_CNTL_FORCE_DOWNLOAD_BOOT && persist_flags == 0);
        assert(pin_writes == 2 && USB_SERIAL_JTAG.conf0.phy_sel == 0);
        if (!installed) assert(elapsed >= RW_MAINTENANCE_ACK_GRACE_MS + 1000);
    }
}
int main(void)
{
    admissions(); accepted(true, false); accepted(false, false); accepted(true, true);
    puts("maintenance admission and ROM handoff tests passed");
}
bool esp_efuse_read_field_bit(unsigned field) { return fuses[field]; }
esp_err_t rw_power_btn_quiesce(void) { if (!power_error) power_reserved = true; return power_error; }
void rw_power_btn_resume_actions(void) { power_reserved = false; }
esp_err_t rw_hid_quiesce(void) { if (!hid_error) hid_reserved = true; return hid_error; }
void rw_hid_resume_actions(void) { hid_reserved = false; }
esp_err_t esp_register_shutdown_handler(void (*fn)(void)) { if (!registration_error) shutdown_fn = fn; return registration_error; }
esp_err_t esp_unregister_shutdown_handler(void (*fn)(void)) { assert(shutdown_fn == fn); shutdown_fn = NULL; return ESP_OK; }
int xTaskCreate(void (*fn)(void *), const char *n, uint32_t stack, void *arg, unsigned priority, TaskHandle_t *t)
{ (void)n; (void)stack; (void)arg; (void)priority; (void)t; if (allocation_fail) return 0; worker_fn = fn; return pdPASS; }
void vTaskDelay(TickType_t ticks) { elapsed += ticks * 1000 / CONFIG_FREERTOS_HZ; }
void esp_restart(void) { if (shutdown_fn) shutdown_fn(); longjmp(reboot, 1); }
bool tud_inited(void) { return usb_initialized; }
bool tud_disconnect(void) { ++detachments; assert(elapsed >= 500); return true; }
esp_err_t tinyusb_driver_uninstall(void) { ++uninstalls; return teardown_error; }
void usb_serial_jtag_ll_enable_bus_clock(bool e) { assert(e); }
void usb_serial_jtag_ll_phy_enable_pad(bool e) { (void)e; }
void usb_serial_jtag_ll_phy_enable_pull_override(const usb_serial_jtag_pull_override_vals_t *p) { assert(p->dp_pu && !p->dm_pu); }
void usb_serial_jtag_ll_phy_disable_pull_override(void) {}
void usb_serial_jtag_ll_disable_intr_mask(unsigned m) { (void)m; }
void usb_serial_jtag_ll_clr_intsts_mask(unsigned m) { (void)m; }
esp_err_t gpio_config(const gpio_config_t *c) { assert(c->pin_bit_mask == ((1ULL << 19) | (1ULL << 20))); return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t p, unsigned level) { assert((p == 19 || p == 20) && !level); ++pin_writes; return ESP_OK; }
void chip_usb_set_persist_flags(unsigned f) { persist_flags = f; }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "mock"; }

void periph_ll_reset(unsigned module) { assert(module == PERIPH_USB_MODULE); }
void periph_ll_disable_clk_set_rst(unsigned module) { assert(module == PERIPH_USB_MODULE); }
