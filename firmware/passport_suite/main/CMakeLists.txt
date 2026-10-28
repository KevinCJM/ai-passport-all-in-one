idf_component_register(SRCS "suite.cpp" "domain.cpp" "ui.cpp" "home_ui.cpp" "status_bar.cpp" "ui_font.cpp" "status_font.cpp" "lunar.cpp" "settings.cpp" "identity.cpp" "ble_model.cpp" "sim_main.cpp"
  INCLUDE_DIRS "." REQUIRES suite_lvgl esp_partition esp_wifi esp_event esp_netif nvs_flash mbedtls esp_timer)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
