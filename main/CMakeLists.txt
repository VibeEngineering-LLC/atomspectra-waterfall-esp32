idf_component_register(
    SRCS "main.c" "usb_host_cdc.c" "wifi_manager.c" "web_server.c" "spectrum.c" "tcp_bridge.c" "spectrogram.c" "web_waterfall.c" "boot_config.c"
    INCLUDE_DIRS "."
    REQUIRES usb_host_cdc_acm esp_wifi esp_http_server nvs_flash esp_event esp_timer littlefs shproto json mdns
    EMBED_FILES "${PROJECT_DIR}/web/index.html" "${PROJECT_DIR}/web/setup.html" "${PROJECT_DIR}/web/waterfall.html" "${PROJECT_DIR}/web/saved.html" "${PROJECT_DIR}/web/system.html"
)
