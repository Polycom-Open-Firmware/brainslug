set(srcs "main.c" "http_api.c" "uart_bridge.c" "net_config.c" "eth_init.c")
set(reqs esp_eth esp_netif esp_http_server nvs_flash driver app_update
         esp_https_ota json esp_ringbuf mdns lwip esp_adc)

if(CONFIG_PROBE_CAMERA)
    list(APPEND srcs "camera.c")
    list(APPEND reqs esp32-camera)
endif()

if(CONFIG_PROBE_AUDIO)
    list(APPEND srcs "audio.c")
    list(APPEND reqs esp-dsp)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES ${reqs}
    EMBED_TXTFILES "ui/index.html"
)
