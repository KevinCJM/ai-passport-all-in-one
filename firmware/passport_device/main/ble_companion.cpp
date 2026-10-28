#include "ble_companion.hpp"
#include "board.hpp"
#include "esp_timer.h"
#include <cstdio>
#include <new>
extern "C" {
#include "host/ble_hs.h"
}
namespace passport {
BleCompanion bleCompanion;
bool BleCompanion::start(){
    if(!mutex_)mutex_=xSemaphoreCreateMutexStatic(&storage_);
    if(!exchange_)exchange_=new(std::nothrow)BleExchange;
    return mutex_&&exchange_;
}
void BleCompanion::stop(){delete exchange_;exchange_=nullptr;connection_=0xffff;secure_=false;}
void BleCompanion::connected(uint16_t connection){
    xSemaphoreTake(mutex_,portMAX_DELAY);if(exchange_)exchange_->reset();connection_=connection;secure_=false;xSemaphoreGive(mutex_);
}
void BleCompanion::disconnected(){
    if(!mutex_)return;
    xSemaphoreTake(mutex_,portMAX_DELAY);if(exchange_)exchange_->reset();connection_=0xffff;secure_=false;xSemaphoreGive(mutex_);
}
int BleCompanion::access(uint16_t connection,uint16_t,ble_gatt_access_ctxt* context,void* argument){
    auto& self=bleCompanion;
    if(!self.secure_||self.connection_!=connection||board::usbConnected())return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    if(xSemaphoreTake(self.mutex_,pdMS_TO_TICKS(1000))!=pdTRUE)return BLE_ATT_ERR_INSUFFICIENT_RES;
    int result=BLE_ATT_ERR_UNLIKELY;uint8_t bytes[BleExchange::packetSize];
    if(self.exchange_&&context->op==BLE_GATT_ACCESS_OP_WRITE_CHR&&!argument){
        uint16_t size{};
        if(!ble_hs_mbuf_to_flat(context->om,bytes,sizeof(bytes),&size)){
            const auto now=esp_timer_get_time();
            if(!self.exchange_->pending&&now-self.fragmentAt_>3000000)self.exchange_->reset();
            result=self.exchange_->accept(bytes,size)?0:BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;self.fragmentAt_=now;
        }
    }else if(self.exchange_&&context->op==BLE_GATT_ACCESS_OP_READ_CHR&&argument){
        const size_t size=self.exchange_->read(bytes);result=os_mbuf_append(context->om,bytes,size)?BLE_ATT_ERR_INSUFFICIENT_RES:0;
    }
    xSemaphoreGive(self.mutex_);return result;
}
const ble_gatt_svc_def* BleCompanion::services(){
    static const ble_uuid128_t service=BLE_UUID128_INIT(0x20,0xb0,0xe3,0xdc,0xbd,0x4f,0x6c,0x9f,0x30,0x4e,0xed,0x70,0x6a,0xd6,0x67,0x5d);
    static const ble_uuid128_t command=BLE_UUID128_INIT(0x21,0xb0,0xe3,0xdc,0xbd,0x4f,0x6c,0x9f,0x30,0x4e,0xed,0x70,0x6a,0xd6,0x67,0x5d);
    static const ble_uuid128_t response=BLE_UUID128_INIT(0x22,0xb0,0xe3,0xdc,0xbd,0x4f,0x6c,0x9f,0x30,0x4e,0xed,0x70,0x6a,0xd6,0x67,0x5d);
    static ble_gatt_chr_def chars[3]{};static ble_gatt_svc_def services[2]{};
    chars[0].uuid=&command.u;chars[0].access_cb=access;chars[0].flags=BLE_GATT_CHR_F_WRITE|BLE_GATT_CHR_F_WRITE_ENC;chars[0].min_key_size=16;
    chars[1].uuid=&response.u;chars[1].access_cb=access;chars[1].arg=reinterpret_cast<void*>(1);chars[1].flags=BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_READ_ENC;chars[1].min_key_size=16;
    services[0].type=BLE_GATT_SVC_TYPE_PRIMARY;services[0].uuid=&service.u;services[0].characteristics=chars;return services;
}
void BleCompanion::poll(int(*handler)(const char*,char*,size_t)){
    if(!mutex_||xSemaphoreTake(mutex_,0)!=pdTRUE)return;
    if(exchange_&&exchange_->pending){
        constexpr size_t prefix=32;
        const int status=handler(exchange_->request,exchange_->response+prefix,BleExchange::capacity-prefix-2);
        if(status!=200)strcpy(exchange_->response+prefix,"{}");
        char header[prefix];const int length=snprintf(header,sizeof(header),"{\"status\":%d,\"body\":",status);
        const size_t size=strlen(exchange_->response+prefix);
        memmove(exchange_->response+length,exchange_->response+prefix,size);memcpy(exchange_->response,header,length);exchange_->response[length+size]='}';exchange_->response[length+size+1]=0;exchange_->complete();
    }
    xSemaphoreGive(mutex_);
}
bool BleCompanion::drained(){
    if(!mutex_||!exchange_)return true;
    if(xSemaphoreTake(mutex_,0)!=pdTRUE)return false;
    const bool done=!exchange_->pending&&(!exchange_->ready||exchange_->readOffset==exchange_->responseSize||esp_timer_get_time()-fragmentAt_>500000);
    xSemaphoreGive(mutex_);return done;
}
}
