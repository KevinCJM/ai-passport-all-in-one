set(LIVE "${CMAKE_CURRENT_LIST_DIR}/../../passport_connectivity/main")
set(SUITE "${CMAKE_CURRENT_LIST_DIR}/../../passport_suite/main")
idf_component_register(SRCS "board.cpp" "codec_memory.cpp" "${LIVE}/main.cpp" "${LIVE}/live_services.cpp" "${LIVE}/audio_pipe.cpp" "${LIVE}/ble_dictation.cpp" "${LIVE}/ppp_link.cpp" "${SUITE}/suite.cpp" "${SUITE}/domain.cpp" "${SUITE}/settings.cpp" "${SUITE}/ui.cpp" "${SUITE}/ui_font.cpp" "${SUITE}/status_font.cpp" "${SUITE}/lunar.cpp"
 INCLUDE_DIRS "." "${LIVE}" "${SUITE}"
 PRIV_REQUIRES esp_partition esp_app_format tcp_transport esp-tls
 REQUIRES suite_lvgl esp_wifi esp_event esp_netif nvs_flash mbedtls esp_timer esp_http_server esp_http_client bt espressif__cjson espressif__esp_audio_codec espressif__esp_codec_dev espressif__esp-dsp espressif__esp-sr esp_partition esp_driver_usb_serial_jtag esp_driver_uart esp_driver_i2s esp_driver_i2c esp_driver_spi esp_adc esp_lcd esp_driver_gpio esp_driver_ledc)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
target_sources(${COMPONENT_LIB} PRIVATE "tool_tasks.cpp" "wifi_portal.cpp" "native_services.cpp" "crash_trace.cpp" "nothrow_alloc.cpp" "pcm_playback.cpp" "ble_companion.cpp" "ble_slides.cpp" "${SUITE}/home_ui.cpp" "${SUITE}/status_bar.cpp" "${SUITE}/identity.cpp")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
# codec_memory.cpp replaces esp_audio_codec's weak allocators; nothing else references it, so pull it in.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-u,media_lib_module_malloc")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(${COMPONENT_LIB} PRIVATE PASSPORT_DEVICE=1)
# The fixed-point echo loop must share one C3 core with Opus in real time.
set_source_files_properties("${LIVE}/audio_pipe.cpp" PROPERTIES COMPILE_OPTIONS "-O2")
