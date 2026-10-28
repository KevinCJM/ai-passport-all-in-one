#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ESP32_SIM";

void app_main(void) {
    ESP_LOGI(TAG, "SIM_BOOT_OK");
    ESP_LOGI(TAG, "TARGET=%s", CONFIG_IDF_TARGET);

    unsigned heartbeat = 0;
    while (true) {
        ESP_LOGI(TAG, "SIM_HEARTBEAT %u", heartbeat++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
