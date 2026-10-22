#include "ota_mock.h"
#include <assert.h>
#include "../../main/ota.c"
static esp_partition_t running={.type=ESP_PARTITION_TYPE_APP,.subtype=0};
static esp_ota_img_states_t state=ESP_OTA_IMG_PENDING_VERIFY;
static unsigned confirmed,rejected,seeded;
int main(void)
{
    rw_ota_registration_t registration;rw_ota_status_t status;
    assert(rw_ota_init(NULL)==ESP_ERR_NOT_SUPPORTED);
    assert(!rw_ota_get_registration(&registration));
    assert(rw_ota_offer_manifest("https://example.com/manifest.json")==ESP_ERR_NOT_SUPPORTED);
    rw_ota_get_status(&status);assert(status.phase==RW_OTA_DISABLED);
    assert(rw_ota_confirm_healthy()==ESP_OK && confirmed==0); /* factory */
    running.subtype=ESP_PARTITION_SUBTYPE_APP_OTA_0;
    assert(rw_ota_confirm_healthy()==ESP_OK && confirmed==1);
    state=ESP_OTA_IMG_UNDEFINED;
    assert(rw_ota_confirm_healthy()==ESP_OK && confirmed==2 && seeded==1); /* USB bootstrap */
    state=ESP_OTA_IMG_PENDING_VERIFY;
    assert(rw_ota_reject_unhealthy()==ESP_OK && rejected==1);
}
const esp_partition_t *esp_ota_get_running_partition(void){return &running;}
const esp_partition_t *esp_ota_get_boot_partition(void){return &running;}
esp_err_t esp_ota_get_state_partition(const esp_partition_t*p,esp_ota_img_states_t*s)
{assert(p==&running);if(!running.subtype||state==ESP_OTA_IMG_UNDEFINED)return ESP_ERR_NOT_FOUND;*s=state;return ESP_OK;}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t*p){assert(p==&running);state=ESP_OTA_IMG_NEW;++seeded;return ESP_OK;}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void){++confirmed;state=ESP_OTA_IMG_VALID;return ESP_OK;}
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void){++rejected;return ESP_OK;}
