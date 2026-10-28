set(SUITE "${CMAKE_CURRENT_LIST_DIR}/../../passport_suite/main")
idf_component_register(SRCS "main.cpp" "live_services.cpp" "audio_pipe.cpp" "ble_dictation.cpp" "ppp_link.cpp" "${SUITE}/suite.cpp" "${SUITE}/domain.cpp" "${SUITE}/settings.cpp" "${SUITE}/ui.cpp" "${SUITE}/ui_font.cpp" "${SUITE}/status_font.cpp" "${SUITE}/lunar.cpp"
 INCLUDE_DIRS "." "${SUITE}" REQUIRES suite_lvgl esp_partition esp_wifi esp_event esp_netif nvs_flash mbedtls esp_timer esp_http_server esp_http_client bt espressif__cjson espressif__esp_audio_codec esp_driver_uart esp_driver_usb_serial_jtag)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
target_sources(${COMPONENT_LIB} PRIVATE "${SUITE}/home_ui.cpp" "${SUITE}/status_bar.cpp" "${SUITE}/identity.cpp")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(${COMPONENT_LIB} PRIVATE PASSPORT_SIMULATOR=1)
