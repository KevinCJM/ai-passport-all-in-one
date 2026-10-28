cmake_minimum_required(VERSION 3.16)
# Registry components pinned in scripts/fetch_deps.sh.
set(DONOR "${CMAKE_CURRENT_LIST_DIR}/../../deps")
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../passport_suite/components/suite_lvgl" "${DONOR}/espressif__cjson" "${DONOR}/espressif__esp_audio_codec" "${DONOR}/espressif__esp_codec_dev" "${DONOR}/espressif__esp-dsp")
set(COMPONENTS main suite_lvgl esp_wifi esp_event esp_netif nvs_flash mbedtls bt esp_http_server)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(passport_device VERSION 0.5.12)
