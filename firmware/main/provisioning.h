/* provisioning — SoftAP setup mode.
 *
 * Brings up an isolated WPA2 access point named remote-wake-<deviceId[0:4]>
 * with a passphrase derived from the device token (printed on the serial
 * console together with a WIFI: QR payload), and serves a single-page setup
 * UI on http://192.168.4.1.
 *
 * Endpoints (setup mode ONLY — the server is never started on the home
 * network, so /pair simply does not exist there):
 *   GET  /         setup page
 *   GET  /info     {deviceId, keys, maxKeys, ssid, relayUrl, fw}
 *   POST /config   {ssid, pass, relay}
 *   POST /pair     {pubkey, name} -> {deviceId, deviceToken, relayUrl, kid}
 *   POST /finish   reboot into normal mode
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start SoftAP + HTTP server. Wi-Fi must already be initialized. */
esp_err_t rw_prov_start(void);

/** Tear the server and the AP down. */
esp_err_t rw_prov_stop(void);

bool rw_prov_active(void);

/** Print the AP name, passphrase and a WIFI: QR payload to the console. */
void rw_prov_print_credentials(void);

#ifdef __cplusplus
}
#endif
