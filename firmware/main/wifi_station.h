#pragma once
#include "esp_err.h"

/* Start the normal-mode station and its recovery task. Call once after the
 * Wi-Fi driver, default event loop, and relay client have been initialized. */
esp_err_t rw_wifi_station_start(void);
