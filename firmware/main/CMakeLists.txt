set(RW_SRCS
    "main.c"
    "wifi_recovery.c"
    "wifi_station.c"
    "config.c"
    "auth.c"
    "canon_json.c"
    "b64url.c"
    "power_btn.c"
    "relay_client.c"
    "provisioning.c"
    "led.c"
    "usb_hid.c"
)

set(RW_REQUIRES
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    esp_timer
    esp_http_server
    esp_websocket_client
    esp-tls
    mbedtls
    json
    libsodium
    qrcode
    driver
    lwip
)

# Requirements are expanded before sdkconfig is loaded, so profile CONFIG_*
# values cannot select this dependency. Match the manifest's supported targets.
if(IDF_TARGET STREQUAL "esp32s2" OR IDF_TARGET STREQUAL "esp32s3" OR
   IDF_TARGET STREQUAL "esp32p4")
    list(APPEND RW_REQUIRES esp_tinyusb)
endif()

if(CONFIG_RW_LED_ENABLED)
    list(APPEND RW_REQUIRES led_strip)
endif()

idf_component_register(
    SRCS ${RW_SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${RW_REQUIRES}
    EMBED_TXTFILES "www/setup.html"
)
