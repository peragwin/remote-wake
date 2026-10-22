#include "maintenance.h"
#include "sdkconfig.h"
#include "esp_idf_version.h"

#if CONFIG_IDF_TARGET_ESP32S3 && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0) && \
    ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 0)
#include <stdatomic.h>
#include "usb_hid.h"
#include "power_btn.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_private/periph_ctrl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "hal/clk_gate_ll.h"
#include "soc/periph_defs.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "soc/usb_pins.h"
#include "esp32s3/rom/usb/chip_usb_dw_wrapper.h"
#include "tinyusb.h"

static atomic_bool s_pending;
static atomic_bool s_download_ready;
static const char *TAG = "rw.maintenance";

bool rw_maintenance_bootloader_supported(void)
{
#if CONFIG_TINYUSB_NO_DEFAULT_TASK
    return false;
#else
    return !esp_efuse_read_field_bit(ESP_EFUSE_DIS_DOWNLOAD_MODE) &&
           !esp_efuse_read_field_bit(ESP_EFUSE_DIS_USB_SERIAL_JTAG) &&
           !esp_efuse_read_field_bit(ESP_EFUSE_DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE) &&
           !esp_efuse_read_field_bit(ESP_EFUSE_USB_PHY_SEL);
#endif
}

/* Set at shutdown, after esp_restart() has begun, so an unrelated earlier
 * restart cannot accidentally select ROM. Do not persist the application USB
 * descriptors: the ROM exposes the fixed hardware USB Serial/JTAG device. */
static void IRAM_ATTR download_shutdown(void)
{
    if (s_download_ready) REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
}

static void handoff_usb(void)
{
    if (tud_inited()) {
        tud_disconnect();
        vTaskDelay(pdMS_TO_TICKS(30));
        esp_err_t err = tinyusb_driver_uninstall();
        if (err != ESP_OK) {
            /* Do not run on into a mux change while the USB task may run. */
            ESP_LOGE(TAG, "USB teardown failed (%s); restarting normally", esp_err_to_name(err));
            esp_unregister_shutdown_handler(download_shutdown);
            esp_restart();
        }
    }
    /* Match Espressif Arduino's S3 usb_switch_to_cdc_jtag handoff, using
     * IDF 5.4 LL APIs. A low interval ensures the host sees a disconnect. */
    PERIPH_RCC_ATOMIC() {
        periph_ll_reset(PERIPH_USB_MODULE);
        periph_ll_disable_clk_set_rst(PERIPH_USB_MODULE);
        usb_serial_jtag_ll_enable_bus_clock(true);
    }
    usb_serial_jtag_ll_phy_enable_pad(false);
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG,
                       RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
    USB_SERIAL_JTAG.conf0.phy_sel = 0;
    gpio_config_t pins = {
        .pin_bit_mask = (1ULL << USBPHY_DM_NUM) | (1ULL << USBPHY_DP_NUM),
        .mode = GPIO_MODE_OUTPUT_OD,
    };
    gpio_config(&pins);
    gpio_set_level(USBPHY_DM_NUM, 0);
    gpio_set_level(USBPHY_DP_NUM, 0);
    vTaskDelay(pdMS_TO_TICKS(30));
    const usb_serial_jtag_pull_override_vals_t pulls = {.dp_pu = 1};
    usb_serial_jtag_ll_phy_enable_pull_override(&pulls);
    usb_serial_jtag_ll_phy_disable_pull_override();
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
    usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
    usb_serial_jtag_ll_phy_enable_pad(true);
    /* Bounded host re-enumeration wait, without an ISR/allocation dependency.
     * ROM will enumerate on its own even if the cable/host is absent. */
    for (unsigned i = 0; i < 100 && !USB_SERIAL_JTAG.int_raw.usb_bus_reset_int_raw; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    chip_usb_set_persist_flags(0);
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(RW_MAINTENANCE_ACK_GRACE_MS));
    handoff_usb();
    s_download_ready = true;
    esp_restart();
}

esp_err_t rw_maintenance_enter_bootloader(void)
{
    if (!rw_maintenance_bootloader_supported()) return ESP_ERR_NOT_SUPPORTED;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_pending, &expected, true)) return ESP_ERR_INVALID_STATE;
    esp_err_t err = rw_power_btn_quiesce();
    if (err != ESP_OK) {
        s_pending = false;
        return err;
    }
    err = rw_hid_quiesce();
    if (err != ESP_OK) {
        rw_power_btn_resume_actions();
        s_pending = false;
        return err;
    }
    err = esp_register_shutdown_handler(download_shutdown);
    if (err == ESP_OK) {
        if (xTaskCreate(reboot_task, "rw-rom", 4096, NULL, 5, NULL) == pdPASS) return ESP_OK;
        esp_unregister_shutdown_handler(download_shutdown);
        err = ESP_ERR_NO_MEM;
    }
    rw_hid_resume_actions();
    rw_power_btn_resume_actions();
    s_pending = false;
    return err;
}
#else
bool rw_maintenance_bootloader_supported(void) { return false; }
esp_err_t rw_maintenance_enter_bootloader(void) { return ESP_ERR_NOT_SUPPORTED; }
#endif
