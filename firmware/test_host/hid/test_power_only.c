#include <assert.h>
#include "../../main/usb_hid.c"
int main(void)
{
    assert(rw_hid_init() == ESP_OK);
    assert(!rw_hid_available() && !rw_hid_mounted() && !rw_hid_suspended());
    assert(rw_hid_wake() == ESP_ERR_NOT_SUPPORTED);
    assert(rw_hid_type("a", true) == ESP_ERR_NOT_SUPPORTED);
    assert(rw_hid_keys(NULL) == ESP_ERR_NOT_SUPPORTED);
}
