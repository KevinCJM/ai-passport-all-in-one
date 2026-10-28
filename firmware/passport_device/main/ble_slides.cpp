#include "ble_slides.hpp"
#include "board.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
extern "C" {
#include "host/ble_hs.h"
}
namespace passport {
BleSlides slidesBle;
namespace {
enum Attribute : uintptr_t { Info=1, Map, Control, Input, Reference, Pnp, Battery };
// Keyboard, report ID 1: modifiers, reserved, six keys. No LED output report.
const uint8_t reportMap[]={
    0x05,1,0x09,6,0xa1,1,0x85,1,0x05,7,0x19,0xe0,0x29,0xe7,0x15,0,0x25,1,
    0x75,1,0x95,8,0x81,2,0x95,1,0x75,8,0x81,1,0x95,6,0x75,8,0x15,0,
    0x25,0x65,0x05,7,0x19,0,0x29,0x65,0x81,0,0xc0};
}
int BleSlides::access(uint16_t,uint16_t,ble_gatt_access_ctxt* context,void* argument){
    const auto append=[&](const void* data,uint16_t size){return os_mbuf_append(context->om,data,size)?BLE_ATT_ERR_INSUFFICIENT_RES:0;};
    switch(reinterpret_cast<uintptr_t>(argument)){
    case Info:{const uint8_t value[]={0x11,1,0,2};return append(value,4);}  // HID 1.11, normally connectable
    case Map:return append(reportMap,sizeof(reportMap));
    case Control:return 0;  // Suspend / exit suspend: nothing to do.
    case Input:{const uint8_t value[8]{};return append(value,8);}
    case Reference:{const uint8_t value[]={1,1};return append(value,2);}  // report ID 1, input
    case Pnp:{const uint8_t value[]={2,0xc0,0x16,0xdf,0x05,0,1};return append(value,7);}  // USB IDs 16C0:05DF, v1.0
    case Battery:{const auto battery=board::battery();const uint8_t level=battery.known?battery.percent:100;return append(&level,1);}
    default:return BLE_ATT_ERR_UNLIKELY;
    }
}
const ble_gatt_svc_def* BleSlides::services(){
    static const ble_uuid16_t hid=BLE_UUID16_INIT(0x1812),info=BLE_UUID16_INIT(0x2a4a),map=BLE_UUID16_INIT(0x2a4b),control=BLE_UUID16_INIT(0x2a4c),
        input=BLE_UUID16_INIT(0x2a4d),reference=BLE_UUID16_INIT(0x2908),dis=BLE_UUID16_INIT(0x180a),pnp=BLE_UUID16_INIT(0x2a50),
        bas=BLE_UUID16_INIT(0x180f),level=BLE_UUID16_INIT(0x2a19);
    static ble_gatt_dsc_def refs[2]{};static ble_gatt_chr_def hidChars[5]{},disChars[2]{},basChars[2]{};static ble_gatt_svc_def defs[4]{};
    const auto chr=[](ble_gatt_chr_def& c,const ble_uuid16_t& uuid,Attribute a,ble_gatt_chr_flags flags){c.uuid=&uuid.u;c.access_cb=access;c.arg=reinterpret_cast<void*>(a);c.flags=flags;};
    refs[0].uuid=&reference.u;refs[0].att_flags=BLE_ATT_F_READ;refs[0].access_cb=access;refs[0].arg=reinterpret_cast<void*>(Reference);
    chr(hidChars[0],info,Info,BLE_GATT_CHR_F_READ);chr(hidChars[1],map,Map,BLE_GATT_CHR_F_READ);chr(hidChars[2],control,Control,BLE_GATT_CHR_F_WRITE_NO_RSP);
    chr(hidChars[3],input,Input,BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_READ_ENC|BLE_GATT_CHR_F_NOTIFY);hidChars[3].descriptors=refs;hidChars[3].val_handle=&inputHandle_;
    chr(disChars[0],pnp,Pnp,BLE_GATT_CHR_F_READ);chr(basChars[0],level,Battery,BLE_GATT_CHR_F_READ);
    const ble_uuid16_t* ids[]={&hid,&dis,&bas};const ble_gatt_chr_def* chars[]={hidChars,disChars,basChars};
    for(unsigned i=0;i<3;++i){defs[i].type=BLE_GATT_SVC_TYPE_PRIMARY;defs[i].uuid=&ids[i]->u;defs[i].characteristics=chars[i];}
    return defs;
}
bool BleSlides::report(uint8_t modifiers,uint8_t key){
    const uint8_t data[8]={modifiers,0,key,0,0,0,0,0};
    for(unsigned attempt=0;attempt<10;++attempt){
        if(!ready())return false;
        auto* buffer=ble_hs_mbuf_from_flat(data,sizeof(data));
        if(buffer&&!ble_gatts_notify_custom(connection_,inputHandle_,buffer))return true;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}
bool BleSlides::tap(uint8_t modifiers,uint8_t key){
    // macOS drops taps much shorter than ~100 ms; always send the release.
    const bool pressed=report(modifiers,key);
    vTaskDelay(pdMS_TO_TICKS(120));
    const bool released=report(0,0);
    if(pressed&&released)++sent;else ++failed;
    return pressed&&released;
}
}
