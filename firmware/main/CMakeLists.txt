# Standalone application boot flow. Shared functionality lives in remote_wake.
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES remote_wake
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event driver
)
