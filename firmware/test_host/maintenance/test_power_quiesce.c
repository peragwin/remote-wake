#include "maintenance_mock.h"
#include <assert.h>
#include "../../main/power_btn.c"
struct mock_mutex { bool held; } mutex;
static unsigned pin_writes, delays;
static void (*shutdown_fn)(void);
int main(void)
{
    assert(rw_power_btn_init() == ESP_OK && shutdown_fn);
    unsigned before = pin_writes;
    assert(rw_power_btn_quiesce() == ESP_OK);
    assert(rw_power_btn_quiesce() == ESP_ERR_INVALID_STATE);
    assert(rw_power_tap(RW_TAP_MS_DEF) == ESP_ERR_INVALID_STATE);
    assert(rw_power_hold(RW_HOLD_MS_DEF) == ESP_ERR_INVALID_STATE);
    assert(pin_writes == before && delays == 0 && !rw_power_busy());
    rw_power_btn_resume_actions();
    assert(rw_power_tap(RW_TAP_MS_DEF) == ESP_OK && delays == 1);
    assert(pin_writes == before + 2 && !rw_power_busy());
    assert(rw_power_btn_quiesce() == ESP_OK);
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &mutex; }
int xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait)
{ (void)wait; if (m->held) return 0; m->held = true; return pdTRUE; }
void xSemaphoreGive(SemaphoreHandle_t m) { assert(m->held); m->held = false; }
void vTaskDelay(TickType_t ticks)
{
    assert(ticks && rw_power_busy());
    assert(rw_power_btn_quiesce() == ESP_ERR_INVALID_STATE); /* actual pulse owns lock */
    ++delays;
}
esp_err_t esp_register_shutdown_handler(void (*fn)(void)) { shutdown_fn = fn; return ESP_OK; }
esp_err_t gpio_config(const gpio_config_t *c) { assert(c->pin_bit_mask == (1ULL << 5)); return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t pin, unsigned level) { assert(pin == 5); (void)level; ++pin_writes; return ESP_OK; }
void gpio_hold_dis(gpio_num_t pin) { assert(pin == 5); }
