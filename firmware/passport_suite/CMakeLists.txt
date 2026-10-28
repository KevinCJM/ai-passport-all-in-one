cmake_minimum_required(VERSION 3.16)
set(COMPONENTS main suite_lvgl esp_wifi esp_event esp_netif nvs_flash mbedtls)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(passport_suite)
