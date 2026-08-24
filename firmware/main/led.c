#include "led.h"

#include "sdkconfig.h"

#if !CONFIG_RW_LED_ENABLED

esp_err_t rw_led_init(void) { return ESP_OK; }
void rw_led_set(rw_led_state_t s) { (void)s; }
void rw_led_flash(rw_led_state_t s, uint32_t ms) { (void)s; (void)ms; }

#else

#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "led_strip_rmt.h"

static const char *TAG = "rw.led";

#define TICK_MS   20
#define PEAK      CONFIG_RW_LED_MAX_BRIGHTNESS

static led_strip_handle_t s_strip;
static volatile rw_led_state_t s_state = RW_LED_BOOT;
static volatile rw_led_state_t s_flash_state = RW_LED_OFF;
static volatile int64_t        s_flash_until_us;

static inline uint8_t scale(uint8_t base, float k)
{
    float v = (float)base * k;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

/* Compute the RGB for @p st at animation phase @p t (seconds since boot). */
static void pattern(rw_led_state_t st, double t, uint8_t *r, uint8_t *g,
                    uint8_t *b)
{
    *r = *g = *b = 0;
    switch (st) {
    case RW_LED_OFF:
        break;
    case RW_LED_BOOT:
        *r = *g = *b = scale(PEAK, 0.35f);
        break;
    case RW_LED_SETUP: {
        /* Breathing: a raised cosine reads much smoother than a triangle. */
        float k = 0.08f + 0.92f * (float)((1.0 - cos(t * 2.0 * M_PI / 3.0)) / 2.0);
        *g = scale(PEAK, k * 0.75f);
        *b = scale(PEAK, k);
        break;
    }
    case RW_LED_CONNECTING: {
        float k = 0.15f + 0.85f * (float)((1.0 - cos(t * 2.0 * M_PI / 1.2)) / 2.0);
        *r = scale(PEAK, k);
        *g = scale(PEAK, k * 0.45f);
        break;
    }
    case RW_LED_CONNECTED:
        /* Deliberately faint: this thing lives behind a PC 24/7. */
        *g = scale(PEAK, 0.18f);
        break;
    case RW_LED_BUSY:
        if (fmod(t, 0.24) < 0.12) {
            *g = scale(PEAK, 1.0f);
            *b = scale(PEAK, 0.35f);
        }
        break;
    case RW_LED_LOCKOUT:
        *r = scale(PEAK, 1.0f);
        break;
    case RW_LED_FAULT:
        if (fmod(t, 1.6) < 0.25) {
            *r = scale(PEAK, 0.8f);
        }
        break;
    }
}

static void led_task(void *arg)
{
    (void)arg;
    while (true) {
        int64_t now = esp_timer_get_time();
        rw_led_state_t st = s_state;
        if (now < s_flash_until_us) {
            st = s_flash_state;
        }
        double t = (double)now / 1000000.0;

        uint8_t r, g, b;
        pattern(st, t, &r, &g, &b);
        led_strip_set_pixel(s_strip, 0, r, g, b);
        led_strip_refresh(s_strip);

        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

esp_err_t rw_led_init(void)
{
    if (s_strip) {
        return ESP_OK;
    }
    led_strip_config_t strip_cfg = {
        .strip_gpio_num   = CONFIG_RW_LED_GPIO,
        .max_leds         = 1,
        .led_model        = LED_MODEL_WS2812,
        /* GRB ordering is the WS2812 default; the explicit
         * color_component_format field only exists in led_strip >= 3.0. */
        .flags = {
            .invert_out = false,
        },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags = {
            .with_dma = false,
        },
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "led_strip init failed: %s — continuing without status LED",
                 esp_err_to_name(err));
        s_strip = NULL;
        return err;
    }
    led_strip_clear(s_strip);

    if (xTaskCreate(led_task, "rw_led", 2560, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "status LED on GPIO%d", CONFIG_RW_LED_GPIO);
    return ESP_OK;
}

void rw_led_set(rw_led_state_t state)
{
    s_state = state;
}

void rw_led_flash(rw_led_state_t state, uint32_t ms)
{
    s_flash_state    = state;
    s_flash_until_us = esp_timer_get_time() + (int64_t)ms * 1000;
}

#endif /* CONFIG_RW_LED_ENABLED */
