/* No GPIO/RTOS mocks: disabled power must not depend on or touch them. */
#include <assert.h>
#include "../../main/power_btn.c"
int main(void)
{
    assert(rw_power_btn_init() == ESP_OK);
    assert(rw_power_btn_init() == ESP_OK);
    assert(!rw_power_busy());
    assert(rw_power_tap(RW_TAP_MS_DEF) == ESP_ERR_NOT_SUPPORTED);
    assert(rw_power_hold(RW_HOLD_MS_DEF) == ESP_ERR_NOT_SUPPORTED);
    assert(rw_power_tap(0) == ESP_ERR_NOT_SUPPORTED);
    assert(!rw_power_busy());
}
