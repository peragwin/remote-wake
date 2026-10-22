#pragma once
#include "esp_err.h"
typedef struct { int unused; } esp_qrcode_config_t;
#define ESP_QRCODE_CONFIG_DEFAULT() ((esp_qrcode_config_t){0})
esp_err_t esp_qrcode_generate(esp_qrcode_config_t *cfg, const char *text);
