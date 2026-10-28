#pragma once
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <atomic>

namespace passport {
class WifiPortal {
    struct Network {char hex[65]{};int rssi{};bool secure{};};
    StaticSemaphore_t storage_{};SemaphoreHandle_t mutex_{};
    Network networks_[16]{};unsigned count_{};
    char phase_[12]{"idle"},message_[100]{},target_[33]{},ip_[16]{};
    std::atomic<bool> enabled_{},scanRequest_{},scanDone_{},dnsStop_{true};
    std::atomic<unsigned> dnsQueries_{};
    mutable std::atomic<unsigned> setupRequests_{};
    bool scanning_{},scanned_{};int scanError_{};TaskHandle_t dnsTask_{};int dnsSocket_{-1};
    static void dnsTask(void*);
    bool startDns();
    bool access(httpd_req_t*,bool owner=true)const;
    esp_err_t get(httpd_req_t*);
    esp_err_t identityData(httpd_req_t*);
    esp_err_t identityUpload(httpd_req_t*);
    esp_err_t mediaPrefs(httpd_req_t*);
    esp_err_t mediaSave(httpd_req_t*);
    esp_err_t ratePrefs(httpd_req_t*);
    esp_err_t rateSave(httpd_req_t*);
    esp_err_t podcastList(httpd_req_t*);
    esp_err_t podcastSearch(httpd_req_t*);
    esp_err_t podcastSave(httpd_req_t*);
    esp_err_t stockList(httpd_req_t*);
    esp_err_t stockSearch(httpd_req_t*);
    esp_err_t stockSave(httpd_req_t*);
    esp_err_t wheelList(httpd_req_t*);
    esp_err_t wheelSave(httpd_req_t*);
public:
    char name[28]{};
    void init(httpd_handle_t,esp_netif_t*);
    void begin();
    void end();
    void scanDone(){scanDone_=true;}
    void tick(bool apReady,bool joining);
    void phase(const char*,const char*,const char* ssid="",const char* ip="");
    esp_err_t status(httpd_req_t*);
    bool acceptsSetup(httpd_req_t* request)const{return access(request,false);}
    static bool sameOrigin(httpd_req_t* request);
    unsigned dnsQueries()const{return dnsQueries_;}
};
}
