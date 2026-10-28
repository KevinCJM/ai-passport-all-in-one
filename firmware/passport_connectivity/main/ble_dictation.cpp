#include "startup_object.hpp"
#include "ble_dictation.hpp"
#include <algorithm>
#include <cstring>
#include "esp_timer.h"
extern "C" {
#include "host/ble_hs.h"
}
namespace passport {
BleDictation& dictationBle=startupObject<BleDictation>();
namespace {
[[maybe_unused]] const ble_uuid16_t service=BLE_UUID16_INIT(0x1812), map=BLE_UUID16_INIT(0x2a4b),
    report=BLE_UUID16_INIT(0x2a4d), reference=BLE_UUID16_INIT(0x2908), info=BLE_UUID16_INIT(0x2a4a);
const uint8_t reportMap[]={
    0x05,1,0x09,6,0xa1,1,0x85,1,0x05,7,0x19,0xe0,0x29,0xe7,0x15,0,0x25,1,
    0x75,1,0x95,8,0x81,2,0x95,1,0x75,8,0x81,1,0x95,6,0x75,8,0x15,0,
    0x25,0x65,0x05,7,0x19,0,0x29,0x65,0x81,0,0xc0,
    0x06,0,0xff,0x09,1,0xa1,1,0x85,2,0x15,0,0x26,0xff,0,0x75,8,0x95,176,
    0x09,1,0x81,2,0x85,3,0x95,16,0x09,2,0x91,2,0xc0};
uint8_t refs[3][2]={{1,1},{2,1},{3,2}};
uint32_t load32(const uint8_t* p) { return p[0]|p[1]<<8U|p[2]<<16U|uint32_t(p[3])<<24U; }
void store32(uint8_t* p,uint32_t value) { for(unsigned i=0;i<4;++i) p[i]=(value>>(8*i))&255; }
}
int BleDictation::access(uint16_t connection,uint16_t,ble_gatt_access_ctxt* context,void* argument) {
    auto& self=dictationBle; const auto type=reinterpret_cast<uintptr_t>(argument);
    if(context->op==BLE_GATT_ACCESS_OP_READ_DSC)
        return os_mbuf_append(context->om,refs[type-10],2) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    if(context->op==BLE_GATT_ACCESS_OP_READ_CHR) {
        if(type==1) return os_mbuf_append(context->om,reportMap,sizeof(reportMap));
        if(type==2) { const uint8_t value[]={0x11,1,0,2}; return os_mbuf_append(context->om,value,4); }
        const uint8_t value[8]{}; return os_mbuf_append(context->om,value,sizeof(value));
    }
    if(context->op==BLE_GATT_ACCESS_OP_WRITE_CHR && type==5) {
        uint8_t value[16]{}; uint16_t size=0;
        if(!self.secure_ || connection!=self.connection_ ||
           ble_hs_mbuf_to_flat(context->om,value,sizeof(value),&size) || size!=16 ||
           load32(value+1)!=self.generation_ || load32(value+5)!=self.session_ ||
           (value[0]!=1 && value[0]!=2)) { ++self.rejected; return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN; }
        self.acknowledgement_=value[0]; return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}
const ble_gatt_svc_def* BleDictation::services() {
    if(!mutex_) mutex_=xSemaphoreCreateMutex();
    #ifdef PASSPORT_DEVICE
    static const ble_uuid128_t serviceId=BLE_UUID128_INIT(0x10,0xb0,0xe3,0xdc,0xbd,0x4f,0x6c,0x9f,0x30,0x4e,0xed,0x70,0x6a,0xd6,0x67,0x5d);
    static const ble_uuid128_t keyId=BLE_UUID128_INIT(0x11,0xb0,0xe3,0xdc,0xbd,0x4f,0x6c,0x9f,0x30,0x4e,0xed,0x70,0x6a,0xd6,0x67,0x5d);
    static const ble_uuid128_t audioId=BLE_UUID128_INIT(0x12,0xb0,0xe3,0xdc,0xbd,0x4f,0x6c,0x9f,0x30,0x4e,0xed,0x70,0x6a,0xd6,0x67,0x5d);
    static const ble_uuid128_t ackId=BLE_UUID128_INIT(0x13,0xb0,0xe3,0xdc,0xbd,0x4f,0x6c,0x9f,0x30,0x4e,0xed,0x70,0x6a,0xd6,0x67,0x5d);
    static ble_gatt_chr_def deviceChars[4]{};static ble_gatt_svc_def deviceServices[2]{};
    const ble_uuid_t* ids[]={&keyId.u,&audioId.u,&ackId.u};
    for(unsigned i=0;i<3;++i){deviceChars[i].uuid=ids[i];deviceChars[i].access_cb=access;deviceChars[i].arg=reinterpret_cast<void*>(i+3);deviceChars[i].min_key_size=16;deviceChars[i].flags=i==2?BLE_GATT_CHR_F_WRITE|BLE_GATT_CHR_F_WRITE_ENC:BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_READ_ENC|BLE_GATT_CHR_F_NOTIFY;}
    deviceChars[0].val_handle=&keyboardHandle_;deviceChars[1].val_handle=&audioHandle_;deviceServices[0].type=BLE_GATT_SVC_TYPE_PRIMARY;deviceServices[0].uuid=&serviceId.u;deviceServices[0].characteristics=deviceChars;return deviceServices;
    #else
    static ble_gatt_dsc_def descriptors[3][2]{};
    static ble_gatt_chr_def chars[6]{};
    static ble_gatt_svc_def definitions[2]{};
    for(unsigned i=0;i<3;++i) {
        descriptors[i][0].uuid=&reference.u; descriptors[i][0].att_flags=BLE_ATT_F_READ;
        descriptors[i][0].access_cb=access; descriptors[i][0].arg=reinterpret_cast<void*>(10+i);
    }
    const uint16_t flags[5]={BLE_GATT_CHR_F_READ,BLE_GATT_CHR_F_READ,
        BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_READ_ENC|BLE_GATT_CHR_F_NOTIFY,
        BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_READ_ENC|BLE_GATT_CHR_F_NOTIFY,
        BLE_GATT_CHR_F_WRITE|BLE_GATT_CHR_F_WRITE_ENC};
    for(unsigned i=0;i<5;++i) {
        chars[i].uuid=i==0 ? &map.u : i==1 ? &info.u : &report.u;
        chars[i].access_cb=access; chars[i].arg=reinterpret_cast<void*>(i+1); chars[i].flags=flags[i];
        if(i>=2) { chars[i].min_key_size=16; chars[i].descriptors=descriptors[i-2]; }
    }
    chars[2].val_handle=&keyboardHandle_; chars[3].val_handle=&audioHandle_;
    definitions[0].type=BLE_GATT_SVC_TYPE_PRIMARY; definitions[0].uuid=&service.u; definitions[0].characteristics=chars;
    return definitions;
    #endif
}
void BleDictation::subscribed(uint16_t handle,bool enabled) {
    if(handle==keyboardHandle_) keysSubscribed_=enabled;
    if(handle==audioHandle_) audioSubscribed_=enabled;
}
void BleDictation::disconnected() {
    connection_=0xffff; secure_=keysSubscribed_=audioSubscribed_=false; phase_=acknowledgement_=0;
}
bool BleDictation::packet(uint8_t type,const uint8_t* payload,size_t size) {
    if(size>160){++dropped;return false;}
#ifdef PASSPORT_DEVICE
    const bool usb=usbMode_;
    if(usb?(!usbReady_||!usbOutput_):(!secure_||!audioSubscribed_||connection_==0xffff)){++dropped;return false;}
#else
    if(!secure_||!audioSubscribed_||connection_==0xffff){++dropped;return false;}
#endif
    uint8_t data[176]{}; data[0]=type; store32(data+1,generation_); store32(data+5,session_);
    data[9]=sequence_&255; data[10]=sequence_>>8; data[11]=size;
    if(size) memcpy(data+12,payload,size);
    bool accepted=false;
#ifdef PASSPORT_DEVICE
    if(usb){accepted=usbOutput_(usbContext_,11,data,sizeof(data));if(accepted){++sequence_;++sent;return true;}++dropped;return false;}
    const int64_t until=esp_timer_get_time()+(type==2?30000:250000);
    do{
        // Reports come from the larger mbuf pool (asking for 280 bytes selects it), so audio the link has not
        // carried yet never takes the small buffers the computer's requests are answered from.
        auto* buffer=os_msys_get_pkthdr(280,0);
        if(buffer){buffer->om_data+=32;if(os_mbuf_append(buffer,data,sizeof(data))){os_mbuf_free_chain(buffer);buffer=nullptr;}}
        if(buffer&&!ble_gatts_notify_custom(connection_,audioHandle_,buffer)){accepted=true;break;}
        vTaskDelay(pdMS_TO_TICKS(2));
    }while(secure_&&connection_!=0xffff&&esp_timer_get_time()<until);
#else
    auto* buffer=ble_hs_mbuf_from_flat(data,sizeof(data));accepted=buffer&&!ble_gatts_notify_custom(connection_,audioHandle_,buffer);
#endif
    if(!accepted){++dropped;return false;}
    ++sequence_; ++sent; return true;
}
int BleDictation::toggle(uint32_t generation) {
    if(!mutex_ || xSemaphoreTake(mutex_,pdMS_TO_TICKS(1000))!=pdTRUE) return -6;
    int result=0;
#ifdef PASSPORT_DEVICE
    if(usbMode_?(!usbReady_||!usbOutput_):(!secure_||!keysSubscribed_||!audioSubscribed_))result=-7;
#else
    if(!secure_ || !keysSubscribed_ || !audioSubscribed_) result=-7;
#endif
    else if(phase_==0) {
        generation_=generation; ++session_; sequence_=0; acknowledgement_=0; phase_=1;
#ifdef PASSPORT_DEVICE
        ima_={};pendingSamples_=0;
        // 47 audio reports a second need a short connection interval: 15-30 ms, within Apple's rules
        // (max at least min + 15 ms, supervision timeout 2-6 s). The computer may keep its own.
        // Asked only when slower: a Mac already at 15 ms stalled the link for ~10 s on a repeated request.
        ble_gap_conn_desc link{};
        if(!usbMode_&&!ble_gap_conn_find(connection_,&link)&&link.conn_itvl>24){ble_gap_upd_params fast{};fast.itvl_min=12;fast.itvl_max=24;fast.supervision_timeout=400;ble_gap_update_params(connection_,&fast);}
        const uint8_t format[7]={0x80,0x3e,0,0,16,1,1};
#else
        const uint8_t format[6]={0x80,0x3e,0,0,16,1};
#endif
        if(!packet(1,format,sizeof(format))) { phase_=0; result=-8; }
        deadline_=esp_timer_get_time()+5000000;
    } else if(phase_==2) result=finish();
    else result=-9;
    xSemaphoreGive(mutex_); return result;
}
int BleDictation::key(uint32_t generation,Key key) {
    if(!mutex_ || xSemaphoreTake(mutex_,pdMS_TO_TICKS(1000))!=pdTRUE) return -6;
    int result=0;
#ifdef PASSPORT_DEVICE
    if(usbMode_?(!usbReady_||!usbOutput_):(!secure_||!audioSubscribed_))result=-7;
#else
    if(!secure_ || !audioSubscribed_) result=-7;
#endif
    else if(phase_!=0) result=-9;
    else { generation_=generation; const uint8_t which=key; if(!packet(7,&which,1)) result=-8; }
    xSemaphoreGive(mutex_); return result;
}
// Ends the recording: the computer acknowledges once it has the whole utterance. Called with the mutex held.
int BleDictation::finish() {
#ifdef PASSPORT_DEVICE
    if(pendingSamples_&&!flushPcm())return -8;
#endif
    phase_=3; acknowledgement_=0;
    if(!packet(3)) { phase_=0; return -8; }
    deadline_=esp_timer_get_time()+5000000; return 0;
}
void BleDictation::tick() {
    if(!mutex_ || xSemaphoreTake(mutex_,0)!=pdTRUE) return;
    const auto ack=acknowledgement_.exchange(0);
    // The computer recognises the speech itself (语音输入): recording starts as soon as it is ready.
    if(phase_==1 && ack==1) { phase_=2; packet(5); deadline_=esp_timer_get_time()+kLongest; }
    else if(phase_==3 && ack==2) { packet(4); phase_=0; }
    else if(phase_==2 && esp_timer_get_time()>deadline_) finish();
    if((phase_==1 || phase_==3) && esp_timer_get_time()>deadline_) { packet(6); phase_=0; }
    xSemaphoreGive(mutex_);
}
bool BleDictation::pcm(uint32_t generation,const uint8_t* data,size_t size) {
    if(!mutex_ || xSemaphoreTake(mutex_,pdMS_TO_TICKS(1000))!=pdTRUE) return false;
    bool ok=phase_==2 && generation==generation_;
#ifdef PASSPORT_DEVICE
    if(size%2)ok=false;
    const auto* samples=reinterpret_cast<const int16_t*>(data);
    for(size_t i=0;ok&&i<size/2;++i){pendingPcm_[pendingSamples_++]=samples[i];if(pendingSamples_==310)ok=flushPcm();}
#else
    for(size_t offset=0;ok && offset<size;offset+=160) {
        ok=packet(2,data+offset,std::min<size_t>(160,size-offset));
        if(ok) pcmBytes+=std::min<size_t>(160,size-offset);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
#endif
    xSemaphoreGive(mutex_); return ok;
}
#ifdef PASSPORT_DEVICE
void BleDictation::usbMode(bool enabled){
    if(!mutex_)mutex_=xSemaphoreCreateMutex();
    if(usbMode_==enabled)return;
    cancel();usbReady_=false;usbMode_=enabled;
}
void BleDictation::usbConnected(bool value){usbReady_=value;if(!value&&usbMode_)cancel();}
bool BleDictation::usbAck(const uint8_t* value,size_t size){
    if(!usbMode_||!usbReady_||size!=16||!mutex_||xSemaphoreTake(mutex_,pdMS_TO_TICKS(1000))!=pdTRUE)return false;
    const bool valid=load32(value+1)==generation_&&load32(value+5)==session_&&((value[0]==1&&phase_==1)||(value[0]==2&&phase_==3));
    if(valid)acknowledgement_=value[0];else ++rejected;
    xSemaphoreGive(mutex_);return valid;
}
bool BleDictation::flushPcm(){
    uint8_t encoded[160];const size_t count=pendingSamples_;const auto length=imaBlock(pendingPcm_,count,encoded,sizeof(encoded),ima_);pendingSamples_=0;
    if(!length||!packet(2,encoded,length))return false;
    pcmBytes+=count*2;return true;
}
#endif
void BleDictation::cancel() {
    if(!mutex_ || xSemaphoreTake(mutex_,pdMS_TO_TICKS(1000))!=pdTRUE) return;
    if(phase_!=0) packet(6);
    phase_=acknowledgement_=0; xSemaphoreGive(mutex_);
}
}
