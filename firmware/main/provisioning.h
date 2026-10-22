/* provisioning — SoftAP setup mode.
 *
 * Brings up an isolated WPA2 access point named remote-wake-<deviceId[0:4]>
 * with a passphrase derived from the device token (printed on the serial
 * console together with a WIFI: QR payload), and serves a single-page setup
 * UI on http://192.168.4.1. DHCP advertises AP-local DNS; HTTP connectivity
 * probes redirect to the setup UI so clients can offer a captive-portal login.
 *
 * Endpoints (setup mode ONLY — the server is never started on the home
 * network, so /pair simply does not exist there):
 *   GET  /         setup page
 *   GET  /info     {deviceId, keys, maxKeys, ssid, relayUrl, relayOverride,
 *                   relayDefault, fw}
 *   POST /config   {ssid, pass, relay?} (relay omitted/empty keeps the stored
 *                  override; the firmware default clears it)
 *   POST /pair     {pubkey, name} -> {deviceId, deviceToken, relayUrl, kid}
 *   POST /finish   reboot into normal mode
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start SoftAP + captive DNS + HTTP server. Wi-Fi must already be initialized. */
esp_err_t rw_prov_start(void);

/** Tear down HTTP, captive DNS and AP (also cleans up partial startup). */
esp_err_t rw_prov_stop(void);

bool rw_prov_active(void);

/** Print the AP name, passphrase and a WIFI: QR payload to the console. */
void rw_prov_print_credentials(void);

#ifdef __cplusplus
}
#endif
