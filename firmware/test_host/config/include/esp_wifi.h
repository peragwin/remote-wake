#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef enum { WIFI_MODE_AP = 2 } wifi_mode_t;
typedef enum { WIFI_IF_AP = 1 } wifi_interface_t;
#define WIFI_AUTH_WPA2_PSK 3
typedef struct {
    struct {
        uint8_t ssid[32], password[64], ssid_len, channel, max_connection;
        int authmode;
        struct { int required; } pmf_cfg;
    } ap;
} wifi_config_t;
esp_err_t esp_wifi_set_mode(wifi_mode_t mode);
esp_err_t esp_wifi_set_config(wifi_interface_t iface, wifi_config_t *conf);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
