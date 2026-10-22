#pragma once
#include "esp_err.h"
esp_err_t app_ota_start(void);
esp_err_t app_ota_prepare_restart(void *ctx);
#include "ota.h"
void app_ota_progress(void *ctx, const rw_ota_status_t *status);
