#include "aes/esp_aes.h"
#include <memory>
#include <new>
#include "board.hpp"
#include "button_input.hpp"
#include "pcm_playback.hpp"
#include "pairing.hpp"
#include "socket_ipv4.hpp"
#include "cJSON.h"
#include "esp_mac.h"
#include "esp_attr.h"
#include "esp_random.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/stream_buffer.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
extern "C" void passport_crash_trace(char*,size_t);
namespace passport::board {
namespace {
esp_lcd_panel_handle_t panel;
esp_lcd_panel_io_handle_t panelIo;
SemaphoreHandle_t displayDone;
i2c_master_bus_handle_t bus;
struct BatteryBus {
    i2c_master_dev_handle_t handle{};
    bool read(uint8_t reg,uint8_t* data,size_t size){return handle&&i2c_master_transmit_receive(handle,&reg,1,data,size,20)==ESP_OK;}
    bool write(uint8_t reg,uint8_t value){const uint8_t data[]={reg,value};return handle&&i2c_master_transmit(handle,data,sizeof(data),20)==ESP_OK;}
    void delayMs(unsigned ms){vTaskDelay(pdMS_TO_TICKS(ms));}
} batteryBus;
Cw2017<BatteryBus> gauge(batteryBus);
std::atomic<uint32_t> batteryPacked{},batteryReads{},batteryErrors{},batteryPollMaxUs{};
std::atomic<bool> batteryInitialized{},batteryProfileUpdated{};
RTC_DATA_ATTR int batterySleepVerified=-1;
adc_oneshot_unit_handle_t adc;
adc_cali_handle_t calibration;
char token[65]{};
bool provisionActive = false;
Pairing pairing;
uint32_t pairingEpoch{};
portMUX_TYPE pairMux=portMUX_INITIALIZER_UNLOCKED;
struct Event {
    unsigned request, app, generation;
    char operation[48];  // "open:" and a program's name for 打开 Mac 程序
};
QueueHandle_t eventsQueue;
i2s_chan_handle_t tx, rx;
const audio_codec_data_if_t *dataIf;
const audio_codec_ctrl_if_t *ctrlIf;
const audio_codec_gpio_if_t *gpioIf;
const audio_codec_if_t *codecIf;
esp_codec_dev_handle_t codec;
StreamBufferHandle_t micBuffer, refBuffer, alignedRefBuffer;
std::atomic<bool> referenceEnabled{};portMUX_TYPE refMux=portMUX_INITIALIZER_UNLOCKED;
std::atomic<unsigned> referenceSamples{},referenceDropped{},referenceBegin{},referenceEnd{};
std::atomic<bool> captureStop{true}, captureDone{true};
std::atomic<unsigned> captureEpoch{};
bool codecPresent = false;
std::atomic<unsigned> microphoneRms{}, referenceRms{}, keyEvents{};
std::atomic<unsigned> micFrames{}, micDrops{}, speakerSamples{}, audioErrors{}, adcMillivolts{};
struct KeyReport { ButtonEvent event; uint32_t context; };
StaticQueue_t keyQueueControl;
alignas(4) uint8_t keyQueueStorage[8 * sizeof(KeyReport)];
QueueHandle_t keyQueue;
esp_timer_handle_t keyTimer;
ButtonInput keyInput;
std::atomic<uint32_t> inputContext{ButtonInput::Awake};
uint32_t inputPage{UINT32_MAX}, inputPair{UINT32_MAX};
std::atomic<unsigned> keySamples{}, keyDrops{};
unsigned screenPower = 0;
unsigned screenBrightness=80;
RTC_DATA_ATTR unsigned sleepEntries=0,sleepWakeLevel=0;
uint32_t wakeSeconds;  // wakeAfter: deep sleep also ends after this long (the alarm)
unsigned speakerVolume=55;
std::atomic<unsigned> captureGain{24};
PcmPlayback speakerPlayback;
bool speakerMuted=true;
void sampleKeys(void*) {
    int raw = 0, millivolts = 0;
    if (adc_oneshot_read(adc, ADC_CHANNEL_0, &raw) != ESP_OK) { keyInput.invalidSample(); return; }
    millivolts = raw * 2500 / 4095;
    if (calibration && adc_cali_raw_to_voltage(calibration, raw, &millivolts) != ESP_OK) { keyInput.invalidSample(); return; }
    adcMillivolts = millivolts;
    ++keySamples;
    const auto context = inputContext.load();
    const auto event = keyInput.sample(millivolts, esp_timer_get_time() / 1000, context);
    if (event != ButtonEvent::None) {
        const KeyReport report{event, context};
        if (xQueueSend(keyQueue, &report, 0) == pdTRUE) ++keyEvents;
        else ++keyDrops;
    }
}
bool colorDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *) {
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(displayDone, &wake);
    return wake == pdTRUE;
}
bool IRAM_ATTR speakerSent(i2s_chan_handle_t, i2s_event_data_t* event, void*) {
    // Before DMA auto-clear, after these mono samples actually played.
    // Queued writes lead the microphone by the DMA queue latency.
    if(!event->dma_buf)return false;
    const auto* pcm=static_cast<const int16_t*>(event->dma_buf);
    BaseType_t wake=pdFALSE;
    if(referenceEnabled.load()&&event->size<=320){
        int16_t down[80];const size_t count=event->size/4;
        for(size_t i=0;i<count;++i)down[i]=(static_cast<int>(pcm[2*i])+pcm[2*i+1])/16; // Match playback mic gain, 18dB below normal capture.
        portENTER_CRITICAL_ISR(&refMux);
        if(referenceEnabled.load()&&refBuffer){const auto bytes=xStreamBufferSendFromISR(refBuffer,down,count*sizeof(*down),&wake);referenceSamples+=count;referenceDropped+=count-bytes/sizeof(*down);}
        portEXIT_CRITICAL_ISR(&refMux);
    }
    if(speakerPlayback.active())speakerSamples+=speakerPlayback.fillFromISR(static_cast<int16_t*>(event->dma_buf),event->size/2,&wake);
    else memset(event->dma_buf,0,event->size);
    return wake==pdTRUE;
}
void capture(void *) {
    int16_t samples[960],reference[480];
    unsigned settling=4;
    while (!captureStop) {
        const auto epoch=captureEpoch.load();
        if (esp_codec_dev_read(codec, samples, sizeof(samples))) {
            ++audioErrors;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if((epoch&1)||epoch!=captureEpoch.load())continue;
        if(settling){--settling;continue;}
        uint64_t energy = 0;
        for (auto sample : samples)
            energy += static_cast<int32_t>(sample) * sample;
        microphoneRms = std::sqrt(double(energy) / 960);
        if(alignedRefBuffer){
            memset(reference,0,sizeof(reference));portENTER_CRITICAL(&refMux);
            if(referenceEnabled&&refBuffer)xStreamBufferReceive(refBuffer,reference,sizeof(reference),0);
            portEXIT_CRITICAL(&refMux);
            uint64_t refEnergy=0;for(auto sample:reference)refEnergy+=static_cast<int32_t>(sample)*sample;
            referenceRms=std::sqrt(double(refEnergy)/480);
        }
        const bool room=xStreamBufferSpacesAvailable(micBuffer)>=sizeof(samples)&&(!alignedRefBuffer||xStreamBufferSpacesAvailable(alignedRefBuffer)>=sizeof(reference));
        if(room){xStreamBufferSend(micBuffer,samples,sizeof(samples),0);if(alignedRefBuffer)xStreamBufferSend(alignedRefBuffer,reference,sizeof(reference),0);}
        else ++micDrops;
        ++micFrames;
    }
    captureDone = true;
    vTaskDelete(nullptr);
}
} // namespace
void init(SystemSettings &settings) {
    // AES/SHA take one GDMA channel pair on their first DMA-sized job and keep it. If that first job comes after
    // the codec's I2S has used and released a channel, crypto inherits it and TLS fails to verify certificates
    // (seen when 语音唤醒 listened before the first HTTPS request of a boot). Claim crypto's pair now.
    {esp_aes_context aes;esp_aes_init(&aes);const uint8_t key[16]{};esp_aes_setkey(&aes,key,128);
     auto block=std::unique_ptr<uint8_t[]>(new(std::nothrow)uint8_t[1024]());size_t offset=0;uint8_t nonce[16]{},stream[16]{};
     if(block)esp_aes_crypt_ctr(&aes,1024,&offset,nonce,stream,block.get(),block.get());
     esp_aes_free(&aes);}
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open("device", NVS_READWRITE, &nvs));
    size_t length = sizeof(token);
    if(nvs_get_str(nvs,"api_token",token,&length)==ESP_ERR_NVS_NOT_FOUND){uint8_t bytes[32];esp_fill_random(bytes,sizeof(bytes));for(unsigned i=0;i<32;++i)snprintf(token+i*2,3,"%02x",bytes[i]);ESP_ERROR_CHECK(nvs_set_str(nvs,"api_token",token));}
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);
    if(nvs_open("device",NVS_READONLY,&nvs)==ESP_OK){uint8_t saved=55;if(nvs_get_u8(nvs,"speaker_volume",&saved)==ESP_OK&&saved<=100)speakerVolume=saved;nvs_close(nvs);}
    configASSERT(strlen(token) == 64);
    // WiFi was imported privately from the device backup; no simulator credentials.
    if (!strcmp(settings.wifiSsid, "myssid")) {
        ESP_ERROR_CHECK(nvs_open("device", NVS_READONLY, &nvs));
        length = sizeof(settings.wifiSsid);
        nvs_get_str(nvs, "wifi_ssid", settings.wifiSsid, &length);
        length = sizeof(settings.wifiPassword);
        nvs_get_str(nvs, "wifi_pass", settings.wifiPassword, &length);
        nvs_close(nvs);
        SettingsStore store;
        configASSERT(store.save(settings));
    }
    gpio_hold_dis(GPIO_NUM_0);
    gpio_hold_dis(GPIO_NUM_21);
    gpio_deep_sleep_hold_dis();
    gpio_reset_pin(GPIO_NUM_21);
    gpio_set_direction(GPIO_NUM_21, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_21, 0);
    displayDone = xSemaphoreCreateBinary();
    configASSERT(displayDone);
    spi_bus_config_t spi{};
    spi.mosi_io_num = 9;
    spi.miso_io_num = -1;
    spi.sclk_io_num = 8;
    spi.quadwp_io_num = spi.quadhd_io_num = -1;
    spi.max_transfer_sz = 240 * 20 * 2;  // one LVGL band (ui.cpp kDrawRows) per transfer
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &spi, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t io{};
    io.cs_gpio_num = GPIO_NUM_1;
    io.dc_gpio_num = GPIO_NUM_20;
    io.spi_mode = 0;
    io.pclk_hz = 40000000;
    io.trans_queue_depth = 1;
    io.lcd_cmd_bits = io.lcd_param_bits = 8;
    io.on_color_trans_done = colorDone;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io, &panelIo));
    esp_lcd_panel_dev_config_t config{};
    config.reset_gpio_num = GPIO_NUM_NC;
    config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    config.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panelIo, &config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    struct Init {
        uint8_t command, length, data[14];
    };
    const Init init[] = {
        {0xb2, 5, {5, 5, 0, 0x33, 0x33}},
        {0xb7, 1, {0x35}},
        {0xbb, 1, {0x21}},
        {0xc0, 1, {0x2c}},
        {0xc2, 1, {1}},
        {0xc3, 1, {0x0b}},
        {0xc4, 1, {0x20}},
        {0xc6, 1, {0x0f}},
        {0xd0, 2, {0xa7, 0xa1}},
        {0xd0, 2, {0xa4, 0xa1}},
        {0xd6, 1, {0xa1}},
        {0xe0, 14, {0xd0, 4, 8, 0x0a, 9, 5, 0x2d, 0x43, 0x49, 9, 0x16, 0x15, 0x26, 0x2b}},
        {0xe1, 14, {0xd0, 3, 9, 0x0a, 0x0a, 6, 0x2e, 0x44, 0x40, 0x3a, 0x15, 0x15, 0x26, 0x2a}}};
    for (const auto &item : init)
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panelIo, item.command, item.data, item.length));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    ledc_timer_config_t backlightTimer{};backlightTimer.speed_mode=LEDC_LOW_SPEED_MODE;backlightTimer.duty_resolution=LEDC_TIMER_8_BIT;backlightTimer.timer_num=LEDC_TIMER_0;backlightTimer.freq_hz=5000;backlightTimer.clk_cfg=LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&backlightTimer));
    ledc_channel_config_t backlight{};backlight.gpio_num=21;backlight.speed_mode=LEDC_LOW_SPEED_MODE;backlight.channel=LEDC_CHANNEL_0;backlight.timer_sel=LEDC_TIMER_0;backlight.duty=settings.brightness*255/100;
    ESP_ERROR_CHECK(ledc_channel_config(&backlight));screenBrightness=settings.brightness;
    adc_oneshot_unit_init_cfg_t adcConfig{};
    adcConfig.unit_id = ADC_UNIT_1;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&adcConfig, &adc));
    adc_oneshot_chan_cfg_t adcChannel{};
    adcChannel.atten = ADC_ATTEN_DB_12;
    adcChannel.bitwidth = ADC_BITWIDTH_DEFAULT;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, ADC_CHANNEL_0, &adcChannel));
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cal{};
    cal.unit_id = ADC_UNIT_1;
    cal.chan = ADC_CHANNEL_0;
    cal.atten = ADC_ATTEN_DB_12;
    cal.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc_cali_create_scheme_curve_fitting(&cal, &calibration);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cal{};
    cal.unit_id = ADC_UNIT_1;
    cal.atten = ADC_ATTEN_DB_12;
    cal.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc_cali_create_scheme_line_fitting(&cal, &calibration);
