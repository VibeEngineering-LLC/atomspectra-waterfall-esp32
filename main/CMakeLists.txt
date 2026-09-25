    idf_component_register(
    SRCS "main.c" "usb_host_cdc.c" "wifi_manager.c" "web_server.c" "web_util.c" "spectrum.c" "spectrum_t1.c" "tcp_bridge.c" "spectrogram.c" "web_waterfall.c" "boot_config.c" "wf_offload.c" "wf_seg_delete.c" "monitor.c" "net_time.c" "http_io_gate.c" "spectrum_http_cache.c" "debug_log_ring.c" "hist_drop_diag.c" "flash_quiet.c" "spectrum_hist_stage.c" "backup_plan.c"
    INCLUDE_DIRS "."
    REQUIRES usb_host_cdc_acm esp_wifi esp_http_server nvs_flash esp_event esp_timer littlefs shproto json mdns esp_http_client mbedtls esp_app_format
    EMBED_FILES "${PROJECT_DIR}/web/index.html" "${PROJECT_DIR}/web/setup.html" "${PROJECT_DIR}/web/waterfall.html" "${PROJECT_DIR}/web/saved.html" "${PROJECT_DIR}/web/system.html" "${PROJECT_DIR}/web/service.html" "${PROJECT_DIR}/web/monitor.html" "${PROJECT_DIR}/web/common-time.js" "${PROJECT_DIR}/web/captive.html"
)

# P0 review (f091b01): механическая защита от повторения "3 spectrum_data_t на
# стеке app_main" — порог = CONFIG_ESP_MAIN_TASK_STACK_SIZE (sdkconfig, 3584 Б).
# -Werror делает это жёстким гейтом сборки; на 2026-09-25 шум от старого кода
# main/* — 0 предупреждений (проверено: -Wframe-larger-than=3584 глобально по
# всем 934 единицам трансляции сборки — тоже 0).
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=frame-larger-than=3584)
