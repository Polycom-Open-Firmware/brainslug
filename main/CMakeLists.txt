idf_component_register(
    SRCS "main.c" "http_api.c" "uart_bridge.c"
    INCLUDE_DIRS "."
    REQUIRES esp_eth esp_netif esp_http_server nvs_flash driver app_update esp_https_ota json esp_ringbuf
)
