#include <assert.h>
#include "../../main/maintenance.c"
int main(void)
{
    assert(!rw_maintenance_bootloader_supported());
    assert(rw_maintenance_enter_bootloader() == ESP_ERR_NOT_SUPPORTED);
}
