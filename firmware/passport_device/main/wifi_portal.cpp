#include "wifi_portal.hpp"
#include "wifi_portal_page.hpp"
#include "portal_protocol.hpp"
#include "socket_ipv4.hpp"
#include "board.hpp"
#include "identity.hpp"
#include "media_catalog.hpp"
#include "podcast_catalog.hpp"
#include "stock_catalog.hpp"
#include "settings.hpp"
#include "apps.hpp"
#include "cJSON.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <memory>
#include <new>
#include <initializer_list>

namespace passport {
void WifiPortal::init(httpd_handle_t server,esp_netif_t* ap){
    mutex_=xSemaphoreCreateMutexStatic(&storage_);
    uint8_t mac[6];esp_read_mac(mac,ESP_MAC_WIFI_STA);snprintf(name,sizeof(name),"AI-Passport-%02X%02X",mac[4],mac[5]);
    esp_netif_dhcps_stop(ap);
    uint8_t offer=2;ESP_ERROR_CHECK(esp_netif_dhcps_option(ap,ESP_NETIF_OP_SET,ESP_NETIF_DOMAIN_NAME_SERVER,&offer,sizeof(offer)));
    esp_netif_dns_info_t dns{};dns.ip.type=ESP_IPADDR_TYPE_V4;IP4_ADDR(&dns.ip.u_addr.ip4,192,168,8,1);ESP_ERROR_CHECK(esp_netif_set_dns_info(ap,ESP_NETIF_DNS_MAIN,&dns));
    static char uri[]="http://192.168.8.1/";ESP_ERROR_CHECK(esp_netif_dhcps_option(ap,ESP_NETIF_OP_SET,ESP_NETIF_CAPTIVEPORTAL_URI,uri,strlen(uri)));
    httpd_uri_t route{};route.method=HTTP_GET;route.user_ctx=this;route.handler=[](httpd_req_t* request){return static_cast<WifiPortal*>(request->user_ctx)->get(request);};
    for(const char* path:{"/wifi/status","/wifi/scan","/","/*"}){route.uri=path;ESP_ERROR_CHECK(httpd_register_uri_handler(server,&route));}
    httpd_uri_t upload{};upload.uri="/identity";upload.method=HTTP_POST;upload.user_ctx=this;upload.handler=[](httpd_req_t* request){return static_cast<WifiPortal*>(request->user_ctx)->identityUpload(request);};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&upload));
    upload.uri="/media";upload.handler=[](httpd_req_t* request){return static_cast<WifiPortal*>(request->user_ctx)->mediaSave(request);};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&upload));
    upload.uri="/rates";upload.handler=[](httpd_req_t* request){return static_cast<WifiPortal*>(request->user_ctx)->rateSave(request);};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&upload));
    upload.uri="/podcasts";upload.handler=[](httpd_req_t* request){return static_cast<WifiPortal*>(request->user_ctx)->podcastSave(request);};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&upload));
    upload.uri="/stocks";upload.handler=[](httpd_req_t* request){return static_cast<WifiPortal*>(request->user_ctx)->stockSave(request);};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&upload));
    upload.uri="/wheel";upload.handler=[](httpd_req_t* request){return static_cast<WifiPortal*>(request->user_ctx)->wheelSave(request);};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&upload));
}
bool WifiPortal::access(httpd_req_t* request,bool owner)const{
    if(owner&&board::authorized(request))return true;
    const bool permitted=enabled_&&board::provisioning()&&socketLocalIpv4(httpd_req_to_sockfd(request),inet_addr("192.168.8.1"));
    if(permitted)++setupRequests_;
    return permitted;
}
bool WifiPortal::sameOrigin(httpd_req_t* request){
    // Browsers send Origin on cross-site POSTs; a page on another site cannot write here.
    char origin[80]{};
    return httpd_req_get_hdr_value_str(request,"Origin",origin,sizeof(origin))!=ESP_OK||!strcmp(origin,"http://192.168.8.1");
}
namespace {
esp_err_t reply(httpd_req_t* request,const char* status,const char* body){
    httpd_resp_set_status(request,status);httpd_resp_set_type(request,"application/json");httpd_resp_set_hdr(request,"Cache-Control","no-store");return httpd_resp_sendstr(request,body);
}
}
esp_err_t WifiPortal::identityData(httpd_req_t* request){
    std::unique_ptr<uint8_t[]> buffer(new(std::nothrow) uint8_t[2048]);
    if(!buffer)return reply(request,"500 Internal Server Error","{\"message\":\"设备内存不足，请稍后重试\"}");
    // A damaged card (e.g. an interrupted upload) reads as "not imported", never as garbage.
    if(!identityStored())return reply(request,"404 Not Found","{\"message\":\"尚未导入身份卡\"}");
    httpd_resp_set_type(request,"application/octet-stream");httpd_resp_set_hdr(request,"Cache-Control","no-store");
    for(uint32_t offset=0;offset<kIdentityBytes;offset+=2048){
        const auto size=std::min<uint32_t>(2048,kIdentityBytes-offset);
        // An upload that starts mid-download ends the response; the page reports it.
        if(!identityRead(offset,buffer.get(),size)||httpd_resp_send_chunk(request,reinterpret_cast<char*>(buffer.get()),size)!=ESP_OK){httpd_resp_send_chunk(request,nullptr,0);return ESP_FAIL;}
    }
    return httpd_resp_send_chunk(request,nullptr,0);
}
esp_err_t WifiPortal::identityUpload(httpd_req_t* request){
    if(!access(request)||!sameOrigin(request))return reply(request,"403 Forbidden","{\"message\":\"请在设备上开启手机配置后重试\"}");
    if(request->content_len!=kIdentityBytes)return reply(request,"400 Bad Request","{\"message\":\"身份卡数据大小不正确\"}");
    std::unique_ptr<uint8_t[]> buffer(new(std::nothrow) uint8_t[4096]);
    if(!buffer)return reply(request,"500 Internal Server Error","{\"message\":\"设备内存不足，请稍后重试\"}");
    auto receive=[&](uint8_t* into,size_t size)->int{
        for(unsigned timeouts=0;;){const int n=httpd_req_recv(request,reinterpret_cast<char*>(into),size);if(n!=HTTPD_SOCK_ERR_TIMEOUT||++timeouts>3)return n;}
    };
    // Validate the header before erasing anything; a wrong file leaves the old card intact.
    size_t used=0;
    while(used<16){const int n=receive(buffer.get()+used,16-used);if(n<=0)return ESP_FAIL;used+=n;}
    uint32_t version;memcpy(&version,buffer.get()+4,4);
    if(memcmp(buffer.get(),"PPID",4)||version!=1)return reply(request,"400 Bad Request","{\"message\":\"身份卡数据格式不正确\"}");
    if(!identityWriteBegin())return reply(request,"409 Conflict","{\"message\":\"正在保存其他数据，请稍后重试\"}");
    bool written=identityWrite(0,buffer.get(),16);
    while(written&&used<kIdentityBytes){
        const int n=receive(buffer.get(),std::min<size_t>(4096,kIdentityBytes-used));
        written=n>0&&identityWrite(used,buffer.get(),n);
        if(n>0)used+=n;
    }
    // Always finish: a broken transfer fails the CRC and the screens show "not imported".
    if(!identityWriteFinish()||!written)return reply(request,"400 Bad Request","{\"message\":\"保存失败，请重新上传\"}");
    return reply(request,"200 OK","{\"saved\":true}");
}
esp_err_t WifiPortal::mediaPrefs(httpd_req_t* request){
    MediaPrefs prefs{};mediaLoad("prefs",prefs);
    auto* json=cJSON_CreateObject();if(!json)return ESP_FAIL;
    cJSON_AddStringToObject(json,"city",prefs.city);cJSON_AddNumberToObject(json,"latitude",prefs.latitude);cJSON_AddNumberToObject(json,"longitude",prefs.longitude);
    cJSON_AddStringToObject(json,"country",prefs.country);cJSON_AddStringToObject(json,"province",prefs.province);
    // Local stations in the built-in table, so the page can show it worked.
    if(*prefs.city){const auto picks=pickRadios(prefs.city,prefs.province);unsigned local=0;for(unsigned i=0;i<picks.count;++i)local+=kRadioStations[picks.index[i]].region!=0;cJSON_AddNumberToObject(json,"radios",local);}
    auto* raw=cJSON_PrintUnformatted(json);cJSON_Delete(json);if(!raw)return ESP_FAIL;
    httpd_resp_set_type(request,"application/json");httpd_resp_set_hdr(request,"Cache-Control","no-store");const auto result=httpd_resp_sendstr(request,raw);cJSON_free(raw);return result;
}
esp_err_t WifiPortal::mediaSave(httpd_req_t* request){
    if(!access(request)||!sameOrigin(request))return reply(request,"403 Forbidden","{\"message\":\"请在设备上开启手机配置后重试\"}");
    if(request->content_len<=0||request->content_len>1024)return reply(request,"400 Bad Request","{\"message\":\"设置内容过长\"}");
    char raw[1025]{};int used=0;
    while(used<request->content_len){const int n=httpd_req_recv(request,raw+used,request->content_len-used);if(n<=0)return ESP_FAIL;used+=n;}
    auto* json=cJSON_Parse(raw);
    const auto* city=cJSON_GetObjectItemCaseSensitive(json,"city");const auto* country=cJSON_GetObjectItemCaseSensitive(json,"country");
    const auto* latitude=cJSON_GetObjectItemCaseSensitive(json,"latitude");const auto* longitude=cJSON_GetObjectItemCaseSensitive(json,"longitude");
    const auto* province=cJSON_GetObjectItemCaseSensitive(json,"province");
    MediaPrefs prefs{};bool valid=cJSON_IsString(city)&&cJSON_IsString(country)&&cJSON_IsNumber(latitude)&&cJSON_IsNumber(longitude)&&cJSON_IsString(province)&&strlen(province->valuestring)<sizeof(prefs.province)
        &&*city->valuestring&&strlen(city->valuestring)<sizeof(prefs.city)&&std::isfinite(latitude->valuedouble)&&std::isfinite(longitude->valuedouble)
        &&std::fabs(latitude->valuedouble)<=90&&std::fabs(longitude->valuedouble)<=180;
    // Cities in China only (the page lists them with coordinates).
    if(valid){const char* c=country->valuestring;valid=!strcmp(c,"CN")||!strcmp(c,"HK")||!strcmp(c,"MO")||!strcmp(c,"TW");}
    if(valid){strcpy(prefs.city,city->valuestring);strcpy(prefs.country,country->valuestring);prefs.latitude=latitude->valuedouble;prefs.longitude=longitude->valuedouble;strcpy(prefs.province,province->valuestring);}
    cJSON_Delete(json);
    if(!valid)return reply(request,"400 Bad Request","{\"message\":\"城市不正确，请刷新页面后重试\"}");
    if(!passport::mediaSave("prefs",prefs))return reply(request,"500 Internal Server Error","{\"message\":\"保存失败，请重试\"}");
    ++mediaPrefsRevision;
    return reply(request,"200 OK","{\"saved\":true}");
}
esp_err_t WifiPortal::ratePrefs(httpd_req_t* request){
    RateBoard board;if(!SettingsStore::loadRates(board))board={};
    auto* json=cJSON_CreateObject();if(!json)return ESP_FAIL;
    cJSON_AddStringToObject(json,"base",board.base);auto* list=cJSON_AddArrayToObject(json,"targets");
    for(auto& code:board.targets)if(*code)cJSON_AddItemToArray(list,cJSON_CreateString(code));
    auto* raw=cJSON_PrintUnformatted(json);cJSON_Delete(json);if(!raw)return ESP_FAIL;
    httpd_resp_set_type(request,"application/json");httpd_resp_set_hdr(request,"Cache-Control","no-store");const auto result=httpd_resp_sendstr(request,raw);cJSON_free(raw);return result;
}
// 汇率 settings: {"base":"CNY","targets":["USD",...]}, codes from kCurrencies, at most six targets.
esp_err_t WifiPortal::rateSave(httpd_req_t* request){
    if(!access(request)||!sameOrigin(request))return reply(request,"403 Forbidden","{\"message\":\"请在设备上开启手机配置后重试\"}");
    if(request->content_len<=0||request->content_len>256)return reply(request,"400 Bad Request","{\"message\":\"设置内容过长\"}");
    char raw[257]{};int used=0;
    while(used<request->content_len){const int n=httpd_req_recv(request,raw+used,request->content_len-used);if(n<=0)return ESP_FAIL;used+=n;}
    const auto known=[](const cJSON* v){if(!cJSON_IsString(v))return false;for(auto& c:kCurrencies)if(!strcmp(c.code,v->valuestring))return true;return false;};
    auto* json=cJSON_Parse(raw);const auto* base=cJSON_GetObjectItemCaseSensitive(json,"base");const auto* targets=cJSON_GetObjectItemCaseSensitive(json,"targets");
    RateBoard board{};memset(board.targets,0,sizeof(board.targets));
    bool valid=known(base)&&cJSON_IsArray(targets)&&cJSON_GetArraySize(targets)>=1&&cJSON_GetArraySize(targets)<=RateBoard::kTargets;
    if(valid){strcpy(board.base,base->valuestring);unsigned i=0;const cJSON* code;cJSON_ArrayForEach(code,targets){if(!known(code)||!strcmp(code->valuestring,board.base)){valid=false;break;}strcpy(board.targets[i++],code->valuestring);}}
    cJSON_Delete(json);
    if(!valid)return reply(request,"400 Bad Request","{\"message\":\"请选择 1–6 种与主要货币不同的货币\"}");
    if(!SettingsStore::saveRates(board))return reply(request,"500 Internal Server Error","{\"message\":\"保存失败，请重试\"}");
    return reply(request,"200 OK","{\"saved\":true}");
}
namespace {
esp_err_t sendJson(httpd_req_t* request,cJSON* json){
    auto* raw=json?cJSON_PrintUnformatted(json):nullptr;cJSON_Delete(json);if(!raw)return reply(request,"500 Internal Server Error","{\"message\":\"设备内存不足，请稍后重试\"}");
    httpd_resp_set_type(request,"application/json");httpd_resp_set_hdr(request,"Cache-Control","no-store");const auto result=httpd_resp_sendstr(request,raw);cJSON_free(raw);return result;
}
}
esp_err_t WifiPortal::podcastList(httpd_req_t* request){
    PodcastList list;if(!mediaLoad("podcasts",list)||!list.count||list.count>PodcastList::kMax)list=defaultPodcasts();
    auto* json=cJSON_CreateObject();auto* items=cJSON_AddArrayToObject(json,"items");
    for(unsigned i=0;i<list.count;++i){auto* o=cJSON_CreateObject();cJSON_AddNumberToObject(o,"id",list.items[i].album);cJSON_AddStringToObject(o,"n",list.items[i].name);cJSON_AddItemToArray(items,o);}
    return sendJson(request,json);
}
// The phone has no internet on the hotspot, so the device searches Ximalaya through its own WiFi
// (plain HTTP, about 14 KB for eight albums, read as it arrives).
esp_err_t WifiPortal::podcastSearch(httpd_req_t* request){
    if(!access(request))return reply(request,"403 Forbidden","{\"message\":\"请在设备上开启手机配置后重试\"}");
    char word[160]{};
    // The page sends the keyword percent-encoded, which is passed on as it is.
    if(httpd_req_get_url_query_str(request,word,sizeof(word))!=ESP_OK||strncmp(word,"kw=",3)||!word[3])return reply(request,"400 Bad Request","{\"message\":\"请输入节目名称\"}");
    for(const char* c=word+3;*c;++c)if(!isalnum(static_cast<unsigned char>(*c))&&!strchr("%._~-+",*c))return reply(request,"400 Bad Request","{\"message\":\"搜索内容无效\"}");
    struct Found{uint32_t id;uint32_t tracks;bool paid;char title[64],by[32];};
    struct Search{JsonScan scan;Found found[8];unsigned count;};
    static constexpr const char* keys[]{"id","title","nickname","tracks","is_paid"};
    const JsonScan::Field field=[](void* context,unsigned key,const char* v){
        auto& s=*static_cast<Search*>(context);
        if(key==0){if(s.count<std::size(s.found))s.found[s.count++]={uint32_t(strtoul(v,nullptr,10)),0,false,{},{}};else s.count=std::size(s.found)+1;return;}
        if(!s.count||s.count>std::size(s.found))return;
        auto& f=s.found[s.count-1];
        const auto put=[&](auto& to){size_t n=std::min(strlen(v),sizeof(to)-1);while(n&&(static_cast<unsigned char>(v[n])&0xc0)==0x80)--n;memcpy(to,v,n);to[n]=0;};
        if(key==1)put(f.title);else if(key==2)put(f.by);else if(key==3)f.tracks=strtoul(v,nullptr,10);else f.paid=!strcmp(v,"true");
    };
    std::unique_ptr<Search> search(new(std::nothrow) Search{{keys,std::size(keys),"\"docs\":[",field,nullptr},{},0});
    if(!search)return reply(request,"500 Internal Server Error","{\"message\":\"设备内存不足，请稍后重试\"}");
    search->scan=JsonScan(keys,std::size(keys),"\"docs\":[",field,search.get());
    char url[240];snprintf(url,sizeof(url),"http://search.ximalaya.com/front/v1?kw=%s&core=album&page=1&rows=8",word+3);
    esp_http_client_config_t config{};config.url=url;config.timeout_ms=8000;config.buffer_size=1024;config.buffer_size_tx=512;config.addr_type=HTTP_ADDR_TYPE_INET;config.user_data=search.get();
    config.event_handler=[](esp_http_client_event_t* e)->esp_err_t{if(e->event_id==HTTP_EVENT_ON_DATA&&e->data_len>0)static_cast<Search*>(e->user_data)->scan.feed(static_cast<const char*>(e->data),e->data_len);return ESP_OK;};
    auto client=esp_http_client_init(&config);if(!client)return reply(request,"500 Internal Server Error","{\"message\":\"设备内存不足，请稍后重试\"}");
    const auto result=esp_http_client_perform(client);const int code=esp_http_client_get_status_code(client);esp_http_client_cleanup(client);
    if(result!=ESP_OK||code!=200)return reply(request,"502 Bad Gateway","{\"message\":\"搜索失败：Passport 需要先连上能上网的 WiFi\"}");
    auto* json=cJSON_CreateObject();auto* items=cJSON_AddArrayToObject(json,"items");
    for(unsigned i=0;i<std::min<unsigned>(search->count,std::size(search->found));++i){const auto& f=search->found[i];if(!f.id||!*f.title)continue;auto* o=cJSON_CreateObject();
        cJSON_AddNumberToObject(o,"id",f.id);cJSON_AddStringToObject(o,"n",f.title);cJSON_AddStringToObject(o,"by",f.by);cJSON_AddNumberToObject(o,"tracks",f.tracks);cJSON_AddBoolToObject(o,"paid",f.paid);cJSON_AddItemToArray(items,o);}
    return sendJson(request,json);
}
// {"items":[{"id":51076156,"n":"声动早咖啡"},...]}: 1-12 programs, names cut to fit.
esp_err_t WifiPortal::podcastSave(httpd_req_t* request){
    if(!access(request)||!sameOrigin(request))return reply(request,"403 Forbidden","{\"message\":\"请在设备上开启手机配置后重试\"}");
    if(request->content_len<=0||request->content_len>2048)return reply(request,"400 Bad Request","{\"message\":\"设置内容过长\"}");
    std::unique_ptr<char[]> raw(new(std::nothrow) char[request->content_len+1]);if(!raw)return reply(request,"500 Internal Server Error","{\"message\":\"设备内存不足，请稍后重试\"}");
    int used=0;while(used<request->content_len){const int n=httpd_req_recv(request,raw.get()+used,request->content_len-used);if(n<=0)return ESP_FAIL;used+=n;}raw[used]=0;
    auto* json=cJSON_Parse(raw.get());const auto* items=cJSON_GetObjectItemCaseSensitive(json,"items");
    PodcastList list{};bool valid=cJSON_IsArray(items)&&cJSON_GetArraySize(items)>=1&&cJSON_GetArraySize(items)<=int(PodcastList::kMax);
    const cJSON* item;
    if(valid)cJSON_ArrayForEach(item,items){
        const auto* id=cJSON_GetObjectItemCaseSensitive(item,"id");const auto* name=cJSON_GetObjectItemCaseSensitive(item,"n");
        if(!cJSON_IsNumber(id)||id->valuedouble<1||id->valuedouble>4e9||!cJSON_IsString(name)||!*name->valuestring){valid=false;break;}
        auto& to=list.items[list.count];to.album=uint32_t(id->valuedouble);
        for(unsigned i=0;i<list.count;++i)if(list.items[i].album==to.album)valid=false;
        size_t n=std::min(strlen(name->valuestring),sizeof(to.name)-1);while(n&&(static_cast<unsigned char>(name->valuestring[n])&0xc0)==0x80)--n;memcpy(to.name,name->valuestring,n);
        ++list.count;
    }
    cJSON_Delete(json);
    if(!valid)return reply(request,"400 Bad Request","{\"message\":\"节目列表不正确，请刷新页面后重试\"}");
    if(!passport::mediaSave("podcasts",list))return reply(request,"500 Internal Server Error","{\"message\":\"保存失败，请重试\"}");
    return reply(request,"200 OK","{\"saved\":true}");
}
esp_err_t WifiPortal::stockList(httpd_req_t* request){
    StockList list;if(!mediaLoad("stocks",list)||!list.count||list.count>StockList::kMax)list=defaultStocks();
    auto* json=cJSON_CreateObject();auto* items=cJSON_AddArrayToObject(json,"items");
    for(unsigned i=0;i<list.count;++i){auto* o=cJSON_CreateObject();cJSON_AddStringToObject(o,"id",list.items[i].id);cJSON_AddStringToObject(o,"n",list.items[i].name);cJSON_AddItemToArray(items,o);}
    return sendJson(request,json);
}
// Eastmoney's search box through the device's own WiFi (the phone has no internet on the hotspot), plain HTTP; its
// token is the fixed public one of the eastmoney.com page, no account. Only markets the quotes cover are offered.
int searchStocks(const char* word,StockHit* hits,unsigned capacity){
    struct Search{JsonScan scan;StockHit found[8];unsigned count;};
    static constexpr const char* keys[]{"Code","Name","SecurityTypeName","QuoteID"};
    const JsonScan::Field field=[](void* context,unsigned key,const char* v){
        auto& s=*static_cast<Search*>(context);
        if(key==0){if(s.count<std::size(s.found))s.found[s.count++]={};else s.count=std::size(s.found)+1;}
        if(!s.count||s.count>std::size(s.found))return;
        auto& f=s.found[s.count-1];
        const auto put=[&](auto& to){size_t n=std::min(strlen(v),sizeof(to)-1);while(n&&(static_cast<unsigned char>(v[n])&0xc0)==0x80)--n;memcpy(to,v,n);to[n]=0;};
        if(key==0)put(f.code);else if(key==1)put(f.name);else if(key==2)put(f.type);else put(f.id);
    };
    std::unique_ptr<Search> search(new(std::nothrow) Search{{keys,std::size(keys),"\"Data\":[",field,nullptr},{},0});
    if(!search)return -1;
    search->scan=JsonScan(keys,std::size(keys),"\"Data\":[",field,search.get());
    char url[320];snprintf(url,sizeof(url),"http://searchapi.eastmoney.com/api/suggest/get?input=%s&type=14&count=8&token=D43BF722C8E33BDC906FB84D85E326E8",word);
    esp_http_client_config_t config{};config.url=url;config.timeout_ms=8000;config.buffer_size=1024;config.buffer_size_tx=512;config.addr_type=HTTP_ADDR_TYPE_INET;config.user_data=search.get();
    config.event_handler=[](esp_http_client_event_t* e)->esp_err_t{if(e->event_id==HTTP_EVENT_ON_DATA&&e->data_len>0)static_cast<Search*>(e->user_data)->scan.feed(static_cast<const char*>(e->data),e->data_len);return ESP_OK;};
    auto client=esp_http_client_init(&config);if(!client)return -1;
    const auto result=esp_http_client_perform(client);const int code=esp_http_client_get_status_code(client);esp_http_client_cleanup(client);
    if(result!=ESP_OK||code!=200)return -1;
    unsigned count=0;
    for(unsigned i=0;i<std::min<unsigned>(search->count,std::size(search->found));++i){const auto& f=search->found[i];if(stockIdValid(f.id)&&*f.name&&count<capacity)hits[count++]=f;}
    return static_cast<int>(count);
}
esp_err_t WifiPortal::stockSearch(httpd_req_t* request){
    if(!access(request))return reply(request,"403 Forbidden","{\"message\":\"请在设备上开启手机配置后重试\"}");
    char word[160]{};
    if(httpd_req_get_url_query_str(request,word,sizeof(word))!=ESP_OK||strncmp(word,"kw=",3)||!word[3])return reply(request,"400 Bad Request","{\"message\":\"请输入名称或代码\"}");
    for(const char* c=word+3;*c;++c)if(!isalnum(static_cast<unsigned char>(*c))&&!strchr("%._~-+",*c))return reply(request,"400 Bad Request","{\"message\":\"搜索内容无效\"}");
    StockHit hits[8];const int count=searchStocks(word+3,hits,std::size(hits));
    if(count<0)return reply(request,"502 Bad Gateway","{\"message\":\"搜索失败：Passport 需要先连上能上网的 WiFi\"}");
    auto* json=cJSON_CreateObject();auto* items=cJSON_AddArrayToObject(json,"items");
    for(int i=0;i<count;++i){const auto& f=hits[i];auto* o=cJSON_CreateObject();
        cJSON_AddStringToObject(o,"id",f.id);cJSON_AddStringToObject(o,"n",f.name);cJSON_AddStringToObject(o,"t",f.type);cJSON_AddStringToObject(o,"c",f.code);cJSON_AddItemToArray(items,o);}
    return sendJson(request,json);
}
// {"items":[{"id":"105.AAPL","n":"苹果"},...]}: 1-12 securities, names cut to fit.
esp_err_t WifiPortal::stockSave(httpd_req_t* request){
    if(!access(request)||!sameOrigin(request))return reply(request,"403 Forbidden","{\"message\":\"请在设备上开启手机配置后重试\"}");
    if(request->content_len<=0||request->content_len>2048)return reply(request,"400 Bad Request","{\"message\":\"设置内容过长\"}");
    std::unique_ptr<char[]> raw(new(std::nothrow) char[request->content_len+1]);if(!raw)return reply(request,"500 Internal Server Error","{\"message\":\"设备内存不足，请稍后重试\"}");
    int used=0;while(used<request->content_len){const int n=httpd_req_recv(request,raw.get()+used,request->content_len-used);if(n<=0)return ESP_FAIL;used+=n;}raw[used]=0;
    auto* json=cJSON_Parse(raw.get());const auto* items=cJSON_GetObjectItemCaseSensitive(json,"items");
    StockList list{};bool valid=cJSON_IsArray(items)&&cJSON_GetArraySize(items)>=1&&cJSON_GetArraySize(items)<=int(StockList::kMax);
    const cJSON* item;
    if(valid)cJSON_ArrayForEach(item,items){
        const auto* id=cJSON_GetObjectItemCaseSensitive(item,"id");const auto* name=cJSON_GetObjectItemCaseSensitive(item,"n");
        if(!cJSON_IsString(id)||!stockIdValid(id->valuestring)||!cJSON_IsString(name)||!*name->valuestring){valid=false;break;}
        auto& to=list.items[list.count];snprintf(to.id,sizeof(to.id),"%s",id->valuestring);
        for(unsigned i=0;i<list.count;++i)if(!strcmp(list.items[i].id,to.id))valid=false;
        size_t n=std::min(strlen(name->valuestring),sizeof(to.name)-1);while(n&&(static_cast<unsigned char>(name->valuestring[n])&0xc0)==0x80)--n;memcpy(to.name,name->valuestring,n);
        ++list.count;
    }
    cJSON_Delete(json);
    if(!valid)return reply(request,"400 Bad Request","{\"message\":\"自选列表不正确，请刷新页面后重试\"}");
    if(!passport::mediaSave("stocks",list))return reply(request,"500 Internal Server Error","{\"message\":\"保存失败，请重试\"}");
    return reply(request,"200 OK","{\"saved\":true}");
}
// 随机转盘: {"modes":[{"n":"吃饭","o":["烧烤",...]},...]}, as the page edits it.
esp_err_t WifiPortal::wheelList(httpd_req_t* request){
    std::unique_ptr<WheelBoard> board(new(std::nothrow) WheelBoard);if(!board)return reply(request,"500 Internal Server Error","{\"message\":\"设备内存不足，请稍后重试\"}");
    SettingsStore::loadWheel(*board);
    auto* json=cJSON_CreateObject();auto* modes=cJSON_AddArrayToObject(json,"modes");
    for(unsigned i=0;i<board->count;++i){
        const auto& mode=board->modes[i];auto* o=cJSON_CreateObject();cJSON_AddStringToObject(o,"n",mode.name);
        auto* options=cJSON_AddArrayToObject(o,"o");for(unsigned j=0;j<mode.count;++j)cJSON_AddItemToArray(options,cJSON_CreateString(mode.options[j]));
        cJSON_AddItemToArray(modes,o);
    }
    return sendJson(request,json);
}
// Up to five modes (distinct names) of two to eight distinct options, each 1-8 characters.
esp_err_t WifiPortal::wheelSave(httpd_req_t* request){
    if(!access(request)||!sameOrigin(request))return reply(request,"403 Forbidden","{\"message\":\"请在设备上开启手机配置后重试\"}");
    if(request->content_len<=0||request->content_len>3072)return reply(request,"400 Bad Request","{\"message\":\"设置内容过长\"}");
    std::unique_ptr<char[]> raw(new(std::nothrow) char[request->content_len+1]);std::unique_ptr<WheelBoard> board(new(std::nothrow) WheelBoard{});
    if(!raw||!board)return reply(request,"500 Internal Server Error","{\"message\":\"设备内存不足，请稍后重试\"}");
    int used=0;while(used<request->content_len){const int n=httpd_req_recv(request,raw.get()+used,request->content_len-used);if(n<=0)return ESP_FAIL;used+=n;}raw[used]=0;
    auto* json=cJSON_Parse(raw.get());const auto* modes=cJSON_GetObjectItemCaseSensitive(json,"modes");
    const auto text=[](const cJSON* v){return cJSON_IsString(v)&&WheelBoard::validText(v->valuestring,strlen(v->valuestring));};
    bool valid=cJSON_IsArray(modes)&&cJSON_GetArraySize(modes)<=int(WheelBoard::kModes);
    const cJSON* mode;
    if(valid)cJSON_ArrayForEach(mode,modes){
        const auto* name=cJSON_GetObjectItemCaseSensitive(mode,"n");const auto* options=cJSON_GetObjectItemCaseSensitive(mode,"o");
        if(!text(name)||board->find(name->valuestring)>=0||!cJSON_IsArray(options)||cJSON_GetArraySize(options)<2||cJSON_GetArraySize(options)>int(WheelBoard::kOptions)){valid=false;break;}
        auto& to=board->modes[board->count++];strcpy(to.name,name->valuestring);
        const cJSON* option;
        cJSON_ArrayForEach(option,options){
            if(!text(option)){valid=false;break;}
            for(unsigned i=0;i<to.count;++i)if(!strcmp(to.options[i],option->valuestring))valid=false;
            strcpy(to.options[to.count++],option->valuestring);
        }
        if(!valid)break;
    }
    cJSON_Delete(json);
    if(!valid)return reply(request,"400 Bad Request","{\"message\":\"转盘内容不正确：每个模式 2–8 个不重复的选项，每项最多 8 个字\"}");
    if(!SettingsStore::saveWheel(*board))return reply(request,"500 Internal Server Error","{\"message\":\"保存失败，请重试\"}");
    return reply(request,"200 OK","{\"saved\":true}");
}
void WifiPortal::phase(const char* phase,const char* message,const char* ssid,const char* ip){
    xSemaphoreTake(mutex_,portMAX_DELAY);snprintf(phase_,sizeof(phase_),"%s",phase);snprintf(message_,sizeof(message_),"%s",message);snprintf(target_,sizeof(target_),"%s",ssid);snprintf(ip_,sizeof(ip_),"%s",ip);xSemaphoreGive(mutex_);
}
void WifiPortal::begin(){enabled_=true;xSemaphoreTake(mutex_,portMAX_DELAY);scanned_=scanning_=false;scanError_=0;count_=0;xSemaphoreGive(mutex_);scanDone_=false;scanRequest_=true;dnsQueries_=0;phase("ready","请选择 WiFi");}
void WifiPortal::end(){
    enabled_=false;scanRequest_=false;dnsStop_=true;
    for(unsigned i=0;dnsTask_&&i<100;++i)vTaskDelay(pdMS_TO_TICKS(10));
}
bool WifiPortal::startDns(){
    if(dnsTask_)return true;
    const int fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(fd<0)return false;
    sockaddr_in local{};local.sin_family=AF_INET;local.sin_port=htons(53);local.sin_addr.s_addr=inet_addr("192.168.8.1");
    timeval timeout{0,200000};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    if(bind(fd,reinterpret_cast<sockaddr*>(&local),sizeof(local))){close(fd);return false;}
    dnsSocket_=fd;dnsStop_=false;
    if(xTaskCreate(dnsTask,"portal_dns",3072,this,3,&dnsTask_)!=pdPASS){dnsStop_=true;close(fd);dnsSocket_=-1;dnsTask_=nullptr;return false;}
    return true;
}
void WifiPortal::dnsTask(void* value){
    auto& self=*static_cast<WifiPortal*>(value);uint8_t query[512],answer[528];
    while(!self.dnsStop_){sockaddr_in peer{};socklen_t size=sizeof(peer);const int count=recvfrom(self.dnsSocket_,query,sizeof(query),0,reinterpret_cast<sockaddr*>(&peer),&size);if(count<=0)continue;
        const auto length=portal::dnsReply(query,count,answer,sizeof(answer));if(length){sendto(self.dnsSocket_,answer,length,0,reinterpret_cast<sockaddr*>(&peer),size);++self.dnsQueries_;}}
    close(self.dnsSocket_);self.dnsSocket_=-1;self.dnsTask_=nullptr;vTaskDelete(nullptr);
}
void WifiPortal::tick(bool apReady,bool joining){
    if(!enabled_||!apReady)return;
    if(dnsStop_)startDns();
    if(scanDone_.exchange(false)){
        auto* records=new(std::nothrow) wifi_ap_record_t[16]{};
        uint16_t count=16;const int result=records?esp_wifi_scan_get_ap_records(&count,records):ESP_ERR_NO_MEM;
        xSemaphoreTake(mutex_,portMAX_DELAY);scanning_=false;scanError_=result;
        if(!result){count_=0;for(unsigned i=0;i<count;++i){if(!records[i].ssid[0])continue;char hex[65];portal::encodeSsid(records[i].ssid,hex);bool duplicate=false;for(unsigned j=0;j<count_;++j)duplicate|=!strcmp(networks_[j].hex,hex);if(duplicate)continue;strcpy(networks_[count_].hex,hex);networks_[count_].rssi=records[i].rssi;networks_[count_].secure=records[i].authmode!=WIFI_AUTH_OPEN;++count_;}scanned_=true;}
        xSemaphoreGive(mutex_);delete[] records;
    }
    if(!joining&&scanRequest_.exchange(false)){
        xSemaphoreTake(mutex_,portMAX_DELAY);const bool running=scanning_;xSemaphoreGive(mutex_);
        if(!running){wifi_scan_config_t config{};config.show_hidden=false;const int result=esp_wifi_scan_start(&config,false);xSemaphoreTake(mutex_,portMAX_DELAY);scanning_=result==ESP_OK;scanError_=result;if(result)scanned_=true;xSemaphoreGive(mutex_);}
    }
}
esp_err_t WifiPortal::status(httpd_req_t* request){
    if(!access(request))return httpd_resp_send_err(request,HTTPD_403_FORBIDDEN,"setup only");
    auto* json=cJSON_CreateObject();if(!json)return ESP_FAIL;
    xSemaphoreTake(mutex_,portMAX_DELAY);cJSON_AddStringToObject(json,"phase",phase_);cJSON_AddStringToObject(json,"message",message_);char hex[65];portal::encodeSsid(reinterpret_cast<const uint8_t*>(target_),hex);cJSON_AddStringToObject(json,"ssid_hex",hex);cJSON_AddStringToObject(json,"ip",ip_);xSemaphoreGive(mutex_);
    cJSON_AddBoolToObject(json,"hotspot",enabled_);cJSON_AddStringToObject(json,"hotspot_name",name);cJSON_AddNumberToObject(json,"dns_queries",dnsQueries_);
    cJSON_AddNumberToObject(json,"setup_requests",setupRequests_);
    uint8_t location=0;nvs_handle_t settings;if(nvs_open("device",NVS_READONLY,&settings)==ESP_OK){nvs_get_u8(settings,"ip_location",&location);nvs_close(settings);}cJSON_AddBoolToObject(json,"ip_location_allowed",location==1);
    wifi_config_t config{};cJSON_AddBoolToObject(json,"hotspot_open",esp_wifi_get_config(WIFI_IF_AP,&config)==ESP_OK&&config.ap.authmode==WIFI_AUTH_OPEN&&config.ap.password[0]==0);
    cJSON_AddStringToObject(json,"firmware",esp_app_get_description()->version);
    SystemSettings saved{};SettingsStore store;cJSON_AddBoolToObject(json,"bluetooth_enabled",store.load(saved)&&saved.bluetoothEnabled);
    cJSON_AddStringToObject(json,"bluetooth_name",board::bluetoothName());
    auto* station=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");esp_netif_ip_info_t address{};if(station)esp_netif_get_ip_info(station,&address);char stationIp[16];snprintf(stationIp,sizeof(stationIp),IPSTR,IP2STR(&address.ip));cJSON_AddStringToObject(json,"station_ip",stationIp);
    auto* route=esp_netif_get_default_netif();const char* key=route?esp_netif_get_ifkey(route):"";
    cJSON_AddStringToObject(json,"route_key",key);
    cJSON_AddStringToObject(json,"route",!strcmp(key,"WIFI_STA_DEF")?"wifi":!strcmp(key,"PASSPORT_PPP")?"ppp":"none");
    auto* raw=cJSON_PrintUnformatted(json);cJSON_Delete(json);if(!raw)return ESP_FAIL;httpd_resp_set_type(request,"application/json");httpd_resp_set_hdr(request,"Cache-Control","no-store");const auto result=httpd_resp_sendstr(request,raw);cJSON_free(raw);return result;
}
esp_err_t WifiPortal::get(httpd_req_t* request){
    if(!access(request))return httpd_resp_send_err(request,HTTPD_403_FORBIDDEN,"enable setup on device");
    if(!strcmp(request->uri,"/wifi/status"))return status(request);
    if(!strcmp(request->uri,"/identity/data"))return identityData(request);
    if(!strcmp(request->uri,"/media/prefs"))return mediaPrefs(request);
    if(!strcmp(request->uri,"/rates/prefs"))return ratePrefs(request);
    if(!strcmp(request->uri,"/podcast/list"))return podcastList(request);
    if(!strncmp(request->uri,"/podcast/search?",16))return podcastSearch(request);
    if(!strcmp(request->uri,"/stock/list"))return stockList(request);
    if(!strncmp(request->uri,"/stock/search?",14))return stockSearch(request);
    if(!strcmp(request->uri,"/wheel/list"))return wheelList(request);
    httpd_resp_set_hdr(request,"Cache-Control","no-store");
    if(!strncmp(request->uri,"/wifi/scan",10)){
        auto* json=cJSON_CreateObject();if(!json)return ESP_FAIL;auto* array=cJSON_AddArrayToObject(json,"networks");
        xSemaphoreTake(mutex_,portMAX_DELAY);const bool refresh=strstr(request->uri,"refresh=1");if(refresh||!scanned_)scanRequest_=true;cJSON_AddBoolToObject(json,"scanning",scanning_||scanRequest_);cJSON_AddNumberToObject(json,"error",scanError_);
        for(unsigned i=0;i<count_;++i){auto* item=cJSON_CreateObject();cJSON_AddStringToObject(item,"ssid_hex",networks_[i].hex);cJSON_AddNumberToObject(item,"rssi",networks_[i].rssi);cJSON_AddBoolToObject(item,"secure",networks_[i].secure);cJSON_AddItemToArray(array,item);}xSemaphoreGive(mutex_);
        auto* raw=cJSON_PrintUnformatted(json);cJSON_Delete(json);if(!raw)return ESP_FAIL;httpd_resp_set_type(request,"application/json");const auto result=httpd_resp_sendstr(request,raw);cJSON_free(raw);return result;
    }
    char host[80]{};httpd_req_get_hdr_value_str(request,"Host",host,sizeof(host));
    if(strcmp(request->uri,"/")||(!board::authorized(request)&&strcmp(host,"192.168.8.1")&&strcmp(host,"192.168.8.1:80"))){httpd_resp_set_status(request,"303 See Other");httpd_resp_set_hdr(request,"Location","http://192.168.8.1/");return httpd_resp_sendstr(request,"Open AI Passport WiFi setup");}
    httpd_resp_set_type(request,"text/html;charset=utf-8");return httpd_resp_send(request,portal::page,sizeof(portal::page)-1);
}
}
