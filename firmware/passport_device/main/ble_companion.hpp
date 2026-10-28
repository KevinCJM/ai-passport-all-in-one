#pragma once
#include "ble_exchange.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <atomic>
extern "C" {
#include "host/ble_gatt.h"
}
namespace passport {
class BleCompanion {
    BleExchange* exchange_{};StaticSemaphore_t storage_{};SemaphoreHandle_t mutex_{};
    std::atomic<uint16_t> connection_{0xffff};std::atomic<bool> secure_{};
    int64_t fragmentAt_{};
    static int access(uint16_t,uint16_t,ble_gatt_access_ctxt*,void*);
public:
    bool start();void stop();
    void connected(uint16_t connection);void disconnected();
    void secured(bool secure){secure_=secure;}
    const ble_gatt_svc_def* services();
    void poll(int(*handler)(const char*,char*,size_t));
    bool drained();
};
extern BleCompanion bleCompanion;
}
