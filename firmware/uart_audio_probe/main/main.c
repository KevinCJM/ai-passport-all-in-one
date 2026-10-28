// Standalone UART1 PCM bridge probe (not XiaoZhi firmware).
// Verifies real ESP Emulator UART1 -> Mac TCP -> PortAudio round-trip.
#include <inttypes.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define PROBE_UART UART_NUM_1
#define PROBE_BAUD 2000000
static const char* TAG = "MAC_AUDIO_PROBE";

void app_main(void) {
    uart_config_t cfg = {
        .baud_rate = PROBE_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(PROBE_UART, &cfg));
    ESP_ERROR_CHECK(uart_driver_install(PROBE_UART, 8192, 8192, 0, NULL, 0));
    ESP_LOGI(TAG, "PROBE_READY baud=%d", PROBE_BAUD);
    uint8_t pcm[640];
    uint64_t sent = 0;
    while (true) {
        int n = uart_read_bytes(PROBE_UART, pcm, sizeof(pcm), pdMS_TO_TICKS(60));
        if (n <= 0) continue;
        int wrote = uart_write_bytes(PROBE_UART, (const char*)pcm, n);
        if (wrote > 0) sent += (unsigned)wrote;
        if (sent > 0 && ((sent / 2048) != ((sent - (unsigned)(wrote > 0 ? wrote : 0)) / 2048))) {
            ESP_LOGI(TAG, "PCM_ECHO_BYTES=%" PRIu64, sent);
        }
    }
}
