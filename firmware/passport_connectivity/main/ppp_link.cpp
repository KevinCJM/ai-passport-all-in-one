#include "ppp_link.hpp"
#include <cstdio>
#include <cstring>
#include "esp_netif_ppp.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "driver/usb_serial_jtag.h"
#include "lwip/netdb.h"

namespace passport {
PppLink pppLink;
esp_err_t PppLink::transmit(void* argument,void* data,size_t size) {
    auto& self=*static_cast<PppLink*>(argument);
    const auto result=self.output_?self.output_(self.argument_,static_cast<const uint8_t*>(data),size):ESP_FAIL;
    if(result==ESP_OK) self.tx_+=size;else ++self.errors_;
    return result;
}
void PppLink::event(void* argument,esp_event_base_t base,int32_t id,void* data) {
    auto& self=*static_cast<PppLink*>(argument);
    if(base==IP_EVENT&&id==IP_EVENT_PPP_GOT_IP) {
        if(static_cast<ip_event_got_ip_t*>(data)->esp_netif!=self.netif_)return;
        // PPP reports its address before link-up; finish the IDF connected action
        // on the event loop so default routing is selected after the link is up.
        esp_netif_action_connected(self.netif_,base,id,data);
        self.up_=true;self.dead_=false;
    } else if(base==IP_EVENT&&id==IP_EVENT_PPP_LOST_IP) self.up_=false;
    else if(base==NETIF_PPP_STATUS) {
        if(data&&*static_cast<esp_netif_t**>(data)!=self.netif_) return;
        if(id>=NETIF_PP_PHASE_OFFSET&&id<NETIF_PPP_INTERNAL_ERR_OFFSET) self.phase_=id-NETIF_PP_PHASE_OFFSET;
        if(id==NETIF_PPP_PHASE_DEAD) { self.up_=false;self.dead_=true; }
        if(id>NETIF_PPP_ERRORNONE&&id<NETIF_PP_PHASE_OFFSET) { if(id!=NETIF_PPP_ERRORUSER)++self.errors_;self.up_=false; }
    }
}
esp_err_t PppLink::start(Output output,void* argument) {
    if(netif_)return ESP_ERR_INVALID_STATE;
    if(!ioMutex_)ioMutex_=xSemaphoreCreateMutexStatic(&ioStorage_);
    previous_=esp_netif_get_default_netif();
    for(unsigned i=0;i<2;++i) if(previous_)esp_netif_get_dns_info(previous_,static_cast<esp_netif_dns_type_t>(i),&dns_[i]);
    output_=output;argument_=argument;tx_=rx_=phase_=errors_=0;up_=false;dead_=false;
    auto base=*ESP_NETIF_BASE_DEFAULT_PPP;base.if_key="PASSPORT_PPP";base.if_desc="USB PPP";base.route_prio=50;
    esp_netif_config_t config=ESP_NETIF_DEFAULT_PPP();config.base=&base;netif_=esp_netif_new(&config);
    if(!netif_)return ESP_ERR_NO_MEM;
    esp_netif_driver_ifconfig_t driver{};driver.handle=this;driver.transmit=transmit;
    esp_err_t result=esp_netif_set_driver_config(netif_,&driver);
    esp_netif_ppp_config_t parameters{};parameters.ppp_phase_event_enabled=parameters.ppp_error_event_enabled=true;
    if(!result)result=esp_netif_ppp_set_params(netif_,&parameters);
    if(!result)result=esp_event_handler_register(IP_EVENT,ESP_EVENT_ANY_ID,event,this);
    if(!result)result=esp_event_handler_register(NETIF_PPP_STATUS,ESP_EVENT_ANY_ID,event,this);
    if(result) { dead_=true;stop();return result; }
    esp_netif_action_start(netif_,nullptr,0,nullptr);return ESP_OK;
}
esp_err_t PppLink::input(const uint8_t* data,size_t size) {
    if(!data||!size||size>4096||!ioMutex_)return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(ioMutex_,portMAX_DELAY);
    const auto result=netif_?esp_netif_receive(netif_,const_cast<uint8_t*>(data),size,nullptr):ESP_ERR_INVALID_STATE;
    xSemaphoreGive(ioMutex_);
    if(!result)rx_+=size;else ++errors_;
    return result;
}
void PppLink::linkLost() {
    if(netif_)esp_netif_action_stop(netif_,nullptr,0,nullptr);
}
esp_err_t PppLink::stop() {
    if(!netif_)return ESP_OK;
    if(probing_)return ESP_ERR_INVALID_STATE;
    if(!dead_) {
        esp_netif_action_stop(netif_,nullptr,0,nullptr);
        const auto until=esp_timer_get_time()+4000000;
        while(!dead_&&esp_timer_get_time()<until)vTaskDelay(pdMS_TO_TICKS(10));
        if(!dead_)return ESP_ERR_TIMEOUT;
    }
    usbStop_=true;
    const auto until=esp_timer_get_time()+1000000;
    while(usbTask_&&esp_timer_get_time()<until)vTaskDelay(pdMS_TO_TICKS(10));
    if(usbTask_)return ESP_ERR_TIMEOUT;
    esp_event_handler_unregister(IP_EVENT,ESP_EVENT_ANY_ID,event);esp_event_handler_unregister(NETIF_PPP_STATUS,ESP_EVENT_ANY_ID,event);
    xSemaphoreTake(ioMutex_,portMAX_DELAY);
    esp_netif_destroy(netif_);netif_=nullptr;up_=false;
    xSemaphoreGive(ioMutex_);
    // Destroy/stop already reselects the highest-priority live interface.
    // An explicit override here pins an AP default and defeats WiFi/PPP failover.
#ifndef CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF
    if(previous_&&esp_netif_get_default_netif()==previous_&&esp_netif_is_netif_up(previous_)) {
        for(unsigned i=0;i<2;++i)esp_netif_set_dns_info(previous_,static_cast<esp_netif_dns_type_t>(i),&dns_[i]);
    }
#endif
    if(usb_serial_jtag_is_driver_installed())usb_serial_jtag_driver_uninstall();
    return ESP_OK;
}
void PppLink::usbTask(void* argument) {
    auto& self=*static_cast<PppLink*>(argument);uint8_t buffer[512];
    while(!self.usbStop_) { const int size=usb_serial_jtag_read_bytes(buffer,sizeof(buffer),pdMS_TO_TICKS(20));if(size>0)self.input(buffer,size); }
    self.usbTask_=nullptr;vTaskDelete(nullptr);
}
esp_err_t PppLink::startUsb() {
    usb_serial_jtag_driver_config_t config{};config.rx_buffer_size=config.tx_buffer_size=2048;
    auto result=usb_serial_jtag_driver_install(&config);if(result)return result;
    usbStop_=false;
    result=start([](void*,const uint8_t* data,size_t size)->esp_err_t {
        return usb_serial_jtag_write_bytes(data,size,pdMS_TO_TICKS(100))==static_cast<int>(size)?ESP_OK:ESP_FAIL;
    },nullptr);
    if(!result&&xTaskCreate(usbTask,"usb_ppp",3072,this,4,&usbTask_)!=pdPASS)result=ESP_ERR_NO_MEM;
    if(result) { usbStop_=true;stop();if(!netif_&&usb_serial_jtag_is_driver_installed())usb_serial_jtag_driver_uninstall(); }
    return result;
}
esp_err_t PppLink::probe() {
    if(!up_||probing_.exchange(true))return ESP_ERR_INVALID_STATE;
    probeHttp_=probeBytes_=probeMs_=0;probeError_=0;
    if(xTaskCreate(probeTask,"ppp_https",12288,this,3,nullptr)!=pdPASS) { probing_=false;return ESP_ERR_NO_MEM; }
    return ESP_OK;
}
void PppLink::probeTask(void* argument) {
    auto& self=*static_cast<PppLink*>(argument);const auto started=esp_timer_get_time();
    addrinfo* addresses=nullptr;addrinfo hints{};hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;
    if(getaddrinfo("example.com","443",&hints,&addresses)) self.probeError_=-1;
    else {
        freeaddrinfo(addresses);
        esp_http_client_config_t config{};config.url="https://example.com/";config.timeout_ms=15000;
        config.crt_bundle_attach=esp_crt_bundle_attach;config.buffer_size=1024;config.buffer_size_tx=512;config.user_data=&self;
        config.addr_type=HTTP_ADDR_TYPE_INET; // This PPP link negotiates IPv4 only.
        config.event_handler=[](esp_http_client_event_t* event)->esp_err_t {
            if(event->event_id==HTTP_EVENT_ON_DATA&&event->data_len>0)static_cast<PppLink*>(event->user_data)->probeBytes_+=event->data_len;
            return ESP_OK;
        };
        auto client=esp_http_client_init(&config);
        if(!client)self.probeError_=ESP_ERR_NO_MEM;
        else { self.probeError_=esp_http_client_perform(client);self.probeHttp_=esp_http_client_get_status_code(client);esp_http_client_cleanup(client); }
    }
    self.probeMs_=(esp_timer_get_time()-started)/1000;self.probing_=false;vTaskDelete(nullptr);
}
void PppLink::status(char* output,size_t size) const {
    esp_netif_ip_info_t address{};
    if(ioMutex_)xSemaphoreTake(ioMutex_,portMAX_DELAY);
    if(netif_)esp_netif_get_ip_info(netif_,&address);
    if(ioMutex_)xSemaphoreGive(ioMutex_);
    snprintf(output,size,"{\"active\":%s,\"up\":%s,\"phase\":%u,\"tx_bytes\":%u,\"rx_bytes\":%u,\"errors\":%u,\"ip\":\"" IPSTR "\",\"probing\":%s,\"probe_http\":%u,\"probe_bytes\":%u,\"probe_error\":%d,\"probe_ms\":%u}",
        netif_?"true":"false",up_?"true":"false",unsigned(phase_.load()),unsigned(tx_.load()),unsigned(rx_.load()),unsigned(errors_.load()),IP2STR(&address.ip),probing_?"true":"false",unsigned(probeHttp_.load()),unsigned(probeBytes_.load()),probeError_.load(),unsigned(probeMs_.load()));
}
}
