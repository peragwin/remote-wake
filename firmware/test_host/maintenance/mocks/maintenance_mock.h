#pragma once
#include "mock_sdk.h"
#define IRAM_ATTR
#define CONFIG_RW_PWR_GPIO 5
#define RTC_CNTL_OPTION1_REG 1
#define RTC_CNTL_USB_CONF_REG 0
#define RTC_CNTL_FORCE_DOWNLOAD_BOOT 1
#define RTC_CNTL_SW_HW_USB_PHY_SEL 2
#define RTC_CNTL_SW_USB_PHY_SEL 4
#define RTC_CNTL_USB_PAD_ENABLE 8
#define USB_SERIAL_JTAG_LL_INTR_MASK 0xffff
#define PERIPH_USB_MODULE 1
void periph_ll_reset(unsigned module);
void periph_ll_disable_clk_set_rst(unsigned module);
#define USBPHY_DM_NUM 19
#define USBPHY_DP_NUM 20
#define GPIO_MODE_OUTPUT_OD 1
#define GPIO_MODE_OUTPUT 2
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_ENABLE 1
#define GPIO_INTR_DISABLE 0
#define PERIPH_RCC_ATOMIC() for (int once = 1; once; once = 0)
typedef int gpio_num_t;
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
typedef struct { unsigned dp_pu, dm_pu, dp_pd, dm_pd; } usb_serial_jtag_pull_override_vals_t;
static struct { struct { unsigned phy_sel; } conf0; struct { unsigned usb_bus_reset_int_raw; } int_raw; } USB_SERIAL_JTAG;
extern unsigned registers[2];
#define REG_WRITE(reg,val) (registers[reg] = (val))
#define CLEAR_PERI_REG_MASK(reg,mask) (registers[reg] &= ~(mask))
#define ESP_EFUSE_DIS_DOWNLOAD_MODE 0
#define ESP_EFUSE_DIS_USB_SERIAL_JTAG 1
#define ESP_EFUSE_DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE 2
#define ESP_EFUSE_USB_PHY_SEL 3
bool esp_efuse_read_field_bit(unsigned field);
esp_err_t esp_register_shutdown_handler(void (*fn)(void));
esp_err_t esp_unregister_shutdown_handler(void (*fn)(void));
void esp_restart(void);
bool tud_inited(void);
esp_err_t tinyusb_driver_uninstall(void);
void usb_serial_jtag_ll_enable_bus_clock(bool enable);
void usb_serial_jtag_ll_phy_enable_pad(bool enable);
void usb_serial_jtag_ll_phy_enable_pull_override(const usb_serial_jtag_pull_override_vals_t *pulls);
void usb_serial_jtag_ll_phy_disable_pull_override(void);
void usb_serial_jtag_ll_disable_intr_mask(unsigned mask);
void usb_serial_jtag_ll_clr_intsts_mask(unsigned mask);
esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(gpio_num_t gpio, unsigned level);
void gpio_hold_dis(gpio_num_t gpio);
void chip_usb_set_persist_flags(unsigned flags);
