#pragma once
#include <atomic>
#include <cstdint>
extern "C" {
#include "host/ble_gatt.h"
}
namespace passport {
// Standard BLE HID keyboard for the PPT page: any computer pairs it as a keyboard, no helper app.
class BleSlides {
    std::atomic<uint16_t> connection_{0xffff};
    std::atomic<bool> secure_{},subscribed_{};
    uint16_t inputHandle_{};
    static int access(uint16_t,uint16_t,ble_gatt_access_ctxt*,void*);
    bool report(uint8_t modifiers,uint8_t key);
public:
    static constexpr uint8_t Shift=0x02,Alt=0x04,Gui=0x08;
    static constexpr uint8_t Right=0x4f,Left=0x50,F5=0x3e,Return=0x28,P=0x13,B=0x05,Escape=0x29;
    std::atomic<unsigned> sent{},failed{};
    const ble_gatt_svc_def* services();
    void connected(uint16_t connection){connection_=connection;}
    void secured(bool value){secure_=value;}
    void subscribed(uint16_t handle,bool enabled){if(handle==inputHandle_)subscribed_=enabled;}
    void disconnected(){connection_=0xffff;secure_=subscribed_=false;}
    bool ready() const {return secure_&&subscribed_&&connection_!=0xffff;}
    bool tap(uint8_t modifiers,uint8_t key);
};
extern BleSlides slidesBle;
}
