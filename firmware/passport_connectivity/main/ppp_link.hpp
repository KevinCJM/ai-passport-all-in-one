#pragma once
#include <atomic>
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

namespace passport {
class PppLink {
public:
    using Output=esp_err_t(*)(void*,const uint8_t*,size_t);
private:
    std::atomic<esp_netif_t*> netif_{};
    StaticSemaphore_t ioStorage_{};
    SemaphoreHandle_t ioMutex_{};
    esp_netif_t* previous_{};
    esp_netif_dns_info_t dns_[2]{};
    Output output_{};
    void* argument_{};
    std::atomic<bool> up_{},dead_{true},probing_{};
    std::atomic<uint32_t> tx_{},rx_{},phase_{},errors_{},probeHttp_{},probeBytes_{},probeMs_{};
    std::atomic<int> probeError_{};
    static esp_err_t transmit(void*,void*,size_t);
    static void event(void*,esp_event_base_t,int32_t,void*);
    static void probeTask(void*);
    static void usbTask(void*);
    std::atomic<bool> usbStop_{true};
    TaskHandle_t usbTask_{};
public:
    esp_err_t start(Output,void*);
    esp_err_t startUsb();
    esp_err_t stop();
    void linkLost();
    esp_err_t input(const uint8_t*,size_t);
    esp_err_t probe();
    bool up() const { return up_; }
    bool active() const { return netif_!=nullptr; }
    bool dead() const { return dead_; }
    void status(char*,size_t) const;
};
extern PppLink pppLink;
}
