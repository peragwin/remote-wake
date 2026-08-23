#include "power_btn.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "rw.pwr";

#define PWR_GPIO ((gpio_num_t)CONFIG_RW_PWR_GPIO)

static SemaphoreHandle_t s_lock;
static volatile bool     s_busy;

static void release_pin(void)
{
    /* Belt and braces: drive low, then let the pull-down hold it there. */
    gpio_set_level(PWR_GPIO, 0);
}

static void shutdown_handler(void)
{
    release_pin();
}

esp_err_t rw_power_btn_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }

    /* Order matters: pull the line down *before* enabling the output driver so
     * the opto never sees a glitch from the ROM bootloader's pin state. */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_RW_PWR_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(PWR_GPIO, 0);
    /* Survive light sleep / deep sleep without floating. */
    gpio_hold_dis(PWR_GPIO);

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    esp_register_shutdown_handler(shutdown_handler);
    ESP_LOGI(TAG, "PWR_SW on GPIO%d, idle low", CONFIG_RW_PWR_GPIO);
    return ESP_OK;
}

static esp_err_t pulse(uint32_t ms, uint32_t lo, uint32_t hi)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ms < lo || ms > hi) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE; /* busy */
    }

    s_busy = true;
    ESP_LOGI(TAG, "PWR_SW high for %u ms", (unsigned)ms);
    gpio_set_level(PWR_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    release_pin();
    s_busy = false;

    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t rw_power_tap(uint32_t ms)
{
    return pulse(ms, RW_TAP_MS_MIN, RW_TAP_MS_MAX);
}

esp_err_t rw_power_hold(uint32_t ms)
{
    return pulse(ms, RW_HOLD_MS_MIN, RW_HOLD_MS_MAX);
}

bool rw_power_busy(void)
{
    return s_busy;
}
