#include "menu_service.hpp"
#include "json_output.hpp"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include <cmath>
#include <cstdio>
#include <cstring>
namespace passport {
void MenuService::init(Runtime& runtime){runtime_=&runtime;mutex_=xSemaphoreCreateMutexStatic(&storage_);revision_=esp_random();if(!revision_)revision_=1;}
int MenuService::request(const cJSON* data,char* output,size_t size){
    if(size<2)return 500;
    strcpy(output,"{}");
    if(data){
        const auto* encoded=cJSON_GetObjectItemCaseSensitive(data,"layout");const auto* revision=cJSON_GetObjectItemCaseSensitive(data,"revision");
        if(!cJSON_IsString(encoded)||strlen(encoded->valuestring)!=((sizeof(MenuLayout)+2)/3)*4||!cJSON_IsNumber(revision)||!std::isfinite(revision->valuedouble)||revision->valuedouble<1||revision->valuedouble>UINT32_MAX||revision->valuedouble!=static_cast<uint32_t>(revision->valuedouble))return 400;
        MenuLayout candidate;size_t bytes=0;
        if(mbedtls_base64_decode(reinterpret_cast<unsigned char*>(&candidate),sizeof(candidate),&bytes,reinterpret_cast<const unsigned char*>(encoded->valuestring),strlen(encoded->valuestring))||bytes!=sizeof(candidate)||!candidate.valid())return 400;
        xSemaphoreTake(mutex_,portMAX_DELAY);
        if(pendingTicket_||revision_!=static_cast<uint32_t>(revision->valuedouble)){xSemaphoreGive(mutex_);snprintf(output,size,"{\"error\":\"配置已变化或正在保存，请重新读取\"}");return 409;}
        pending_=candidate;if(!++nextTicket_)++nextTicket_;pendingTicket_=nextTicket_;
        snprintf(output,size,"{\"queued\":true,\"ticket\":%lu}",static_cast<unsigned long>(pendingTicket_));xSemaphoreGive(mutex_);return 200;
    }
    char encoded[((sizeof(MenuLayout)+2)/3)*4+1];size_t length=0;
    xSemaphoreTake(mutex_,portMAX_DELAY);
    const auto& layout=runtime_->menuLayout();
    const int rc=mbedtls_base64_encode(reinterpret_cast<unsigned char*>(encoded),sizeof(encoded),&length,reinterpret_cast<const unsigned char*>(&layout),sizeof(layout));
    if(!rc)snprintf(output,size,"{\"layout\":\"%s\",\"revision\":%lu,\"pending\":%lu,\"done\":%lu,\"result\":%d}",encoded,static_cast<unsigned long>(revision_),static_cast<unsigned long>(pendingTicket_),static_cast<unsigned long>(doneTicket_),result_);
    xSemaphoreGive(mutex_);return rc||size<length+140?500:200;
}
int MenuService::catalog(unsigned group,char* output,size_t size){
    if(group>=5)return 400;
    JsonOutput json(output,size);char number[32];snprintf(number,sizeof(number),"{\"group\":%u,\"name\":",group);json.append(number);json.string(kMenuPages[group]);json.append(",\"apps\":[");
    for(unsigned i=kMenuStarts[group];i<kMenuStarts[group+1];++i){if(i!=kMenuStarts[group])json.append(",");const auto app=kFunctions[i];snprintf(number,sizeof(number),"{\"id\":%u,\"name\":",static_cast<unsigned>(app));json.append(number);json.string(menuLabel(app));json.append("}");}
    json.append("]}");return json.valid()?200:500;
}
bool MenuService::poll(){
    xSemaphoreTake(mutex_,portMAX_DELAY);
    const bool pending=pendingTicket_!=0;
    if(pending){result_=runtime_->configureMenu(pending_)?0:-10;doneTicket_=pendingTicket_;pendingTicket_=0;if(!result_){if(!++revision_)++revision_;}}
    xSemaphoreGive(mutex_);return pending;
}
}
