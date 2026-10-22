#include "pc_sense.h"

#include "sdkconfig.h"

#if CONFIG_RW_PC_SENSE_ENABLED

#include <stdatomic.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#if CONFIG_RW_PWR_ENABLED && CONFIG_RW_PC_SENSE_GPIO == CONFIG_RW_PWR_GPIO
#error "CONFIG_RW_PC_SENSE_GPIO must differ from CONFIG_RW_PWR_GPIO"
#endif
#if CONFIG_RW_LED_ENABLED && CONFIG_RW_PC_SENSE_GPIO == CONFIG_RW_LED_GPIO
#error "CONFIG_RW_PC_SENSE_GPIO must differ from CONFIG_RW_LED_GPIO"
#endif
#if CONFIG_RW_PC_SENSE_GPIO == CONFIG_RW_BOOT_GPIO
#error "CONFIG_RW_PC_SENSE_GPIO must differ from CONFIG_RW_BOOT_GPIO"
#endif

static const char *TAG = "rw.pcsense";

#define SENSE_GPIO ((gpio_num_t)CONFIG_RW_PC_SENSE_GPIO)
#if CONFIG_RW_PC_SENSE_ACTIVE_LOW
#define SENSE_ACTIVE_LOW 1
#else
#define SENSE_ACTIVE_LOW 0
#endif

static rw_pc_classifier_t  s_cls;     /* owned by the timer callback */
static atomic_int          s_state = RW_PC_UNKNOWN;
static esp_timer_handle_t  s_timer;

static void sample(void *arg)
{
    (void)arg;
    bool lit = (gpio_get_level(SENSE_GPIO) != 0) != SENSE_ACTIVE_LOW;
    rw_pc_power_t next = rw_pc_classifier_step(&s_cls, lit,
                                               (uint64_t)esp_timer_get_time() / 1000u);
    if (atomic_exchange(&s_state, (int)next) != (int)next) {
        ESP_LOGI(TAG, "PC power: %s", rw_pc_power_name(next));
    }
}

esp_err_t rw_pc_sense_init(void)
{
    if (s_timer) {
        return ESP_OK;
    }
    /* Pull toward the inactive level so an unplugged sense lead reads "off". */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_RW_PC_SENSE_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = SENSE_ACTIVE_LOW ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = SENSE_ACTIVE_LOW ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d config failed: %s", CONFIG_RW_PC_SENSE_GPIO,
                 esp_err_to_name(err));
        return err;
    }
    rw_pc_classifier_init(&s_cls);
    const esp_timer_create_args_t args = {
        .callback        = sample,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "rw_pcsense",
    };
    esp_timer_handle_t timer = NULL;
    err = esp_timer_create(&args, &timer);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(timer, RW_PC_SAMPLE_MS * 1000u);
        if (err != ESP_OK) {
            esp_timer_delete(timer);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sampler start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_timer = timer;
    ESP_LOGI(TAG, "PC power LED sense on GPIO%d, active %s", CONFIG_RW_PC_SENSE_GPIO,
             SENSE_ACTIVE_LOW ? "low" : "high");
    return ESP_OK;
}

rw_pc_power_t rw_pc_sense_get(void)
{
    return (rw_pc_power_t)atomic_load(&s_state);
}

#else
esp_err_t rw_pc_sense_init(void) { return ESP_OK; }
rw_pc_power_t rw_pc_sense_get(void) { return RW_PC_UNKNOWN; }
#endif