#endif
    i2c_master_bus_config_t i2c{};
    i2c.i2c_port = I2C_NUM_0;
    i2c.sda_io_num = GPIO_NUM_10;
    i2c.scl_io_num = GPIO_NUM_7;
    i2c.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c.glitch_ignore_cnt = 7;
    i2c.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c, &bus));
    codecPresent = i2c_master_probe(bus, 0x18, 100) == ESP_OK;
    i2c_device_config_t fuel{};fuel.dev_addr_length=I2C_ADDR_BIT_LEN_7;fuel.device_address=0x63;fuel.scl_speed_hz=100000;
    if(i2c_master_bus_add_device(bus,&fuel,&batteryBus.handle)==ESP_OK){
        batteryInitialized=gauge.initialize(esp_timer_get_time()/1000);
        batteryProfileUpdated=gauge.profileUpdated;
    }
    pollBattery();
    eventsQueue = xQueueCreate(8, sizeof(Event));
    configASSERT(eventsQueue);
    keyQueue = xQueueCreateStatic(8, sizeof(KeyReport), keyQueueStorage, &keyQueueControl);
    configASSERT(keyQueue);
    esp_timer_create_args_t keyConfig{};
    keyConfig.callback = sampleKeys;
    keyConfig.dispatch_method = ESP_TIMER_TASK;
    keyConfig.name = "passport_keys";
    keyConfig.skip_unhandled_events = true;
    ESP_ERROR_CHECK(esp_timer_create(&keyConfig, &keyTimer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(keyTimer, 10000));
}
void draw(int x1, int y1, int x2, int y2, uint8_t *pixels) {
    // ST7789 accepts big-endian RGB565; LVGL's native buffer is little-endian.
    for (int i = 0; i < (x2 - x1 + 1) * (y2 - y1 + 1) * 2; i += 2)
        std::swap(pixels[i], pixels[i + 1]);
    xSemaphoreTake(displayDone, 0);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, x1, y1, x2 + 1, y2 + 1, pixels));
    configASSERT(xSemaphoreTake(displayDone, pdMS_TO_TICKS(1000)) == pdTRUE);
}
void fill(uint16_t color, uint16_t *band, int rows) {
    std::fill(band, band + 240 * rows, uint16_t(color >> 8 | color << 8));  // panel is big-endian
    for (int y = 0; y < 320; y += rows) {
        xSemaphoreTake(displayDone, 0);
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, y, 240, std::min(320, y + rows), band));
        configASSERT(xSemaphoreTake(displayDone, pdMS_TO_TICKS(1000)) == pdTRUE);
    }
}
void backlight(bool on) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, on && !screenPower ? screenBrightness * 255 / 100 : 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
void keyMode(bool volumeKeys, uint32_t generation, bool awake, uint32_t pairEpoch,bool homeScreen) {
    const uint32_t flags = (volumeKeys ? ButtonInput::Volume : 0) | (awake ? ButtonInput::Awake : 0) | (homeScreen ? ButtonInput::HomeScreen : 0);
    const auto previous = inputContext.load();
    if (inputPage != generation || inputPair != pairEpoch || (previous & 7) != flags) {
        inputPage = generation;
        inputPair = pairEpoch;
        inputContext = ((previous + 8) & ~uint32_t(7)) | flags;
    }
}
const char *key() {
    const auto context = inputContext.load();
    KeyReport report{};
    while (xQueueReceive(keyQueue, &report, 0) == pdTRUE) {
        if (report.context != context) continue;
        switch (report.event) {
        case ButtonEvent::Up: return "up";
        case ButtonEvent::Down: return "down";
        case ButtonEvent::Confirm: return "ok";
        case ButtonEvent::Back: return "back";
        case ButtonEvent::Wake: return "wake";
        case ButtonEvent::AccessPoint: return "ap_on";
        case ButtonEvent::Settings: return "settings";
        default: break;
        }
    }
    return nullptr;
}
uint32_t keyContext() { return inputContext.load(); }
bool usbConnected() {
    static int64_t last = esp_timer_get_time();
    if (usb_serial_jtag_is_connected())
        last = esp_timer_get_time();
    return esp_timer_get_time() - last < 3000000;
}
BatteryReading battery(){
    const auto packed=batteryPacked.load();
    return {bool(packed&(1u<<24)),bool(packed&(1u<<25)),uint8_t((packed>>16)&0xff),uint16_t(packed&0xffff)};
}
void pollBattery(){
    const auto before=gauge.reads;const auto started=esp_timer_get_time();gauge.poll(started/1000);
    if(gauge.reads!=before){const auto elapsed=uint32_t(esp_timer_get_time()-started);if(elapsed>batteryPollMaxUs)batteryPollMaxUs=elapsed;}
    const auto reading=gauge.reading();
    batteryPacked=(uint32_t(reading.present)<<24)|(uint32_t(reading.known)<<25)|(uint32_t(reading.percent)<<16)|reading.millivolts;
    batteryReads=gauge.reads;batteryErrors=gauge.errors;
}
void batteryDiagnostics(char* output,size_t size){
    const auto reading=battery();
    snprintf(output,size,"{\"present\":%s,\"known\":%s,\"percent\":%d,\"millivolts\":%d,\"reads\":%u,\"errors\":%u,\"initialized\":%s,\"profile_updated\":%s,\"poll_max_us\":%u,\"last_sleep_verified\":%d}",reading.present?"true":"false",reading.known?"true":"false",reading.known?reading.percent:-1,reading.known?reading.millivolts:-1,unsigned(batteryReads.load()),unsigned(batteryErrors.load()),batteryInitialized?"true":"false",batteryProfileUpdated?"true":"false",unsigned(batteryPollMaxUs.load()),batterySleepVerified);
}
bool setBrightness(unsigned value){
    if(value>100)return false;
    screenBrightness=value;
    return ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,screenPower?0:value*255/100)==ESP_OK&&ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0)==ESP_OK;
}
const char* bluetoothName(){static char name[32]{};if(!*name){uint8_t mac[6];esp_read_mac(mac,ESP_MAC_WIFI_STA);snprintf(name,sizeof(name),"AI-Passport-%02X%02X%02X%02X%02X%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);}return name;}
void power(unsigned state) {
    if (state == screenPower)
        return;
    screenPower = state;
    setBrightness(screenBrightness);
    if (state == 2 && !usbConnected()) {
        audioClose();
        esp_timer_stop(keyTimer);
        ESP_ERROR_CHECK(adc_oneshot_del_unit(adc));adc=nullptr;
        gpio_config_t wakeInput{};wakeInput.pin_bit_mask=1ULL<<GPIO_NUM_0;wakeInput.mode=GPIO_MODE_INPUT;wakeInput.pull_up_en=GPIO_PULLUP_ENABLE;wakeInput.pull_down_en=GPIO_PULLDOWN_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&wakeInput));
        sleepWakeLevel=gpio_get_level(GPIO_NUM_0);
        if(!sleepWakeLevel||usbConnected()){
            adc_oneshot_unit_init_cfg_t unit{};unit.unit_id=ADC_UNIT_1;ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit,&adc));
            adc_oneshot_chan_cfg_t channel{};channel.atten=ADC_ATTEN_DB_12;channel.bitwidth=ADC_BITWIDTH_DEFAULT;ESP_ERROR_CHECK(adc_oneshot_config_channel(adc,ADC_CHANNEL_0,&channel));
            ESP_ERROR_CHECK(esp_timer_start_periodic(keyTimer,10000));screenPower=1;return;
        }
        batterySleepVerified=gauge.sleep()?1:0;
        // Release the shared bus only after codec and gauge transactions have ended.
        for(auto pin:{GPIO_NUM_10,GPIO_NUM_7}){gpio_set_direction(pin,GPIO_MODE_INPUT);gpio_set_pull_mode(pin,GPIO_FLOATING);}
        esp_lcd_panel_disp_on_off(panel, false);
        gpio_hold_en(GPIO_NUM_21);
        gpio_deep_sleep_hold_en();
        ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
            1ULL << GPIO_NUM_0, ESP_GPIO_WAKEUP_GPIO_LOW));
        if(wakeSeconds)esp_sleep_enable_timer_wakeup(uint64_t(wakeSeconds)*1000000);
        ++sleepEntries;esp_deep_sleep_start();
    }
}
void diagnostics(char *output, size_t size) {
    snprintf(
        output, size,
        "{\"reset_reason\":%u,\"display\":true,\"adc_mv\":%u,\"keys\":%u,\"key_samples\":%u,\"key_drops\":%u,\"mic_frames\":%u,\"mic_"
        "rms\":%u,\"mic_"
        "drops\":%u,\"speaker_samples\":%u,\"audio_errors\":%u,\"es8311\":%s,\"codec_open\":%s,\"volume\":%u}",
        unsigned(esp_reset_reason()), adcMillivolts.load(), keyEvents.load(), keySamples.load(), keyDrops.load(), micFrames.load(),
        microphoneRms.load(), micDrops.load(), speakerSamples.load(), audioErrors.load(),
        codecPresent ? "true" : "false", codec ? "true" : "false",speakerVolume);
    const size_t wake=strlen(output);if(wake&&wake+120<size)snprintf(output+wake-1,size-wake+1,",\"wake_cause\":%u,\"wake_gpio\":%llu,\"sleep_entries\":%u,\"sleep_wake_level\":%u,\"brightness\":%u}",unsigned(esp_sleep_get_wakeup_cause()),static_cast<unsigned long long>(esp_sleep_get_gpio_wakeup_status()),sleepEntries,sleepWakeLevel,screenBrightness);
    const size_t base=strlen(output);if(base&&base+30<size)snprintf(output+base-1,size-base+1,",\"reference_rms\":%u}",referenceRms.load());
    char crash[200];passport_crash_trace(crash,sizeof(crash));for(char* c=crash;*c;++c)if(*c=='"'||*c=='\\'||static_cast<unsigned char>(*c)<32)*c=' ';
    const size_t used=strlen(output);if(used&&used+strlen(crash)+20<size)snprintf(output+used-1,size-used+1,",\"last_crash\":\"%s\"}",crash);
}
const char *apiToken() { return token; }
const char* apName(){static char name[28]{};if(!*name){uint8_t mac[6];esp_read_mac(mac,ESP_MAC_WIFI_STA);snprintf(name,sizeof(name),"AI-Passport-%02X%02X",mac[4],mac[5]);}return name;}
bool provisioning() { return provisionActive; }
void provisioning(bool value) { provisionActive = value; }
bool authorized(httpd_req_t *request) {
    char value[72]{};
    if (httpd_req_get_hdr_value_str(request, "Authorization", value, sizeof(value)) != ESP_OK)
        return false;
    unsigned diff = 0;
    const char prefix[] = "Bearer ";
    for (unsigned i = 0; i < 7; ++i)
        diff |= value[i] ^ prefix[i];
    for (unsigned i = 0; i < 64; ++i)
        diff |= value[i + 7] ^ token[i];
    return diff == 0 && value[71] == 0;
}
bool pairCode(char* code,uint32_t* epoch){portENTER_CRITICAL(&pairMux);const bool result=pairing.code(code,esp_timer_get_time()/1000000);if(epoch)*epoch=result?pairingEpoch:0;portEXIT_CRITICAL(&pairMux);return result;}
void pairKey(bool allow,uint32_t expectedEpoch){portENTER_CRITICAL(&pairMux);if(expectedEpoch==pairingEpoch)pairing.key(allow,esp_timer_get_time()/1000000);portEXIT_CRITICAL(&pairMux);}
esp_err_t pair(httpd_req_t* request){
    if(!usbConnected()||!socketLocalIpv4(httpd_req_to_sockfd(request),inet_addr("10.77.0.15")))return httpd_resp_send_err(request,HTTPD_403_FORBIDDEN,"USB only");
    char nonce[33]{};int state=-1;
    if(request->method==HTTP_POST){
        if(request->content_len<=0||request->content_len>80)return httpd_resp_send_err(request,HTTPD_400_BAD_REQUEST,"size");
        char raw[81]{};int used=0;while(used<request->content_len){const int got=httpd_req_recv(request,raw+used,request->content_len-used);if(got<=0)return ESP_FAIL;used+=got;}
        auto* json=cJSON_Parse(raw);const auto* item=cJSON_GetObjectItem(json,"nonce");
        if(cJSON_IsString(item)&&strlen(item->valuestring)==32)strcpy(nonce,item->valuestring);
        cJSON_Delete(json);
        portENTER_CRITICAL(&pairMux);const bool begun=pairing.begin(nonce,esp_timer_get_time()/1000000);if(begun)++pairingEpoch;portEXIT_CRITICAL(&pairMux);
        if(!begun)return httpd_resp_send_err(request,HTTPD_400_BAD_REQUEST,"pair busy or invalid nonce");
        state=0;
    }else{
        if(httpd_req_get_hdr_value_str(request,"X-Passport-Pair",nonce,sizeof(nonce))!=ESP_OK)return httpd_resp_send_err(request,HTTPD_403_FORBIDDEN,"nonce");
        portENTER_CRITICAL(&pairMux);state=pairing.poll(nonce,esp_timer_get_time()/1000000);portEXIT_CRITICAL(&pairMux);
        if(state<0)return httpd_resp_send_err(request,HTTPD_403_FORBIDDEN,"expired");
    }
    uint8_t mac[6];esp_read_mac(mac,ESP_MAC_WIFI_STA);char output[200];
    if(state==1)snprintf(output,sizeof(output),"{\"state\":\"approved\",\"api_token\":\"%s\",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}",token,mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    else strcpy(output,"{\"state\":\"pending\"}");
    httpd_resp_set_type(request,"application/json");httpd_resp_set_hdr(request,"Cache-Control","no-store");return httpd_resp_sendstr(request,output);
}
void event(const char *operation, unsigned request, unsigned app, unsigned generation) {
    Event e{request, app, generation, {}};
    snprintf(e.operation, sizeof(e.operation), "%s", operation);
    if (xQueueSend(eventsQueue, &e, 0) != pdTRUE) {
        Event old;
        xQueueReceive(eventsQueue, &old, 0);
        xQueueSend(eventsQueue, &e, 0);
    }
}
esp_err_t events(httpd_req_t *request) {
    if (!authorized(request))
        return httpd_resp_send_err(request, HTTPD_401_UNAUTHORIZED, "unauthorized");
    char output[160];
    nextEvent(output,sizeof(output));
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, output);
}
void nextEvent(char* output,size_t size){
    Event event{};if(xQueueReceive(eventsQueue,&event,0)!=pdTRUE){snprintf(output,size,"{}");return;}
    snprintf(output, size,
             "{\"request\":%u,\"app\":%u,\"generation\":%u,\"operation\":\"%s\"}", event.request,
             event.app, event.generation, event.operation);
}
bool audioOpen(bool captureEnabled,unsigned captureFrames,bool alignedReference) {
    if (!codecPresent)
        return false;
    if (codec)
        return true;
    captureStop = true;
    captureDone = true;
    captureEpoch=0;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = 4;
    channel.dma_frame_num = 160;
    channel.auto_clear_after_cb = false; // Callback clears/refills after copying played reference.
    if (i2s_new_channel(&channel, &tx, &rx) != ESP_OK)
        return false;
    i2s_std_config_t config{};
    config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000);
    config.slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    config.gpio_cfg.mclk = GPIO_NUM_6;
    config.gpio_cfg.bclk = GPIO_NUM_5;
    config.gpio_cfg.ws = GPIO_NUM_3;
    config.gpio_cfg.dout = GPIO_NUM_2;
    config.gpio_cfg.din = GPIO_NUM_4;
    if (i2s_channel_init_std_mode(tx, &config) || i2s_channel_init_std_mode(rx, &config)) {
        audioClose();
        return false;
    }
    i2s_event_callbacks_t callbacks{};callbacks.on_sent=speakerSent;
    if(i2s_channel_register_event_callback(tx,&callbacks,nullptr)!=ESP_OK){audioClose();return false;}
    i2s_channel_enable(tx);
    i2s_channel_enable(rx);
    audio_codec_i2s_cfg_t data{};
    data.port = I2S_NUM_0;
    data.rx_handle = rx;
    data.tx_handle = tx;
    dataIf = audio_codec_new_i2s_data(&data);
    audio_codec_i2c_cfg_t control{};
    control.port = I2C_NUM_0;
    control.addr = 0x30;
    control.bus_handle = bus;
    ctrlIf = audio_codec_new_i2c_ctrl(&control);
    gpioIf = audio_codec_new_gpio();
    if (!dataIf || !ctrlIf || !gpioIf) {
        audioClose();
        return false;
    }
    uint8_t reset = 0x1f;
    ctrlIf->write_reg(ctrlIf, 0, 1, &reset, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    es8311_codec_cfg_t chip{};
    chip.ctrl_if = ctrlIf;
    chip.gpio_if = gpioIf;
    chip.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    chip.pa_pin = GPIO_NUM_NC;
    chip.use_mclk = true;
    chip.hw_gain.pa_voltage = 5;
    chip.hw_gain.codec_dac_voltage = 3.3;
    codecIf = es8311_codec_new(&chip);
    if (!codecIf) {
        audioClose();
        return false;
    }
    esp_codec_dev_cfg_t dev{};
    dev.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT;
    dev.codec_if = codecIf;
    dev.data_if = dataIf;
    codec = esp_codec_dev_new(&dev);
    esp_codec_dev_sample_info_t format{};
    format.sample_rate = 16000;
    format.channel = 1;
    format.bits_per_sample = 16;
    if (!codec || esp_codec_dev_open(codec, &format)) {
        audioClose();
        return false;
    }
    if(esp_codec_dev_set_in_gain(codec,24)!=ESP_CODEC_DEV_OK){audioClose();return false;}captureGain=24;
    esp_codec_dev_set_out_vol(codec, speakerVolume);
    esp_codec_dev_set_out_mute(codec,true);speakerMuted=true;
    if(!captureEnabled)return true;
    micBuffer = xStreamBufferCreate(std::clamp(captureFrames,2U,3U)*1920, 1920);
    referenceEnabled=false;
    refBuffer=alignedReference?xStreamBufferCreate(1920,1):nullptr;
    alignedRefBuffer=alignedReference?xStreamBufferCreate(std::clamp(captureFrames,2U,3U)*960,960):nullptr;
    if (!micBuffer||(alignedReference&&(!refBuffer||!alignedRefBuffer))) {
        audioClose();
        return false;
    }
    captureStop = false;
    captureDone = false;
    if (xTaskCreate(capture, "device_mic", 4608, nullptr, 5, nullptr) != pdPASS) {
        captureDone = true;
        audioClose();
        return false;
    }
    return true;
}
void audioClose() {
    if(!speakerPlayback.stop())return;
    captureStop = true;
    for (unsigned i = 0; i < 150 && !captureDone; ++i)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (!captureDone)
        return;
    if (codec) {
        esp_codec_dev_close(codec);
        esp_codec_dev_delete(codec);
        codec = nullptr;
    }
    if (codecIf)
        audio_codec_delete_codec_if(codecIf);
    if (ctrlIf)
        audio_codec_delete_ctrl_if(ctrlIf);
    if (gpioIf)
        audio_codec_delete_gpio_if(gpioIf);
    if (dataIf)
        audio_codec_delete_data_if(dataIf);
    codecIf = nullptr;
    ctrlIf = nullptr;
    gpioIf = nullptr;
    dataIf = nullptr;
    if (tx) {
        i2s_channel_disable(tx);
        i2s_del_channel(tx);
        tx = nullptr;
    }
    if (rx) {
        i2s_channel_disable(rx);
        i2s_del_channel(rx);
        rx = nullptr;
    }
    if (micBuffer)
        vStreamBufferDelete(micBuffer);
    if (refBuffer)
        vStreamBufferDelete(refBuffer);
    if(alignedRefBuffer)vStreamBufferDelete(alignedRefBuffer);
    micBuffer = refBuffer = alignedRefBuffer = nullptr;referenceEnabled=false;
}
unsigned volume(){return speakerVolume;}
unsigned microphoneGain(){return captureGain;}
void outputVolume(unsigned value){if(codec)esp_codec_dev_set_out_vol(codec,std::min(value,100u));}
void wakeAfter(uint32_t seconds){wakeSeconds=seconds;}
bool setVolume(unsigned value){
    if(value>100)return false;
    if(value==speakerVolume)return true;
    nvs_handle_t handle;if(nvs_open("device",NVS_READWRITE,&handle)!=ESP_OK)return false;
    const bool saved=nvs_set_u8(handle,"speaker_volume",value)==ESP_OK&&nvs_commit(handle)==ESP_OK;nvs_close(handle);
    if(!saved)return false;
    speakerVolume=value;return !codec||esp_codec_dev_set_out_vol(codec,value)==ESP_CODEC_DEV_OK;
}
bool microphone(int16_t *samples, int16_t *reference) {
    if (!micBuffer || xStreamBufferBytesAvailable(micBuffer) < 1920 || (alignedRefBuffer&&xStreamBufferBytesAvailable(alignedRefBuffer)<960))
        return false;
    if (xStreamBufferReceive(micBuffer, samples, 1920, 0) != 1920)
        return false;
    memset(reference,0,1920);
    if(alignedRefBuffer){int16_t down[480]{};xStreamBufferReceive(alignedRefBuffer,down,sizeof(down),0);for(unsigned n=0;n<960;++n)reference[n]=down[n/2];}
    return true;
}
bool playbackReference(bool enabled) {
    // The nearby full-volume speaker overloads the chat capture gain. Restore
    // normal sensitivity when recording; playback detection uses lower gain.
    if(enabled&&codec){if(esp_codec_dev_set_in_gain(codec,6)!=ESP_CODEC_DEV_OK)return false;captureGain=6;}
    portENTER_CRITICAL(&refMux);referenceEnabled=enabled;
    if(enabled){referenceSamples=referenceDropped=referenceEnd=0;referenceBegin=unsigned(esp_timer_get_time());}else referenceEnd=unsigned(esp_timer_get_time());
    if(refBuffer)xStreamBufferReset(refBuffer);
    portEXIT_CRITICAL(&refMux);
    if(!enabled&&codec){if(esp_codec_dev_set_in_gain(codec,24)!=ESP_CODEC_DEV_OK)return false;captureGain=24;}
    return !enabled||(refBuffer&&alignedRefBuffer);
}
void referenceStats(unsigned& rate,unsigned& dropped){
    const unsigned elapsed=(referenceEnabled?unsigned(esp_timer_get_time()):referenceEnd.load())-referenceBegin.load();
    rate=elapsed?uint64_t(referenceSamples.load())*1000000/elapsed:0;dropped=referenceDropped;
}
namespace {
bool writeSpeaker(const int16_t *samples, size_t count, unsigned rate, unsigned channels) {
    if (!codec || !rate || !channels)
        return false;
    if(speakerMuted){if(esp_codec_dev_set_out_mute(codec,false))return false;speakerMuted=false;}
    int16_t mono[320];
    size_t used = 0;
    const size_t frames = count / channels;
    for (size_t n = 0; n * rate / 16000 < frames; ++n) {
        const size_t index = n * rate / 16000;
        int value = samples[index * channels];
        if (channels == 2)
            value = (value + samples[index * channels + 1]) / 2;
        mono[used++] = value;
        if (used == 320 || ((n + 1) * rate / 16000 >= frames)) {
            if (esp_codec_dev_write(codec, mono, used * 2)){
                ++audioErrors;return false;
            }
            else
                speakerSamples += used;
            used = 0;
        }
    }
    return true;
}
}
bool speaker(const int16_t *samples,size_t count,unsigned rate,unsigned channels){
    if(speakerPlayback.active()){
        if(rate!=16000||channels!=1||!speakerPlayback.push(samples,count)){++audioErrors;return false;}
        return true;
    }
    return writeSpeaker(samples,count,rate,channels);
}
bool bufferedSpeakerStart(){
    if(!codec||!speakerPlayback.start())return false;
    if(speakerMuted){if(esp_codec_dev_set_out_mute(codec,false)){speakerPlayback.stop();return false;}speakerMuted=false;}
    return true;
}
void bufferedSpeakerFinish(){speakerPlayback.finish();}
bool bufferedSpeakerDrained(){return speakerPlayback.drained();}
void bufferedSpeakerSnapshot(char* output,size_t size){speakerPlayback.snapshot(output,size);}
bool bufferedSpeakerStop(){return speakerPlayback.stop();}
void discardMicrophone(){int16_t samples[960],reference[960];for(unsigned i=0;i<3&&microphone(samples,reference);++i){}}
void captureRecording(bool enabled){const unsigned epoch=captureEpoch.load();captureEpoch=((epoch+2)&~1U)|(enabled?0U:1U);}
void silence() {
    if(!speakerPlayback.stop())return;
    if (codec)
        esp_codec_dev_set_out_mute(codec, true);
    speakerMuted=true;
}
int audioListen() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(19546);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) || listen(fd, 1)) {
        close(fd);
        return -1;
    }
    return fd;
}
} // namespace passport::board
