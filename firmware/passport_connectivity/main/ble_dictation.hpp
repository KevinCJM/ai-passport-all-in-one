#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "ima_adpcm.hpp"
extern "C" {
#include "host/ble_gatt.h"
}
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
namespace passport {
class BleDictation {
    SemaphoreHandle_t mutex_{};
    std::atomic<uint16_t> connection_{0xffff};
    std::atomic<bool> secure_{}, keysSubscribed_{}, audioSubscribed_{};
    std::atomic<unsigned> phase_{}, acknowledgement_{};
    uint32_t generation_{}, session_{};
    uint16_t sequence_{}, keyboardHandle_{}, audioHandle_{};
    int64_t deadline_{};
    static constexpr int64_t kLongest=60000000;  // µs: a longer recording ends on its own, as Confirm would
#ifdef PASSPORT_DEVICE
    ImaState ima_{};int16_t pendingPcm_[310]{};size_t pendingSamples_{};
    bool flushPcm();
    using UsbOutput=bool(*)(void*,uint8_t,const uint8_t*,size_t);
    UsbOutput usbOutput_{};void* usbContext_{};
    std::atomic<bool> usbMode_{},usbReady_{};
#endif
    bool packet(uint8_t type,const uint8_t* payload=nullptr,size_t size=0);
    int finish();
    static int access(uint16_t,uint16_t,ble_gatt_access_ctxt*,void*);
public:
#ifdef PASSPORT_DEVICE
    void usbMode(bool);
    void usbOutput(UsbOutput callback,void* context){usbOutput_=callback;usbContext_=context;}
    void usbConnected(bool);
    bool usbAck(const uint8_t*,size_t);
#endif
    std::atomic<uint32_t> sent{}, dropped{}, rejected{}, pcmBytes{};
    const ble_gatt_svc_def* services();
    void connected(uint16_t connection) { connection_=connection; }
    void secured(bool value) { secure_=value; }
    void disconnected();
    void subscribed(uint16_t handle,bool enabled);
    int toggle(uint32_t generation);
    // A key on the computer between phrases (report 7): Return, or deleting the line the cursor is on.
    enum Key : uint8_t { Return=1, ClearLine=2 };
    int key(uint32_t generation,Key key);
    void tick();
    void cancel();
    bool pcm(uint32_t generation,const uint8_t* data,size_t size);
    bool recording() const { return phase_==2; }
    unsigned phase() const { return phase_; }
};
extern BleDictation& dictationBle;
}
