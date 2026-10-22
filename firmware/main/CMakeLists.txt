# Standalone application boot flow. Shared functionality lives in remote_wake.
idf_component_register(
    SRCS "main.c" "ota_app.c"
    INCLUDE_DIRS "."
    REQUIRES remote_wake
    PRIV_REQUIRES app_update esp_app_format nvs_flash esp_wifi esp_netif esp_event driver
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_FIRMWARE_VERSION="${PROJECT_VER}")
