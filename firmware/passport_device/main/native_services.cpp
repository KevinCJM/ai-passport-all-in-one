#include "tool_tasks.hpp"
#include "native_services.hpp"
#include "board.hpp"
#include "network_policy.hpp"
#include "word_list.hpp"
#include "lunar.hpp"
#include "voice_add.hpp"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_netif_sntp.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "esp_tls.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>

extern "C" unsigned passport_last_failed_allocation();
namespace passport {
namespace {
// XiaoZhi's settings check, as its own firmware calls it (CONFIG_OTA_URL); "activate" is appended to poll a binding.
constexpr char kXiaozhiOta[]="https://api.tenclass.net/xiaozhi/ota/";
const cJSON* item(const cJSON* j,const char* k){return cJSON_GetObjectItemCaseSensitive(j,k);}
const char* str(const cJSON* j,const char* k){const auto* v=item(j,k);return cJSON_IsString(v)?v->valuestring:"";}
double num(const cJSON* j,const char* k){const auto* v=item(j,k);return cJSON_IsNumber(v)?v->valuedouble:NAN;}
template<size_t N> void copy(char(&to)[N],const char* from){size_t n=std::min(strlen(from),N-1);while(n&&(static_cast<unsigned char>(from[n])&0xc0)==0x80)--n;memcpy(to,from,n);to[n]=0;}
struct Json{cJSON* value{};~Json(){cJSON_Delete(value);}Json()=default;explicit Json(cJSON* v):value(v){};Json(const Json&)=delete;Json& operator=(const Json&)=delete;};
bool nvsString(nvs_handle_t h,const char* key,char* out,size_t capacity){size_t n=capacity;return nvs_get_str(h,key,out,&n)==ESP_OK&&n>1&&n<=capacity;}
// Curly quotes and dashes become ASCII so the English font has every glyph.
void asciiPunctuation(char* text){for(char* c=text;*c;++c)if(!strncmp(c,"\xe2\x80",2)&&c[2]){const char m=c[2];memmove(c+1,c+3,strlen(c+3)+1);*c=m=='\x98'||m=='\x99'?'\'':m=='\x9c'||m=='\x9d'?'"':'-';}}
// In place: "%3F" -> "?" (Open Trivia DB's url3986 encoding).
void percentDecode(char* text){
    char* out=text;
    for(const char* c=text;*c;++c){if(c[0]=='%'&&isxdigit(static_cast<unsigned char>(c[1]))&&isxdigit(static_cast<unsigned char>(c[2]))){const char hex[3]{c[1],c[2],0};*out++=static_cast<char>(strtol(hex,nullptr,16));c+=2;}else *out++=*c;}
    *out=0;
}
template<size_t N> void decoded(char(&to)[N],const char* from){copy(to,from);percentDecode(to);asciiPunctuation(to);}
void urlEncode(char* out,size_t capacity,const char* text);  // below
}
NativeServices::NativeServices(AudioPipe& audio,Reply reply,void* context):audio_(audio),reply_(reply),context_(context){
    actions_=xQueueCreate(4,sizeof(Job));configASSERT(actions_);nvs_handle_t h;uint8_t allowed=0;
    if(nvs_open("device",NVS_READONLY,&h)==ESP_OK){nvs_get_u8(h,"ip_location",&allowed);nvs_close(h);}allowed_=allowed==1;
}
bool NativeServices::allowLocation(bool allowed){
    nvs_handle_t h;if(nvs_open("device",NVS_READWRITE,&h)!=ESP_OK)return false;
    const bool ok=nvs_set_u8(h,"ip_location",allowed)==ESP_OK&&nvs_commit(h)==ESP_OK;nvs_close(h);if(ok){allowed_=allowed;weatherAt_=0;}return ok;
}
void NativeServices::publish(const Job& job,const char* state,const char* phase,const char* value,const char* const* lines){
    if(!strcmp(job.operation,"probe"))return; // Owner diagnostics never replace the foreground UI or start audio.
    if(desired_.load()!=job.generation)return;
    Json j(cJSON_CreateObject());if(!j.value)return;
    cJSON_AddNumberToObject(j.value,"app",static_cast<unsigned>(job.app));cJSON_AddNumberToObject(j.value,"generation",job.generation);cJSON_AddNumberToObject(j.value,"request",job.request);
    cJSON_AddBoolToObject(j.value,"stream",true);cJSON_AddStringToObject(j.value,"state",state);cJSON_AddStringToObject(j.value,"phase",phase);cJSON_AddStringToObject(j.value,"value",value);
    auto* rows=cJSON_AddArrayToObject(j.value,"lines");for(unsigned i=0;i<4;++i)cJSON_AddItemToArray(rows,cJSON_CreateString(lines&&lines[i]?lines[i]:""));
    if(time(nullptr)>1700000000)cJSON_AddNumberToObject(j.value,"epoch",time(nullptr));
    if(*notice_){cJSON_AddStringToObject(j.value,"notice",notice_);notice_[0]=0;}
    if(*city_){cJSON_AddStringToObject(j.value,"city",city_);city_[0]=0;}
    if(select_>=0){cJSON_AddNumberToObject(j.value,"select",select_);select_=-1;}
    if(job.app==App::Radio){auto* m=cJSON_AddObjectToObject(j.value,"media");cJSON_AddNumberToObject(m,"index",mediaIndex_);cJSON_AddNumberToObject(m,"count",mediaCount_);}
    if(job.app==App::Radio||job.app==App::Podcast)cJSON_AddBoolToObject(j.value,"playing",playing_);
    if(job.app==App::Podcast){auto* p=cJSON_AddObjectToObject(j.value,"podcast");
        if(podcastPlayer_){cJSON_AddNumberToObject(p,"pos",podcastPos_);cJSON_AddNumberToObject(p,"dur",podcastDur_);}
        else{auto* list=cJSON_AddArrayToObject(p,"items");for(unsigned i=0;i<podcastCount_;++i){auto* o=cJSON_CreateObject();cJSON_AddStringToObject(o,"n",page_.podcasts[i].name);cJSON_AddStringToObject(o,"s",page_.podcasts[i].note);cJSON_AddItemToArray(list,o);}}}
    if(job.app==App::Chat){cJSON_AddNumberToObject(j.value,"speech_turn",speechTurn_);}
    // A phrase spoken on 汇率 or 自选股 sends only its own state: what the page shows came from the last fetch.
    const bool spoken=!strcmp(job.operation,"speak");
    if(job.app==App::Rate&&!strcmp(phase,"ready")&&!spoken){auto* list=cJSON_AddArrayToObject(j.value,"rates");cJSON_AddStringToObject(j.value,"base",rateBoard_.base);cJSON_AddNumberToObject(j.value,"updated",rateUpdated_);
        for(unsigned i=0;i<RateBoard::kTargets;++i)if(*rateBoard_.targets[i]&&rates_[i]>0){auto* o=cJSON_CreateObject();cJSON_AddStringToObject(o,"c",rateBoard_.targets[i]);cJSON_AddNumberToObject(o,"v",rates_[i]);cJSON_AddItemToArray(list,o);}}
    // 财经新闻: [time, title] of each; item i's text in full.
    if(job.app==App::News&&!strcmp(phase,"ready")&&news_){auto* list=cJSON_AddArrayToObject(j.value,"news");cJSON_AddNumberToObject(j.value,"page",newsPage_);
        for(unsigned i=0;i<newsCount_;++i){auto* o=cJSON_CreateObject();cJSON_AddStringToObject(o,"t",news_[i].time);cJSON_AddStringToObject(o,"h",news_[i].title);cJSON_AddItemToArray(list,o);}
        if(newsIndex_<newsCount_){cJSON_AddNumberToObject(j.value,"i",newsIndex_);cJSON_AddStringToObject(j.value,"d",news_[newsIndex_].digest);}}
    if(job.app==App::History&&!strcmp(phase,"ready")){auto* list=cJSON_AddArrayToObject(j.value,"events");
        for(unsigned i=0;i<eventCount_;++i){auto* o=cJSON_CreateObject();cJSON_AddNumberToObject(o,"y",events_[i].year);cJSON_AddNumberToObject(o,"k",events_[i].type);cJSON_AddStringToObject(o,"t",events_[i].title);cJSON_AddItemToArray(list,o);}}
    if(job.app==App::Daily&&*daily_.en){auto* d=cJSON_AddObjectToObject(j.value,"daily");
        cJSON_AddStringToObject(d,"en",daily_.en);cJSON_AddStringToObject(d,"cn",daily_.cn);cJSON_AddStringToObject(d,"date",daily_.date);cJSON_AddNumberToObject(d,"back",daily_.back);}
    if(job.app==App::Quake&&!strcmp(phase,"ready")){auto* list=cJSON_AddArrayToObject(j.value,"quakes");
        for(unsigned i=0;i<quakeCount_;++i){const auto& q=page_.quakes[i];auto* o=cJSON_CreateObject();cJSON_AddNumberToObject(o,"at",q.at);cJSON_AddNumberToObject(o,"m",q.mag);cJSON_AddNumberToObject(o,"d",q.depth);cJSON_AddStringToObject(o,"p",q.place);cJSON_AddItemToArray(list,o);}}
    if(job.app==App::Hacker&&!strcmp(phase,"ready")){auto* list=cJSON_AddArrayToObject(j.value,"stories");
        for(unsigned i=0;i<storyCount_;++i){const auto& s=page_.stories[i];auto* o=cJSON_CreateObject();cJSON_AddStringToObject(o,"t",s.title);cJSON_AddStringToObject(o,"s",s.site);cJSON_AddNumberToObject(o,"p",s.points);cJSON_AddNumberToObject(o,"c",s.comments);cJSON_AddNumberToObject(o,"at",s.at);cJSON_AddItemToArray(list,o);}}
    if(job.app==App::Quiz&&!strcmp(phase,"ready")&&page_.quiz.serial){const auto& q=page_.quiz;auto* o=cJSON_AddObjectToObject(j.value,"quiz");
        cJSON_AddNumberToObject(o,"n",q.serial);cJSON_AddStringToObject(o,"q",q.question);cJSON_AddStringToObject(o,"cat",q.category);cJSON_AddStringToObject(o,"lv",q.level);cJSON_AddNumberToObject(o,"c",q.correct);
        auto* answers=cJSON_AddArrayToObject(o,"a");for(auto& a:q.answers)cJSON_AddItemToArray(answers,cJSON_CreateString(a));}
    if(job.app==App::Macro&&!spoken&&page_.macro.valid()){
        const auto& m=page_.macro;auto* o=cJSON_AddObjectToObject(j.value,"macro");cJSON_AddNumberToObject(o,"at",m.updated);cJSON_AddNumberToObject(o,"g",m.group);
        const auto value=[](float v){return std::isfinite(v)?cJSON_CreateNumber(std::round(double(v)*10000)/10000):cJSON_CreateNull();};
        auto* rows=cJSON_AddArrayToObject(o,"r");
        for(unsigned i=0;i<m.count;++i){const auto& r=m.rows[i];auto* a=cJSON_CreateArray();cJSON_AddItemToArray(a,cJSON_CreateNumber(r.series));cJSON_AddItemToArray(a,value(r.value));cJSON_AddItemToArray(a,value(r.previous));cJSON_AddItemToArray(a,cJSON_CreateNumber(r.date));cJSON_AddItemToArray(rows,a);}
        if(m.points&&m.chartFor<m.count){auto* c=cJSON_AddObjectToObject(o,"c");cJSON_AddNumberToObject(c,"i",m.chartFor);cJSON_AddItemToObject(c,"lo",value(m.low));cJSON_AddItemToObject(c,"hi",value(m.high));cJSON_AddNumberToObject(c,"from",m.from);cJSON_AddNumberToObject(c,"zoom",m.zoomFrom);cJSON_AddNumberToObject(c,"to",m.to);
            int points[60];for(unsigned i=0;i<m.points;++i)points[i]=m.chart[i];cJSON_AddItemToObject(c,"p",cJSON_CreateIntArray(points,m.points));}
    }
    if(job.app==App::Stock&&!strcmp(phase,"ready")&&!spoken){
        // Values rounded to what the page shows keep cJSON from printing 17 digits of float noise.
        const auto& s=page_.stocks;auto* o=cJSON_AddObjectToObject(j.value,"stock");cJSON_AddNumberToObject(o,"t",s.updated);
        const auto value=[](float v,int places){const double scale=std::pow(10.0,places);return std::isfinite(v)?cJSON_CreateNumber(std::round(double(v)*scale)/scale):cJSON_CreateNull();};
        auto* rows=cJSON_AddArrayToObject(o,"r");
        const auto add=[&](const auto& r){
            auto* a=cJSON_CreateArray();cJSON_AddItemToArray(a,cJSON_CreateString(r.name));cJSON_AddItemToArray(a,cJSON_CreateString(r.code));cJSON_AddItemToArray(a,cJSON_CreateString(r.tag));
            cJSON_AddItemToArray(a,cJSON_CreateNumber(r.decimals));
            for(unsigned i=0;i<8;++i)cJSON_AddItemToArray(a,value(r.v[i],i==2?2:i==7?0:r.decimals));
            cJSON_AddItemToArray(rows,a);
        };
        if(s.chartFor<0)for(unsigned i=0;i<s.count;++i)add(s.rows[i]);
        else{
            cJSON_AddNumberToObject(o,"i",s.chartFor);add(s.rows[s.chartFor]);
            auto* c=cJSON_AddObjectToObject(o,"c");cJSON_AddNumberToObject(c,"i",s.chartFor);cJSON_AddBoolToObject(c,"k",s.days);cJSON_AddNumberToObject(c,"n",s.chartTotal);
            cJSON_AddItemToObject(c,"lo",value(s.low,4));cJSON_AddItemToObject(c,"hi",value(s.high,4));
            cJSON_AddStringToObject(c,"from",s.from);cJSON_AddStringToObject(c,"mid",s.mid);cJSON_AddStringToObject(c,"to",s.to);
            int points[ChartPoints::kMax];for(unsigned i=0;i<s.chartCount;++i)points[i]=s.chart[i];cJSON_AddItemToObject(c,"p",cJSON_CreateIntArray(points,s.chartCount));
        }
    }
    if(job.app==App::Forecast&&!strcmp(phase,"ready")){const auto& f=page_.forecast;auto* o=cJSON_AddObjectToObject(j.value,"forecast");
        cJSON_AddStringToObject(o,"city",f.city);cJSON_AddNumberToObject(o,"h0",f.hour0);
        auto* days=cJSON_AddArrayToObject(o,"d");for(unsigned i=0;i<f.days;++i){const auto& d=f.day[i];const int v[]{d.low,d.high,d.code,d.rain};cJSON_AddItemToArray(days,cJSON_CreateIntArray(v,4));}
        auto* hours=cJSON_AddArrayToObject(o,"h");for(unsigned i=0;i<f.hours;++i){const auto& h=f.hour[i];const int v[]{h.temp,h.code,h.rain};cJSON_AddItemToArray(hours,cJSON_CreateIntArray(v,3));}
        int air[24];for(unsigned i=0;i<f.airHours;++i)air[i]=f.aqi[i];cJSON_AddItemToObject(o,"air",cJSON_CreateIntArray(air,f.airHours));
        const int more[]{f.primary,f.pm25,f.pm10,f.sunrise,f.sunset,f.uv};cJSON_AddItemToObject(o,"x",cJSON_CreateIntArray(more,6));
        // Warnings: [level, places, kind, where, "MM/DD HH:MM"], left out when they could not be read.
        if(f.warnings!=0xff){auto* list=cJSON_AddArrayToObject(o,"warn");for(unsigned i=0;i<f.warnings;++i){const auto& w=f.warning[i];auto* row=cJSON_CreateArray();
            cJSON_AddItemToArray(row,cJSON_CreateNumber(w.level));cJSON_AddItemToArray(row,cJSON_CreateNumber(w.places));cJSON_AddItemToArray(row,cJSON_CreateString(w.type));
            cJSON_AddItemToArray(row,cJSON_CreateString(w.where));cJSON_AddItemToArray(row,cJSON_CreateString(w.time));cJSON_AddItemToArray(list,row);}}
        // One warning's text: [index, text].
        if(f.warnings!=0xff&&f.detailFor<f.warnings){auto* d=cJSON_AddArrayToObject(o,"detail");cJSON_AddItemToArray(d,cJSON_CreateNumber(f.detailFor));cJSON_AddItemToArray(d,cJSON_CreateString(f.detail));}}
    // 台风: rows [number, name, active]; the shown row's track flat (lon, lat, grade) and forecast (lon, lat, grade,
    // hours); x: [epoch, pressure, lowest pressure, wind, speed, peak wind, peak grade]; c: [city, lon, lat].
    if(job.app==App::Typhoon&&!strcmp(phase,"ready")){const auto& t=page_.typhoon;auto* o=cJSON_AddObjectToObject(j.value,"typhoon");
        auto* rows=cJSON_AddArrayToObject(o,"l");
        for(unsigned i=0;i<t.count;++i){auto* r=cJSON_CreateArray();cJSON_AddItemToArray(r,cJSON_CreateString(t.rows[i].number));cJSON_AddItemToArray(r,cJSON_CreateString(t.rows[i].name));cJSON_AddItemToArray(r,cJSON_CreateNumber(t.rows[i].active));cJSON_AddItemToArray(rows,r);}
        if(t.shown<t.count){
            int v[TyphoonView::kTrack*4];cJSON_AddNumberToObject(o,"i",t.shown);
            for(unsigned i=0;i<t.points;++i){v[3*i]=t.track[i].lon;v[3*i+1]=t.track[i].lat;v[3*i+2]=t.track[i].grade;}cJSON_AddItemToObject(o,"t",cJSON_CreateIntArray(v,3*t.points));
            for(unsigned i=0;i<t.ahead;++i){const auto& a=t.forecast[i];v[4*i]=a.lon;v[4*i+1]=a.lat;v[4*i+2]=a.grade;v[4*i+3]=a.hours;}cJSON_AddItemToObject(o,"f",cJSON_CreateIntArray(v,4*t.ahead));
            auto* x=cJSON_AddArrayToObject(o,"x");for(const double n:{double(t.at),double(t.pressure),double(t.lowest),double(t.wind),double(t.speed),double(t.peakWind),double(t.peakGrade)})cJSON_AddItemToArray(x,cJSON_CreateNumber(n));
            cJSON_AddStringToObject(o,"d",t.heading);
        }
        if(t.cityLat!=INT16_MIN){auto* c=cJSON_AddArrayToObject(o,"c");cJSON_AddItemToArray(c,cJSON_CreateString(t.city));cJSON_AddItemToArray(c,cJSON_CreateNumber(t.cityLon));cJSON_AddItemToArray(c,cJSON_CreateNumber(t.cityLat));}
    }
    // 百度热搜: rows [title cut to the list, heat, tag]; item i in full; at: when fetched.
    if(job.app==App::Hot&&!strcmp(phase,"ready")&&hot_){auto* o=cJSON_AddObjectToObject(j.value,"hot");auto* rows=cJSON_AddArrayToObject(o,"r");
        for(unsigned i=0;i<hotCount_;++i){char title[40];copy(title,hot_[i].title);auto* r=cJSON_CreateArray();cJSON_AddItemToArray(r,cJSON_CreateString(title));
            cJSON_AddItemToArray(r,cJSON_CreateNumber(hot_[i].score));cJSON_AddItemToArray(r,cJSON_CreateNumber(hot_[i].tag));cJSON_AddItemToArray(rows,r);}
        if(hotIndex_<hotCount_){cJSON_AddNumberToObject(o,"i",hotIndex_);cJSON_AddStringToObject(o,"t",hot_[hotIndex_].title);cJSON_AddStringToObject(o,"d",hot_[hotIndex_].desc);}
        cJSON_AddNumberToObject(o,"at",hotUpdated_);
    }
    if(job.app==App::Home&&weatherReading_.known){
        auto* w=cJSON_AddObjectToObject(j.value,"weather");const auto& data=weatherReading_;
        cJSON_AddStringToObject(w,"city",data.city);cJSON_AddStringToObject(w,"condition",data.condition);
        cJSON_AddNumberToObject(w,"temperature",data.temperature);cJSON_AddNumberToObject(w,"minimum",data.minimum);cJSON_AddNumberToObject(w,"maximum",data.maximum);
        cJSON_AddNumberToObject(w,"humidity",data.humidity);cJSON_AddNumberToObject(w,"code",data.code);if(data.airKnown)cJSON_AddNumberToObject(w,"aqi",data.aqi);
        if(data.warningLevel>=0){cJSON_AddStringToObject(w,"warn",data.warning);cJSON_AddNumberToObject(w,"warn_level",data.warningLevel);}
    }
    char* raw=cJSON_PrintUnformatted(j.value);if(raw){reply_(context_,raw);free(raw);}
}
void NativeServices::status(const char* state,const char* phase){
    failed_=!strcmp(phase,"error");
    // 语音笔记: value tells the page whether the device is listening (rec) or waiting for the text (asr).
    if(transcribing()){publish(current_,state,phase,listening_?"rec":asr_?"asr":"");return;}
    const char* lines[]{stt_,answer_,"自动收音 · ","确认：打断 / 重试"};publish(current_,state,phase,"小智语音",lines);
}
bool NativeServices::request(App app,uint32_t generation,uint32_t request,const char* operation){
    if(app==App::Macro&&strncmp(operation,"list:",5)&&strncmp(operation,"chart:",6))return false;
    Job job{app,generation,request,{}};copy(job.operation,operation);desired_=generation;if(app==App::Macro)macroRequest_=request;
    if(!network_&&app!=App::Macro){const char* lines[]{"请先连接 WiFi，或使用 USB PPP","","",""};publish(job,"未联网",app==App::Home?"ready":"error","--",lines);return true;}
    // Only these tasks keep running and read actions_; a one-shot fetch would drop the action, so the request
    // waits as the next job instead and its answer replaces the running fetch's (whose request is then stale).
    const bool reads=app==App::Chat||app==App::Radio||app==App::Podcast||app==App::Words||app==App::Daily||app==App::Quiz||app==App::Hot||app==App::News||keepsListening(app)||speaksOnce(app)||(speaksToo(app)&&!strcmp(operation,"toggle"));
    if(reads&&task_&&!finished_&&owner_.load()==generation)return xQueueSend(actions_,&job,0)==pdTRUE;
    pending_=job;pendingReady_=true;pendingSince_=esp_timer_get_time();memoryWaiting_=false;return true;
}
void NativeServices::poll(){
    if(task_&&finished_.load()){vTaskDelete(task_);task_=nullptr;finished_=false;busy_=false;}
    // 语音唤醒 holds about 57 KB while it listens; main stops it first (LiveServices::nativeBusy), then this starts.
    if(!task_&&pendingReady_&&!tools::wakeHolding()&&(uiPreparedFor(pending_.app)||(!strcmp(pending_.operation,"probe")&&uiPreparedFor(App::Home)))){current_=pending_;owner_=current_.generation;finished_=false;xQueueReset(actions_);
        // Radio and Words stream plain HTTP (openStream refuses TLS) and measured 3.3 KiB peak; the HTTPS pages
        // measured 3.3-4.9 KiB (stock chart the most), so 10 KiB keeps double that.
        const unsigned stack=current_.app==App::Chat||transcribing()?32768:current_.app==App::Radio||current_.app==App::Podcast||current_.app==App::Words?8192:10240;
        if(xTaskCreate(run,"native_online",stack,this,3,&task_)!=pdPASS){
            task_=nullptr;
            // NimBLE's deleted task stack is reclaimed by the idle task.
            if(esp_timer_get_time()-pendingSince_<500000){if(!memoryWaiting_){publish(current_,"正在释放连接资源","loading");memoryWaiting_=true;}}
            else{pendingReady_=false;error_=ESP_ERR_NO_MEM;publish(current_,"内存不足","error");}
        }else{pendingReady_=false;busy_=true;memoryWaiting_=false;error_=0;}
    }
}
void NativeServices::run(void* context){auto& self=*static_cast<NativeServices*>(context);self.work();self.closeSocket();if((self.current_.app==App::Chat&&strcmp(self.current_.operation,"probe"))||self.transcribing()||self.current_.app==App::Radio||self.current_.app==App::Podcast||self.current_.app==App::Words||self.current_.app==App::Daily)self.audio_.stop();self.ready_=self.listening_=self.speaking_=self.playing_=false;self.finished_=true;vTaskSuspend(nullptr);}
void NativeServices::work(){if(cancelled())return;if(!strcmp(current_.operation,"speak"))memo();else if(!strcmp(current_.operation,"locate"))locate();else if(current_.app==App::Home)home();else if(current_.app==App::Chat)chat();else if(current_.app==App::Radio)media();else if(current_.app==App::Podcast)podcast();else if(current_.app==App::Words)words();else if(current_.app==App::News)news();else if(current_.app==App::Rate)rates();else if(current_.app==App::History)history();else if(current_.app==App::Daily)daily();else if(current_.app==App::Quake)quake();else if(current_.app==App::Quiz)quiz();else if(current_.app==App::Hacker)hacker();else if(current_.app==App::Forecast)forecast();else if(current_.app==App::Stock)stock();else if(current_.app==App::Macro)macro();else if(current_.app==App::Typhoon)typhoon();else if(current_.app==App::Hot)hot();else if(transcribing())memo();stackFree_=uxTaskGetStackHighWaterMark(nullptr);}
bool NativeServices::syncClock(){
    // Once per boot even when the clock is set: it drifts in deep sleep (internal RC oscillator), and the alarm
    // wakes the device early to correct it before ringing.
    static bool tried=false;
    if(tried&&time(nullptr)>1700000000)return true;
    esp_sntp_config_t config=ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    if(esp_netif_sntp_init(&config)!=ESP_OK)return time(nullptr)>1700000000;
    bool synced=false;
    for(unsigned i=0;i<24&&!cancelled()&&!synced;++i)synced=esp_netif_sntp_sync_wait(pdMS_TO_TICKS(250))==ESP_OK;
    tried=synced||time(nullptr)>1700000000;
    esp_netif_sntp_deinit();return time(nullptr)>1700000000;
}
bool NativeServices::retryPause(unsigned attempt){
    const auto until=esp_timer_get_time()+attempt*800000;
    while(!cancelled()&&esp_timer_get_time()<until)vTaskDelay(pdMS_TO_TICKS(20));
    return !cancelled();
}
bool NativeServices::fetch(const char* url,char* output,size_t capacity,Sink sink,void* context,const char* post,const char* userAgent,const char* const* headers){
    for(unsigned attempt=1;attempt<=networkAttempts&&!cancelled();++attempt){
    struct Body{NativeServices* self;char* bytes;size_t used,capacity;bool invalid,done;Sink sink;void* context;} body{this,output,0,capacity,false,false,sink,context};
    if(output){output[0]=0;}
    if(sink){sink(context,nullptr,0);}
    esp_http_client_config_t config{};config.url=url;config.timeout_ms=12000;config.buffer_size=1024;config.buffer_size_tx=512;config.max_redirection_count=5;config.addr_type=HTTP_ADDR_TYPE_INET;config.crt_bundle_attach=esp_crt_bundle_attach;config.user_data=&body;config.user_agent=userAgent;
    config.event_handler=[](esp_http_client_event_t* e)->esp_err_t{auto& b=*static_cast<Body*>(e->user_data);if(b.self->cancelled())return ESP_FAIL;if(e->event_id==HTTP_EVENT_ON_DATA&&e->data_len>0&&b.sink){if(esp_http_client_get_status_code(e->client)!=200)return ESP_OK;b.used+=e->data_len;if(!b.sink(b.context,static_cast<const char*>(e->data),e->data_len)){b.done=true;return ESP_FAIL;}return ESP_OK;}
        if(e->event_id==HTTP_EVENT_ON_DATA&&e->data_len>0){if(b.used+e->data_len>=b.capacity){b.invalid=true;return ESP_FAIL;}memcpy(b.bytes+b.used,e->data,e->data_len);b.used+=e->data_len;b.bytes[b.used]=0;}return ESP_OK;};
    if(post)config.method=HTTP_METHOD_POST;
    auto client=esp_http_client_init(&config);if(!client){error_=ESP_ERR_NO_MEM;return false;}
    if(headers)for(unsigned i=0;headers[i]&&headers[i+1];i+=2)esp_http_client_set_header(client,headers[i],headers[i+1]);
    if(post){esp_http_client_set_header(client,"Content-Type","application/json");esp_http_client_set_post_field(client,post,strlen(post));}
    ++httpCount_;const auto result=esp_http_client_perform(client);const int code=esp_http_client_get_status_code(client);httpStatus_=code;error_=result;int tlsCode=0,tlsFlags=0;tlsResult_=esp_http_client_get_and_clear_last_tls_error(client,&tlsCode,&tlsFlags);tlsError_=tlsCode;tlsFlags_=tlsFlags;socketError_=esp_http_client_get_errno(client);esp_http_client_cleanup(client);
    if(!cancelled()&&(body.done||(!body.invalid&&result==ESP_OK&&code==200&&body.used>0)))return true;
    if(body.invalid||!retryNetwork(attempt,result,tlsResult_.load(),tlsFlags,socketError_.load(),code)||!retryPause(attempt))return false;
    ++httpRetries_;
    }
    return false;
}
void NativeServices::home(){
    publish(current_,"网络校时","loading");
    if(!syncClock()){const char* rows[]{"","网络时间同步失败","确认键重试",""};publish(current_,"校时失败","ready","--:--",rows);return;}
    // Weather follows the city set on the setup page or in Settings (spoken, or looked up by IP when asked to).
    MediaPrefs prefs{};const bool chosen=mediaLoad("prefs",prefs)&&*prefs.city;const uint32_t revision=mediaPrefsRevision;
    const char* source="城市天气";
    if(!chosen){const char* rows[]{"","尚未选择城市","系统设置 > 所在城市",""};publish(current_,"本地时钟","ready","",rows);return;}
    if(weatherAt_&&weatherPrefs_==revision&&esp_timer_get_time()/1000000-weatherAt_<600){const char* rows[]{weather_[0],weather_[1],weather_[2],weather_[3]};publish(current_,source,"ready","",rows);return;}
    const char* loading[]{"","正在更新天气","",""};publish(current_,"天气更新中","loading","",loading);
    auto data=std::unique_ptr<char[]>(new(std::nothrow)char[4096]);if(!data){error_=ESP_ERR_NO_MEM;return;}
    double latitude,longitude;char city[80]{};
    latitude=prefs.latitude;longitude=prefs.longitude;copy(city,prefs.city);
    char url[512];snprintf(url,sizeof(url),"https://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f&current=temperature_2m,relative_humidity_2m,weather_code&daily=temperature_2m_max,temperature_2m_min&forecast_days=1&timezone=auto",latitude,longitude);
    if(!fetch(url,data.get(),4096)){const char* rows[]{"","天气请求失败","确认键重试",""};publish(current_,"天气暂不可用","ready","",rows);return;}
    {Json forecast(cJSON_Parse(data.get()));const auto* current=item(forecast.value,"current");const auto* daily=item(forecast.value,"daily");const auto* low=cJSON_GetArrayItem(item(daily,"temperature_2m_min"),0);const auto* high=cJSON_GetArrayItem(item(daily,"temperature_2m_max"),0);
     const double temperature=num(current,"temperature_2m"),humidity=num(current,"relative_humidity_2m"),code=num(current,"weather_code");
     if(!std::isfinite(temperature)||!std::isfinite(humidity)||humidity<0||humidity>100||!std::isfinite(code)||!cJSON_IsNumber(low)||!cJSON_IsNumber(high)||low->valuedouble>high->valuedouble){publish(current_,"天气数据无效","ready");return;}
     if(!weatherReading_.update(city,weatherName(code),temperature,low->valuedouble,high->valuedouble,humidity,code,NAN)){publish(current_,"天气数据无效","ready");return;}
     snprintf(weather_[1],sizeof(weather_[1]),"%s %s",city,weatherName(code));snprintf(weather_[2],sizeof(weather_[2]),"%.1f°C 低%.0f° 高%.0f°",temperature,low->valuedouble,high->valuedouble);snprintf(weather_[3],sizeof(weather_[3]),"湿度 %.0f%% · AQI --",humidity);}
    data.reset();
    if(WeatherForecast reading{};air(latitude,longitude,reading)){const int aqi=reading.aqi[0];weatherReading_.aqi=aqi;weatherReading_.airKnown=true;char* start=strstr(weather_[3],"AQI --");if(start)snprintf(start,sizeof(weather_[3])-(start-weather_[3]),"AQI %d",aqi);}
    // The highest warning in force beside the city.
    {WeatherWarning list[2];uint8_t count=0;const bool read=warnings(prefs,list,count,std::size(list));
     weatherReading_.warningLevel=read&&count?list[0].level:-1;copy(weatherReading_.warning,read&&count?list[0].type:"");}
    if(cancelled()){return;}
    weatherAt_=esp_timer_get_time()/1000000;weatherPrefs_=revision;++weatherUpdates_;const char* rows[]{weather_[0],weather_[1],weather_[2],weather_[3]};publish(current_,source,"ready","",rows);
}
// 自选股: Tencent over plain HTTP, no key. The list's quotes in one request (about 400 bytes each); with
// "chart:<row>:<days>" also that row's minutes today (read as they arrive: 10-15 KB) or its last 30 days.
void NativeServices::stock(){
    publish(current_,"正在获取行情","loading");
    StockList list;if(!mediaLoad("stocks",list)||!list.count||list.count>StockList::kMax)list=defaultStocks();
    // "del:<row>": the list without that row first.
    if(unsigned row=0;sscanf(current_.operation,"del:%u",&row)==1&&row<list.count&&list.count>1){
        char name[24];copy(name,list.items[row].name);
        std::copy(list.items+row+1,list.items+list.count,list.items+row);list.items[--list.count]={};
        if(mediaSave("stocks",list)){snprintf(notice_,sizeof(notice_),"已删除%s",name);select_=std::min<int>(row,list.count-1);}else copy(notice_,"删除失败");
    }
    int row=-1;unsigned days=0;
    if(sscanf(current_.operation,"chart:%d:%u",&row,&days)!=2||row<0||row>=int(list.count))row=-1;
    constexpr size_t kQuotes=8192;
    std::unique_ptr<char[]> data(new(std::nothrow) char[kQuotes]);
    if(!data){publish(current_,"内存不足","error");return;}
    char url[512];stockQuoteUrl(url,sizeof(url),list);
    if(!fetch(url,data.get(),kQuotes)){publish(current_,"行情获取失败","error");return;}
    auto& s=page_.stocks;s.count=list.count;s.updated=time(nullptr);s.chartFor=-1;
    for(unsigned i=0;i<list.count;++i){auto& r=s.rows[i];r={};copy(r.name,list.items[i].name);r.decimals=2;for(auto& v:r.v)v=NAN;
        const char* id=list.items[i].id;const auto* extra=stockExtra(id);
        copy(r.code,extra?extra->code:strchr(id,'.')+1);copy(r.tag,extra?extra->tag:stockMarketTag(atoi(id),r.code));}
    // v_sh600519="1~name~600519~...";  The names are GBK, so the list's own are shown.
    for(char* at=strstr(data.get(),"v_");at;at=strstr(at,"v_")){
        char* equals=strchr(at,'=');char* open=equals?strchr(equals,'"'):nullptr;char* close=open?strchr(open+1,'"'):nullptr;
        if(!close)break;
        *equals=*close=0;const char* symbol=at+2;at=close+1;
        float v[8];unsigned decimals;if(!stockQuote(open+1,symbol,v,decimals))continue;
        for(unsigned i=0;i<list.count;++i){
            char mine[24];stockSymbol(mine,sizeof(mine),list.items[i].id,false);if(strcmp(mine,symbol))continue;
            auto& r=s.rows[i];r.decimals=decimals;std::copy(v,v+8,r.v);
        }
    }
    data.reset();
    if(row>=0&&!cancelled()){
        // Minutes: "0930 1311.84 1049 ..." strings. Days: ["2026-09-04","open","close","high","low",...] rows.
        // Minutes after the close (A shares' after-hours fixed-price trade, Hong Kong's closing auction) are left out.
        // The extras (gold, oil, currencies) come from Eastmoney as strings either way.
        struct Chart{StringArrayScan strings;RowArrayScan rows;ChartPoints points;bool byDay,byRow;char first[16],last[16];const char* after;StringArrayScan::Item item;RowArrayScan::Row row;int close;};
        const StringArrayScan::Item minute=[](void* context,const char* text){
            auto& c=*static_cast<Chart*>(context);if(strlen(text)<6||atoi(text)>c.close)return;
            if(!*c.first){copy(c.first,text);}
            c.points.add(strtof(text+5,nullptr));
        };
        const RowArrayScan::Row day=[](void* context,char (*fields)[16],unsigned count){
            auto& c=*static_cast<Chart*>(context);if(count<5)return;
            if(!*c.first){copy(c.first,fields[0]);}
            copy(c.last,fields[0]);for(unsigned k=1;k<5;++k)c.points.add(strtof(fields[k],nullptr));
        };
        const StringArrayScan::Item eastmoney=[](void* context,const char* text){
            auto& c=*static_cast<Chart*>(context);const char* at=strchr(text,',');if(!at||at-text<10)return;
            if(c.byDay){snprintf(c.last,sizeof(c.last),"%.10s",text);for(unsigned k=0;k<4&&at;++k,at=strchr(at+1,','))c.points.add(strtof(at+1,nullptr));}
            else{snprintf(c.last,sizeof(c.last),"%.5s",text+11);c.points.add(strtof(at+1,nullptr));}
            if(!*c.first)copy(c.first,c.last);
        };
        const char* id=list.items[row].id;const unsigned market=atoi(id);const bool extra=stockExtra(id);
        std::unique_ptr<Chart> chart(new(std::nothrow) Chart{{"",minute,nullptr},{"",day,nullptr},{},days!=0,days&&!extra,{},{},
            extra?(days?"\"klines\":[":"\"trends\":["):"\"data\":[",extra?eastmoney:minute,day,market<=1?1500:market==116?1600:2400});
        if(!chart){publish(current_,"内存不足","error");return;}
        const Sink sink=[](void* context,const char* bytes,size_t size){
            auto& c=*static_cast<Chart*>(context);
            // Each attempt starts over.
            if(!bytes){c.strings=StringArrayScan(c.after,c.item,&c);c.rows=RowArrayScan("\"day\":[",c.row,&c);c.points={};c.first[0]=c.last[0]=0;return true;}
            if(c.byRow)c.rows.feed(bytes,size);else c.strings.feed(bytes,size);
            return true;
        };
        stockChartUrl(url,sizeof(url),id,days);
        if(fetch(url,nullptr,0,sink,chart.get())&&chart->points.count&&(!days||chart->points.count%4==0)){
            const auto& p=chart->points;float low=p.v[0],high=p.v[0];for(unsigned i=0;i<p.count;++i){low=std::min(low,p.v[i]);high=std::max(high,p.v[i]);}
            if(high<=low)high=low+0.001f;
            s.chartFor=row;s.days=days;s.chartCount=p.count;s.low=low;s.high=high;
            for(unsigned i=0;i<p.count;++i)s.chart[i]=int16_t(std::lround((p.v[i]-low)/(high-low)*999));
            s.chartTotal=days?p.count/4:std::max(p.count,(stockSession(market)+p.stride-1)/p.stride);
            s.mid[0]=0;
            if(days){copy(s.from,chart->first+5);copy(s.to,chart->last+5);}  // "2026-09-04" -> "09-04"
            else if(extra){copy(s.from,chart->first);snprintf(s.to,sizeof(s.to),"次日%.5s",chart->first);copy(s.mid,"北京时间");}
            else{
                snprintf(s.from,sizeof(s.from),"%.2s:%.2s",chart->first,chart->first+2);  // "0930" -> "09:30"
                copy(s.to,market==1||market==0?"15:00":"16:00");
                copy(s.mid,market==116?"12:00/13:00":stockUs(market)?"美东时间":"11:30/13:00");
            }
        }
    }
    if(cancelled())return;
    publish(current_,"已更新","ready");
}
// 天气预报: Open-Meteo like the home page (free, no key), for the setup page's city: seven days and the next
// 24 hours in one 1.5 KB reply.
void NativeServices::forecast(){
    // "warn:<i>": only that warning's text, for the forecast already shown.
    if(!strncmp(current_.operation,"warn:",5)){
        auto& f=page_.forecast;const unsigned i=atoi(current_.operation+5);
        if(f.warnings==0xff||i>=f.warnings){publish(current_,"预警已更新，请刷新","error");return;}
        publish(current_,"正在获取预警详情","loading");
        std::unique_ptr<char[]> text(new(std::nothrow) char[768]);
        if(!text||!warningText(f.warning[i].id,text.get(),768)){if(!cancelled())publish(current_,"预警详情获取失败","error");return;}
        const char* body=warningBody(text.get());fitText(f.detail,sizeof(f.detail),body,strlen(body));f.detailFor=i;
        if(!cancelled())publish(current_,"已更新","ready");
        return;
    }
    publish(current_,"正在获取预报","loading");
    MediaPrefs prefs{};if(!mediaLoad("prefs",prefs)||!*prefs.city){publish(current_,"请先在手机配置页选择城市","error");return;}
    std::unique_ptr<char[]> data(new(std::nothrow) char[4096]);
    if(!data){publish(current_,"内存不足","error");return;}
    char url[400];snprintf(url,sizeof(url),"https://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset,uv_index_max"
        "&hourly=temperature_2m,weather_code,precipitation_probability&forecast_days=7&forecast_hours=24&timezone=auto",prefs.latitude,prefs.longitude);
    if(!fetch(url,data.get(),4096)){publish(current_,"预报获取失败","error");return;}
    Json json(cJSON_Parse(data.get()));data.reset();
    const auto at=[](const cJSON* o,const char* k,unsigned i){const auto* v=cJSON_GetArrayItem(item(o,k),i);return cJSON_IsNumber(v)?v->valuedouble:NAN;};
    const auto degrees=[](double v){return static_cast<int8_t>(std::clamp<long>(std::lround(v),-99,99));};
    const auto code=[](double v){return static_cast<uint8_t>(std::clamp(v,0.0,99.0));};
    const auto rain=[](double v){return std::isfinite(v)?static_cast<uint8_t>(std::clamp(v,0.0,100.0)):uint8_t{255};};
    auto& f=page_.forecast;f={};f.detailFor=0xff;copy(f.city,prefs.city);simplifyText(f.city);
    const auto* daily=item(json.value,"daily");
    for(unsigned i=0;i<std::size(f.day);++i){
        const double low=at(daily,"temperature_2m_min",i),high=at(daily,"temperature_2m_max",i),wmo=at(daily,"weather_code",i);
        if(!std::isfinite(low)||!std::isfinite(high)||!std::isfinite(wmo))break;
        f.day[f.days++]={degrees(low),degrees(high),code(wmo),rain(at(daily,"precipitation_probability_max",i))};
    }
    // "2026-10-25T22:00": the first hour, in the city's time.
    const auto* hourly=item(json.value,"hourly");const char* first=cJSON_GetStringValue(cJSON_GetArrayItem(item(hourly,"time"),0));
    f.hour0=first&&strlen(first)>=13?atoi(first+11)%24:0;
    for(unsigned i=0;i<std::size(f.hour);++i){
        const double t=at(hourly,"temperature_2m",i),wmo=at(hourly,"weather_code",i);
        if(!std::isfinite(t)||!std::isfinite(wmo))break;
        f.hour[f.hours++]={degrees(t),code(wmo),rain(at(hourly,"precipitation_probability",i))};
    }
    if(!f.days){publish(current_,"预报数据无效","error");return;}
    // "2026-10-26T06:04" -> 364.
    const auto minutes=[&](const char* key){const char* t=cJSON_GetStringValue(cJSON_GetArrayItem(item(daily,key),0));return t&&strlen(t)>=16?uint16_t(atoi(t+11)*60+atoi(t+14)):uint16_t{0xffff};};
    f.sunrise=minutes("sunrise");f.sunset=minutes("sunset");
    const double uv=at(daily,"uv_index_max",0);f.uv=std::isfinite(uv)?uint8_t(std::clamp(std::lround(uv*10),0l,250l)):uint8_t{255};
    cJSON_Delete(json.value);json.value=nullptr;
    if(cancelled())return;
    if(!air(prefs.latitude,prefs.longitude,f))f.airHours=0;
    if(cancelled())return;
    if(!warnings(prefs,f.warning,f.warnings,std::size(f.warning)))f.warnings=0xff;
    if(cancelled())return;
    publish(current_,"已更新","ready");
}
// China AQI for the next 24 hours from Open-Meteo's air-quality service (plain HTTP, about 3 KB). The readings
// start 23 hours back so PM can be averaged over the past day, as China's hourly report does.
bool NativeServices::air(double latitude,double longitude,WeatherForecast& f){
    constexpr size_t kSize=6144;std::unique_ptr<char[]> data(new(std::nothrow) char[kSize]);if(!data)return false;
    char url[320];snprintf(url,sizeof(url),"http://air-quality-api.open-meteo.com/v1/air-quality?latitude=%.5f&longitude=%.5f"
        "&hourly=pm2_5,pm10,ozone,nitrogen_dioxide,sulphur_dioxide,carbon_monoxide&past_hours=23&forecast_hours=24&timezone=auto",latitude,longitude);
    if(!fetch(url,data.get(),kSize))return false;
    Json json(cJSON_Parse(data.get()));data.reset();
    static constexpr const char* names[]{"pm2_5","pm10","ozone","nitrogen_dioxide","sulphur_dioxide","carbon_monoxide"};
    const auto* hourly=item(json.value,"hourly");const cJSON* series[6];for(unsigned p=0;p<6;++p)series[p]=item(hourly,names[p]);
    const int now=cJSON_GetArraySize(series[0])-24;if(now<0)return false;
    const auto at=[&](unsigned p,int i){const auto* v=cJSON_GetArrayItem(series[p],i);return cJSON_IsNumber(v)?float(v->valuedouble):NAN;};
    f.airHours=0;
    for(int h=0;h<24;++h){
        float reading[6];
        for(unsigned p=0;p<2;++p){float sum=0;unsigned n=0;for(int i=std::max(0,now+h-23);i<=now+h;++i)if(const float v=at(p,i);std::isfinite(v)){sum+=v;++n;}reading[p]=n?sum/n:NAN;}
        for(unsigned p=2;p<6;++p)reading[p]=at(p,now+h);
        reading[5]/=1000;  // CO arrives in µg/m³
        const auto aqi=chinaAqi(reading);f.aqi[f.airHours++]=int16_t(aqi.aqi);if(!h)f.primary=aqi.primary;
    }
    const auto micrograms=[&](unsigned p){const float v=at(p,now);return std::isfinite(v)?uint16_t(std::clamp(std::lround(v),0l,9999l)):uint16_t{0xffff};};
    f.pm25=micrograms(0);f.pm10=micrograms(1);
    return f.aqi[0]>=0;
}
// 气象预警 from 中央气象台 (plain HTTP, no key). It filters by province only, and a province in a cold wave lists
// hundreds (Shanxi 277, 73 KB), so the titles are read as they arrive and only the city's and the province's kept.
bool NativeServices::warnings(const MediaPrefs& prefs,WeatherWarning* list,uint8_t& count,unsigned capacity){
    count=0;if(*prefs.country&&strcmp(prefs.country,"CN"))return false;  // Hong Kong and abroad are not covered
    const char* area=*prefs.province?prefs.province:prefs.city;  // municipalities have no province
    char url[200];int n=snprintf(url,sizeof(url),"http://www.nmc.cn/rest/findAlarm?pageNo=1&pageSize=400&signaltype=&signallevel=&province=");
    for(auto* c=reinterpret_cast<const unsigned char*>(area);*c&&n<int(sizeof(url))-4;++c)n+=snprintf(url+n,sizeof(url)-n,"%%%02X",*c);
    // Each entry: alertid, issuetime, title.
    struct Scan{FieldScan id,issued,title;char alert[32],time[20];WeatherWarning* list;uint8_t* count;unsigned capacity;const MediaPrefs* prefs;};
    const FieldScan::Item onId=[](void* context,const char* text){auto& s=*static_cast<Scan*>(context);snprintf(s.alert,sizeof(s.alert),"%s",text);};
    const FieldScan::Item onTime=[](void* context,const char* text){auto& s=*static_cast<Scan*>(context);snprintf(s.time,sizeof(s.time),"%s",text);};
    const FieldScan::Item onTitle=[](void* context,const char* text){auto& s=*static_cast<Scan*>(context);addWarning(s.list,*s.count,s.capacity,text,s.time,s.alert,s.prefs->city,s.prefs->province);};
    std::unique_ptr<Scan> scan(new(std::nothrow) Scan{{"\"alertid\":\"",onId,nullptr},{"\"issuetime\":\"",onTime,nullptr},{"\"title\":\"",onTitle,nullptr},{},{},list,&count,capacity,&prefs});
    if(!scan)return false;
    // Each attempt starts over (and hands the scanners their context).
    const Sink sink=[](void* context,const char* bytes,size_t size){
        auto& s=*static_cast<Scan*>(context);
        if(!bytes){*s.count=0;s.alert[0]=s.time[0]=0;s.id.reset(&s);s.issued.reset(&s);s.title.reset(&s);return true;}
        // A byte at a time to all three, so a title sees its own entry's id and time.
        for(size_t i=0;i<size;++i){s.id.feed(bytes+i,1);s.issued.feed(bytes+i,1);s.title.feed(bytes+i,1);}
        return true;
    };
    if(!fetch(url,nullptr,0,sink,scan.get()))return false;
    scan.reset();
    // A province's own warning may cover only some of its cities ("舟山市（定海区、普陀区）等1市2县"): read its text
    // once and keep it only when it names the city or the whole province. Unreadable ones are kept.
    // ponytail: a city's lower signal merged under such a warning goes with it; keep both kinds apart if that matters.
    unsigned kept=0;
    for(unsigned i=0;i<count&&!cancelled();++i){
        const auto& w=list[i];bool covers=true;
        if(w.provincial&&*w.id){
            const auto* seen=std::find_if(std::begin(warnSeen_),std::end(warnSeen_),[&](const WarnSeen& s){return !strcmp(s.id,w.id);});
            if(seen!=std::end(warnSeen_))covers=seen->covers;
            else if(std::unique_ptr<char[]> text(new(std::nothrow) char[768]);text&&warningText(w.id,text.get(),768)){
                covers=warningCovers(text.get(),prefs.city);auto& slot=warnSeen_[warnSeenNext_++%std::size(warnSeen_)];copy(slot.id,w.id);slot.covers=covers;
            }
        }
        if(covers)list[kept++]=w;
    }
    count=kept;
    std::stable_sort(list,list+count,[](const WeatherWarning& a,const WeatherWarning& b){return a.level>b.level;});
    return true;
}
// A warning's text from its page on nmc.cn (about 45 KB of HTML, the text near the end); the rest is not read.
bool NativeServices::warningText(const char* id,char* out,size_t capacity){
    char url[120];snprintf(url,sizeof(url),"http://www.nmc.cn/publish/alarm/%s.html",id);
    struct Page{HtmlTextScan scan;char* out;size_t capacity;};
    std::unique_ptr<Page> page(new(std::nothrow) Page{{"id=alarmtext>",out,capacity},out,capacity});if(!page)return false;
    const Sink sink=[](void* context,const char* bytes,size_t size){
        auto& p=*static_cast<Page*>(context);
        if(!bytes){p.scan=HtmlTextScan("id=alarmtext>",p.out,p.capacity);return true;}
        p.scan.feed(bytes,size);return !p.scan.done();
    };
    return fetch(url,nullptr,0,sink,page.get())&&page->scan.done()&&*out;
}
// Plain HTTP on purpose (no TLS buffers): Eastmoney 7x24 headlines and Tencent's index quotes. The task stays for the
// page: "item:<i>" sends that headline's text from what was read, anything else fetches again.
bool NativeServices::newsList(){
    std::unique_ptr<char[]> data(new(std::nothrow) char[16384]);
    if(!data){publish(current_,"内存不足","error");return false;}
    // One line per index, fields split by '~': [3] price, [32] change %. The names are GBK, so ours are used.
    char quotes[96]{};
    if(fetch("http://qt.gtimg.cn/q=sh000001,sz399001",data.get(),4096)){
        const char* names[]{"沪","深"};const char* line=data.get();
        for(unsigned i=0;i<2&&line&&(line=strchr(line,'"'));++i){
            const char* field[33]{};unsigned n=0;for(const char* c=line+1;*c&&*c!='"'&&n<33;++c)if(c==line+1||c[-1]=='~')field[n++]=c;
            if(n==33){const size_t used=strlen(quotes);snprintf(quotes+used,sizeof(quotes)-used,"%s%s %.0f %+.2f%%",used?" ":"",names[i],atof(field[3]),atof(field[32]));}
            line=strchr(line+1,'\n');
        }
    }
    if(cancelled())return false;
    // "page:N" walks back through older headlines, six per page.
    newsPage_=strncmp(current_.operation,"page:",5)?1:std::clamp(atoi(current_.operation+5),1,20);
    char url[96];snprintf(url,sizeof(url),"http://newsapi.eastmoney.com/kuaixun/v1/getlist_102_ajaxResult_6_%u_.html",newsPage_);
    if(!fetch(url,data.get(),16384)){publish(current_,"快讯获取失败","error");return false;}
    const char* start=strchr(data.get(),'{');Json json(start?cJSON_Parse(start):nullptr);data.reset();
    newsCount_=0;const cJSON* entry;
    cJSON_ArrayForEach(entry,item(json.value,"LivesList")){
        if(newsCount_==6)break;
        auto& n=news_[newsCount_++];
        const char* when=str(entry,"showtime");copy(n.time,strlen(when)>=16?when+11:"");  // "2026-10-24 22:16:07" -> 22:16
        copy(n.title,str(entry,"title"));
        // The digest repeats the headline in 【】 first.
        const char* digest=str(entry,"digest");if(!strncmp(digest,"【",3)&&strstr(digest,"】"))digest=strstr(digest,"】")+3;copy(n.digest,digest);
    }
    if(!newsCount_){publish(current_,"快讯解析失败","error");return false;}
    // A new page opens on its first headline (walking back, the badge then asks for the last).
    if(newsIndex_>=newsCount_||strncmp(current_.operation,"page:",5)==0)newsIndex_=0;
    const char* rows[]{quotes,"","",""};publish(current_,"已更新","ready","",rows);
    return true;
}
void NativeServices::news(){
    publish(current_,"正在获取快讯","loading");
    std::unique_ptr<NewsItem[]> items(new(std::nothrow) NewsItem[6]);
    if(!items){publish(current_,"内存不足","error");return;}
    news_=items.get();newsCount_=newsIndex_=0;
    for(;;){
        if(!strncmp(current_.operation,"item:",5)&&newsCount_){newsIndex_=std::min<unsigned>(atoi(current_.operation+5),newsCount_-1);publish(current_,"已更新","ready");}
        else newsList();
        if(cancelled())break;
        Job action{};
        while(!cancelled()&&(xQueueReceive(actions_,&action,pdMS_TO_TICKS(50))!=pdTRUE||action.generation!=current_.generation)){}
        if(cancelled())break;
        current_.request=action.request;copy(current_.operation,action.operation);
    }
    news_=nullptr;newsCount_=0;
}
// open.er-api.com: free, no key, plain HTTP, daily rates for ~160 currencies against any base (about 3 KB).
void NativeServices::rates(){
    publish(current_,"正在获取汇率","loading");
    if(!SettingsStore::loadRates(rateBoard_))rateBoard_={};
    std::unique_ptr<char[]> data(new(std::nothrow) char[6144]);
    if(!data){publish(current_,"内存不足","error");return;}
    char url[64];snprintf(url,sizeof(url),"http://open.er-api.com/v6/latest/%s",rateBoard_.base);
    if(!fetch(url,data.get(),6144)){publish(current_,"汇率获取失败","error");return;}
    Json json(cJSON_Parse(data.get()));data.reset();const auto* table=item(json.value,"rates");
    if(!cJSON_IsObject(table)){publish(current_,"汇率数据无效","error");return;}
    for(unsigned i=0;i<RateBoard::kTargets;++i){const double v=*rateBoard_.targets[i]?num(table,rateBoard_.targets[i]):NAN;rates_[i]=std::isfinite(v)&&v>0?v:0;}
    const double updated=num(json.value,"time_last_update_unix");rateUpdated_=std::isfinite(updated)?updated:0;
    publish(current_,"已更新","ready");
}
// 自选股: the first security Eastmoney's search finds for what was said joins the list (LiveServices then fetches it).
void NativeServices::addStock(const char* heard){
    const std::string word=stockQuery(heard);
    if(word.empty()){copy(notice_,"没有听清，请再说一次");status("没有听清");return;}
    status("正在查找","loading");
    // What was heard, then (found nothing) without its first two characters (a misheard 添加: 添嘉, 天加), then without
    // its last (英伟答).
    std::string tries[3]{word};const unsigned chars=voice::chars(word);
    if(chars>=4)tries[1]=word.substr(voice::head(word,2).size());
    if(chars>=3)tries[2]=voice::head(word,chars-1);
    StockHit hits[4];int found=0;
    for(const auto& t:tries){
        if(t.empty())continue;
        char encoded[160];urlEncode(encoded,sizeof(encoded),t.c_str());
        found=searchStocks(encoded,hits,std::size(hits));
        if(found)break;
    }
    if(found<=0){snprintf(notice_,sizeof(notice_),found<0?"查找失败，请检查网络":"没找到「%s」",word.c_str());status("已识别");return;}
    StockList list;if(!mediaLoad("stocks",list)||!list.count||list.count>StockList::kMax)list=defaultStocks();
    const auto& hit=hits[0];
    for(unsigned i=0;i<list.count;++i)if(!strcmp(list.items[i].id,hit.id)){snprintf(notice_,sizeof(notice_),"%s已在自选中",hit.name);select_=i;status("已识别");return;}
    if(list.count==StockList::kMax){copy(notice_,"最多 12 只，先长按下键删一只");status("已识别");return;}
    auto& to=list.items[list.count++];copy(to.id,hit.id);copy(to.name,hit.name);
    if(!mediaSave("stocks",list)){copy(notice_,"保存失败");status("已识别");return;}
    snprintf(notice_,sizeof(notice_),"已添加%s（%s）",to.name,hit.type);select_=list.count-1;status("已识别");
}
namespace {
// The city as Settings shows it: "杭州 · 浙江".
void cityLabel(char (&out)[80],const MediaPrefs& p){if(*p.province)snprintf(out,sizeof(out),"%s · %s",p.city,p.province);else snprintf(out,sizeof(out),"%s",p.city);}
}
// 所在城市 by voice: a city in the built-in list (the setup page's, and every prefecture-level one).
void NativeServices::setCity(const char* heard){
    const auto* c=spokenCity(heard);
    if(!c){char said[64];copy(said,heard);snprintf(notice_,sizeof(notice_),"没找到城市「%s」，请说地级市名",said);status("已识别");return;}
    MediaPrefs prefs{};copy(prefs.city,c->name);copy(prefs.province,c->province);copy(prefs.country,c->country);prefs.latitude=c->latitude;prefs.longitude=c->longitude;
    if(!mediaSave("prefs",prefs)){copy(notice_,"保存失败");status("已识别");return;}
    ++mediaPrefsRevision;snprintf(notice_,sizeof(notice_),"所在城市已改为%s",c->name);cityLabel(city_,prefs);status("已识别");
}
// 所在城市 by IP: ip-api.com (plain HTTP, no key) names the city of the badge's internet connection, in Chinese. A city
// outside China (a VPN, a proxy) is refused, as the setup page only offers Chinese ones.
void NativeServices::locate(){
    status("正在按 IP 查询","loading");
    char data[512]{};
    if(!fetch("http://ip-api.com/json/?lang=zh-CN&fields=status,country,countryCode,regionName,city,lat,lon",data,sizeof(data))){copy(notice_,"IP 查询失败，请检查网络");status("查询失败","error");return;}
    Json json(cJSON_Parse(data));const char* country=str(json.value,"countryCode");
    if(strcmp(str(json.value,"status"),"success")){copy(notice_,"IP 查询失败，请稍后重试");status("查询失败","error");return;}
    if(strcmp(country,"CN")&&strcmp(country,"HK")&&strcmp(country,"MO")&&strcmp(country,"TW")){
        char where[40];copy(where,str(json.value,"country"));snprintf(notice_,sizeof(notice_),"IP 显示在%s（可能开了代理），请用语音输入",where);status("已查询");return;}
    MediaPrefs prefs{};
    const CityEntry* c=!strcmp(country,"HK")?cityNamed("香港"):!strcmp(country,"MO")?cityNamed("澳门"):cityNamed(str(json.value,"city"));
    if(c){copy(prefs.city,c->name);copy(prefs.province,c->province);copy(prefs.country,c->country);prefs.latitude=c->latitude;prefs.longitude=c->longitude;}
    else{
        // A city the list lacks: the service's own name and coordinates.
        std::string city=str(json.value,"city");if(city.size()>3&&!city.compare(city.size()-3,3,"市"))city.resize(city.size()-3);
        const double lat=num(json.value,"lat"),lon=num(json.value,"lon");
        if(city.empty()||!std::isfinite(lat)||!std::isfinite(lon)){copy(notice_,"IP 没有给出城市，请用语音输入");status("已查询");return;}
        copy(prefs.city,city.c_str());copy(prefs.province,strcmp(country,"CN")?"":provinceName(str(json.value,"regionName")).c_str());copy(prefs.country,country);
        prefs.latitude=lat;prefs.longitude=lon;
    }
    if(!mediaSave("prefs",prefs)){copy(notice_,"保存失败");status("保存失败","error");return;}
    ++mediaPrefsRevision;snprintf(notice_,sizeof(notice_),"按 IP 定位到%s",prefs.city);cityLabel(city_,prefs);status("已定位");
}
void NativeServices::cityName(char* out,size_t size)const{
    MediaPrefs prefs{};char label[80];
    if(mediaLoad("prefs",prefs)&&*prefs.city){cityLabel(label,prefs);snprintf(out,size,"%s",label);}else if(size)*out=0;
}
void NativeServices::closeSocket(){
    if(ws_){esp_transport_close(ws_);esp_transport_destroy(ws_);ws_=nullptr;}
    if(ssl_){esp_transport_destroy(ssl_);ssl_=nullptr;}
    ready_=false;
}
bool NativeServices::text(const char* type,const char* state,const char* mode){
    if(!ws_){return false;}
    Json j(cJSON_CreateObject());if(!j.value)return false;
    cJSON_AddStringToObject(j.value,"type",type);if(*session_)cJSON_AddStringToObject(j.value,"session_id",session_);
    if(state){cJSON_AddStringToObject(j.value,"state",state);}
    if(mode){cJSON_AddStringToObject(j.value,"mode",mode);}
    if(!strcmp(type,"abort"))cJSON_AddStringToObject(j.value,"reason","user_interruption");
    char* raw=cJSON_PrintUnformatted(j.value);if(!raw)return false;const int size=strlen(raw);
    const int sent=esp_transport_ws_send_raw(ws_,static_cast<ws_transport_opcodes_t>(WS_TRANSPORT_OPCODES_TEXT|WS_TRANSPORT_OPCODES_FIN),raw,size,250);free(raw);return sent==size;
}
bool NativeServices::controlAudio(uint8_t command){return audio_.nativeInput(5,&command,1);}
void NativeServices::beginListen(bool interrupted){
    // 语音笔记 listens until Confirm (manual: pauses do not end it) and never plays what comes back. 语音指令 never
    // plays it either, but ends at the pause after the phrase, as 小智语音 does.
    const bool memo=transcribing(),pause=!memo||current_.app==App::Voice;
    resumeListening_=false;dropTts_=interrupted||memo;speaking_=false;
    board::discardMicrophone();
    listening_=true;autoListening_=pause;listenAt_=esp_timer_get_time();
    if(!text("listen","start",pause?"auto":"manual")||!controlAudio(1)){listening_=false;error_=ESP_FAIL;status(memo?"录音启动失败":"自动收音启动失败","error");return;}
    ++listenStarts_;status("正在听");
}
void NativeServices::toggleChat(){
    if(listening_){endListen();return;}
    if(asr_)return;  // 语音笔记: the text of the last one is still on its way
    const bool interrupted=!transcribing()&&(speaking_||dropTts_);
    if(interrupted&&!text("abort")){error_=ESP_FAIL;status("打断发送失败","error");return;}
    beginListen(interrupted);
}
void NativeServices::endListen(){controlAudio(0);listening_=autoListening_=false;if(!text("listen","stop")){status("语音连接断开","error");return;}asr_=transcribing();askedAt_=esp_timer_get_time();status(asr_?"正在识别":"正在思考");}
void NativeServices::audioOutput(void* context,uint8_t type,const uint8_t* data,size_t length){
    auto& self=*static_cast<NativeServices*>(context);if(self.cancelled())return;
    if(type==1&&self.listening_&&self.ws_){
        if(esp_transport_ws_send_raw(self.ws_,static_cast<ws_transport_opcodes_t>(WS_TRANSPORT_OPCODES_BINARY|WS_TRANSPORT_OPCODES_FIN),reinterpret_cast<const char*>(data),length,250)==static_cast<int>(length))++self.chatTx_;
        else {self.error_=ESP_FAIL;self.status("音频发送失败","error");}
    }else if(type==7&&self.listening_&&self.autoListening_)self.endListen();
    else if(type==9){self.error_=ESP_FAIL;self.status("音频硬件错误","error");}
}
int NativeServices::xiaozhiPost(const char* url,const uint8_t* mac,const char* client,const char* body,char* output,size_t capacity){
    struct Body{char* bytes;size_t used,capacity;} reply{output,0,capacity};output[0]=0;
    esp_http_client_config_t config{};config.url=url;config.method=HTTP_METHOD_POST;config.timeout_ms=12000;config.buffer_size=1024;config.buffer_size_tx=1024;
    config.addr_type=HTTP_ADDR_TYPE_INET;config.crt_bundle_attach=esp_crt_bundle_attach;config.user_data=&reply;config.user_agent="ai-passport/1";
    config.event_handler=[](esp_http_client_event_t* e)->esp_err_t{auto& b=*static_cast<Body*>(e->user_data);
        if(e->event_id==HTTP_EVENT_ON_DATA&&e->data_len>0){if(b.used+e->data_len>=b.capacity)return ESP_FAIL;memcpy(b.bytes+b.used,e->data,e->data_len);b.used+=e->data_len;b.bytes[b.used]=0;}return ESP_OK;};
    auto client_=esp_http_client_init(&config);if(!client_)return -1;
    char device[18];snprintf(device,sizeof(device),"%02x:%02x:%02x:%02x:%02x:%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    esp_http_client_set_header(client_,"Device-Id",device);esp_http_client_set_header(client_,"Client-Id",client);
    esp_http_client_set_header(client_,"Activation-Version","1");esp_http_client_set_header(client_,"Accept-Language","zh-CN");esp_http_client_set_header(client_,"Content-Type","application/json");
    esp_http_client_set_post_field(client_,body,strlen(body));++httpCount_;
    const auto result=esp_http_client_perform(client_);const int code=result==ESP_OK?esp_http_client_get_status_code(client_):-1;httpStatus_=code;error_=result==ESP_OK?0:result;
    esp_http_client_cleanup(client_);return code;
}
bool NativeServices::cloudIdentity(const uint8_t* mac,const char* client,char* url,size_t urlSize,char* token,size_t tokenSize,bool probe){
    // One answer per identity and boot: 语音笔记 reconnects after every idle drop.
    static char cachedUrl[160],cachedToken[160];static uint8_t cachedFor[6];
    if(*cachedUrl&&!memcmp(cachedFor,mac,6)){snprintf(url,urlSize,"%s",cachedUrl);snprintf(token,tokenSize,"%s",cachedToken);return true;}
    auto reply=std::unique_ptr<char[]>(new(std::nothrow)char[2048]);if(!reply){status("内存不足","error");return false;}
    char body[256];snprintf(body,sizeof(body),"{\"version\":2,\"language\":\"zh-CN\",\"mac_address\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"uuid\":\"%s\",\"chip_model_name\":\"esp32c3\",\"application\":{\"name\":\"ai-passport\",\"version\":\"%s\"},\"board\":{\"type\":\"ai-passport\",\"name\":\"ai-passport\"}}",
        mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],client,esp_app_get_description()->version);
    char activate[sizeof(kXiaozhiOta)+8];snprintf(activate,sizeof(activate),"%sactivate",kXiaozhiOta);
    for(unsigned attempt=1;!cancelled();++attempt){
        const int code=xiaozhiPost(kXiaozhiOta,mac,client,body,reply.get(),2048);
        if(code!=200){if(attempt<networkAttempts&&retryPause(attempt))continue;if(!cancelled())status("小智服务器无响应，按确认重试","error");return false;}
        Json answer(cJSON_Parse(reply.get()));const auto* socket=item(answer.value,"websocket");const auto* activation=item(answer.value,"activation");
        const char* number=str(activation,"code");
        if(!*number){
            if(strncmp(str(socket,"url"),"wss://",6)){status("小智服务器未提供连接地址","error");return false;}
            snprintf(url,urlSize,"%s",str(socket,"url"));snprintf(token,tokenSize,"%s",str(socket,"token"));
            if(strlen(url)<sizeof(cachedUrl)&&strlen(token)<sizeof(cachedToken)){copy(cachedUrl,url);copy(cachedToken,token);memcpy(cachedFor,mac,6);}
            return true;
        }
        if(probe)return false;
        // New to XiaoZhi: the user binds this board to their account with the code, then the check answers again.
        char state[40];snprintf(state,sizeof(state),"验证码 %s",number);
        const char* lines[]{"打开小智 App 或 xiaozhi.me，添加设备，输入上面的验证码","","",""};
        const auto until=esp_timer_get_time()+std::max(60.0,std::min(600.0,num(activation,"timeout_ms")/1000))*1000000;
        publish(current_,state,transcribing()?"error":"loading",transcribing()?"":"小智语音",lines);
        int result=202;
        while(result==202&&!cancelled()&&esp_timer_get_time()<until){
            for(unsigned i=0;i<150&&!cancelled();++i)vTaskDelay(pdMS_TO_TICKS(20));
            if(!cancelled())result=xiaozhiPost(activate,mac,client,"{}",reply.get(),2048);
        }
        if(cancelled())return false;
        if(result!=200){status(result==202?"验证码已过期，按确认重试":"激活失败，按确认重试","error");return false;}
        status("已激活，正在连接","loading");attempt=0;
    }
    return false;
}
void NativeServices::chat(){
    const bool memo=transcribing();
    ready_=listening_=speaking_=asr_=false;
    const bool probe=!strcmp(current_.operation,"probe");if(probe){cloudCheckOk_=false;++cloudChecks_;}connectAttempts_=0;
    struct Credentials{char url[512]{},token[1024]{},client[129]{},host[128]{},path[512]{},headers[1536]{};int port{443};};
    auto credentials=std::unique_ptr<Credentials>(new(std::nothrow)Credentials);if(!credentials){status("内存不足","error");return;}
    stt_[0]=answer_[0]=0;  // never show the last session's exchange
    status("云端连接中","loading");if(!syncClock()){status("网络校时失败","error");return;}
    bool found=false;
    if(nvs_flash_init_partition("legacy_nvs")==ESP_OK){
        nvs_handle_t h;if(nvs_open_from_partition("legacy_nvs","websocket",NVS_READONLY,&h)==ESP_OK){found=nvsString(h,"url",credentials->url,sizeof(credentials->url));nvsString(h,"token",credentials->token,sizeof(credentials->token));nvs_close(h);}
        if(nvs_open_from_partition("legacy_nvs","xiaozhi",NVS_READONLY,&h)==ESP_OK){if(!found)found=nvsString(h,"ws_url",credentials->url,sizeof(credentials->url));if(!*credentials->token)nvsString(h,"token",credentials->token,sizeof(credentials->token));found=nvsString(h,"client_id",credentials->client,sizeof(credentials->client))&&found;nvs_close(h);}
        // Newer XiaoZhi firmware (OTA layout) keeps no "xiaozhi" namespace: its Client-Id is the board's uuid.
        if(found&&!*credentials->client&&nvs_open_from_partition("legacy_nvs","board",NVS_READONLY,&h)==ESP_OK){nvsString(h,"uuid",credentials->client,sizeof(credentials->client));nvs_close(h);}
        nvs_flash_deinit_partition("legacy_nvs");
    }
    // 语音笔记 is a second XiaoZhi device (same server and token) so notes stay out of the chat's history and memory:
    // the MAC with its locally administered bit set, and a Client-Id the Mac's 语音输入 derives the same way
    // (transcriber() in passport_device_cloud.py). A board that never ran XiaoZhi's firmware uses that Client-Id
    // form for its chat too, and asks XiaoZhi for the server and token.
    uint8_t mac[6];esp_read_mac(mac,ESP_MAC_WIFI_STA);if(memo)mac[0]|=2;
    if(memo||!found||!*credentials->client)snprintf(credentials->client,sizeof(credentials->client),"61692d70-6173-4000-8000-%02x%02x%02x%02x%02x%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    if(!found&&!cloudIdentity(mac,credentials->client,credentials->url,sizeof(credentials->url),credentials->token,sizeof(credentials->token),probe))return;
    if(strncmp(credentials->url,"wss://",6)||strpbrk(credentials->token,"\r\n")||strpbrk(credentials->client,"\r\n")){status("云端连接凭据无效","error");return;}
    const char* authority=credentials->url+6;const char* slash=strchr(authority,'/');const char* end=slash?slash:authority+strlen(authority);const char* colon=static_cast<const char*>(memchr(authority,':',end-authority));
    const size_t hostLength=(colon?colon:end)-authority;if(!hostLength||hostLength>=sizeof(credentials->host)){status("云端地址无效","error");return;}
    memcpy(credentials->host,authority,hostLength);if(colon){char* portEnd;const long port=strtol(colon+1,&portEnd,10);if(portEnd!=end||port<1||port>65535){status("云端地址无效","error");return;}credentials->port=port;}
    copy(credentials->path,slash?slash:"/");
    const int headers=snprintf(credentials->headers,sizeof(credentials->headers),"Protocol-Version: 1\r\nDevice-Id: %02x:%02x:%02x:%02x:%02x:%02x\r\nClient-Id: %s\r\n%s%s%s",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],credentials->client,*credentials->token?"Authorization: ":"",*credentials->token&&!strchr(credentials->token,' ')?"Bearer ":"",credentials->token);
    if(headers<0||headers+3>=static_cast<int>(sizeof(credentials->headers))){status("连接凭据过长","error");return;}
    strcat(credentials->headers,"\r\n");bool connected=false;
    for(unsigned attempt=1;attempt<=networkAttempts&&!cancelled();++attempt){
    connectAttempts_=attempt;ssl_=esp_transport_ssl_init();if(!ssl_){status("TLS 内存不足","error");return;}
    esp_transport_ssl_crt_bundle_attach(ssl_,esp_crt_bundle_attach);esp_transport_ssl_set_addr_family(ssl_,ESP_TLS_AF_INET);
    ws_=esp_transport_ws_init(ssl_);if(!ws_){status("连接内存不足","error");return;}
    esp_transport_ws_set_path(ws_,credentials->path);esp_transport_ws_set_headers(ws_,credentials->headers);
    if(esp_transport_connect(ws_,credentials->host,credentials->port,12000)>=0){connected=true;error_=tlsResult_=tlsError_=tlsFlags_=socketError_=0;break;}
    int tlsCode=0,tlsFlags=0;const int result=esp_tls_get_and_clear_last_error(esp_transport_get_error_handle(ssl_),&tlsCode,&tlsFlags);error_=result?result:ESP_FAIL;tlsResult_=result;tlsError_=tlsCode;tlsFlags_=tlsFlags;socketError_=esp_transport_get_errno(ssl_);
    const bool retry=retryNetwork(attempt,-1,result,tlsFlags,socketError_.load(),0);closeSocket();
    if(!retry)break;
    char state[64];snprintf(state,sizeof(state),"云端重试中 %u/%u",attempt+1,networkAttempts);status(state,"loading");if(!retryPause(attempt))return;
    }
    if(!connected){if(!cancelled())status(error_==ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT?"云端连接超时":"云端连接失败","error");return;}
    credentials.reset();if(cancelled())return;
    char hello[]="{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\",\"features\":{\"mcp\":false},\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}";
    if(esp_transport_ws_send_raw(ws_,static_cast<ws_transport_opcodes_t>(WS_TRANSPORT_OPCODES_TEXT|WS_TRANSPORT_OPCODES_FIN),hello,strlen(hello),1000)!=static_cast<int>(strlen(hello))){status("云端握手失败","error");return;}
    auto buffer=std::unique_ptr<uint8_t[]>(new(std::nothrow)uint8_t[2048]);if(!buffer){status("内存不足","error");return;}
    session_[0]=stt_[0]=answer_[0]=0;speechTurn_=0;listening_=speaking_=autoListening_=resumeListening_=false;ready_=false;dropTts_=memo;
    const auto started=esp_timer_get_time();auto pingAt=started;int64_t lastRx=0;size_t used=0,frameUsed=0,frameLength=0;unsigned messageKind=0;
    pumpUs_=decodeUs_=readUs_=rxGapUs_=0;
    while(!cancelled()){
        if(error_.load()!=0)break;
        Job action{};if(xQueueReceive(actions_,&action,0)==pdTRUE&&action.generation==current_.generation){current_.request=action.request;
            // 中英翻译: reading aloud needs the speaker, so the connection closes first (memo() reads, then reconnects).
            if(current_.app==App::Translate&&!strcmp(action.operation,"say")){if(!listening_&&!asr_){sayWanted_=true;break;}continue;}
            if(ready_&&!strcmp(action.operation,"toggle"))toggleChat();else if(memo&&!strcmp(action.operation,"toggle"))wantListen_=true;else if(ready_)status(listening_?"正在听":speaking_?"正在回答":"云端就绪");}
        if(resumeListening_&&board::bufferedSpeakerDrained()){
            resumeListening_=false;
            if(!controlAudio(4)){status("播放资源释放失败","error");break;}
            beginListen();
        }
        if(ready_){const auto at=esp_timer_get_time();audio_.nativePump();pumpUs_=std::max(pumpUs_.load(),unsigned(esp_timer_get_time()-at));if(listening_&&audio_.encoderResult.load()!=0){status("录音编码失败","error");break;}}
        if(!ready_&&esp_timer_get_time()-started>12000000){status("云端握手超时","error");break;}
        // 语音笔记: one note is at most 60 s (its text must fit the 2 KiB message), and its text comes within 15 s.
        // 中英翻译 keeps a phrase to 30 s: its text and translation then fit the page.
        // 语音指令: a command is short; 10 s also ends one that never paused (or never began).
        if(memo&&listening_&&esp_timer_get_time()-listenAt_>(current_.app==App::Voice?10000000:current_.app==App::Translate?30000000:60000000))endListen();
        if(memo&&asr_&&esp_timer_get_time()-askedAt_>15000000){status("识别超时，请重试","error");break;}
        if(esp_timer_get_time()-pingAt>20000000){esp_transport_ws_send_raw(ws_,static_cast<ws_transport_opcodes_t>(WS_TRANSPORT_OPCODES_PING|WS_TRANSPORT_OPCODES_FIN),nullptr,0,250);pingAt=esp_timer_get_time();}
        const int available=esp_transport_poll_read(ws_,10);
        // The cloud closes a connection left idle: 语音笔记 opens another at the next Confirm.
        const bool idle=memo&&ready_&&!listening_&&!asr_;
        if(available<0){if(idle)idleDrop_=true;else status("云端连接断开","error");break;}if(!available)continue;
        const auto readAt=esp_timer_get_time();const int count=esp_transport_read(ws_,reinterpret_cast<char*>(buffer.get()+used),2047-used,200);
        readUs_=std::max(readUs_.load(),unsigned(esp_timer_get_time()-readAt));
        const unsigned opcode=esp_transport_ws_get_read_opcode(ws_)&15;
        if(opcode==WS_TRANSPORT_OPCODES_CLOSE||count<0){if(idle)idleDrop_=true;else status("云端连接断开","error");break;}if(!count)continue;
        if(!frameUsed){frameLength=esp_transport_ws_get_read_payload_len(ws_);if(opcode!=WS_TRANSPORT_OPCODES_CONT)messageKind=opcode;}
        if(frameLength>2047-used+frameUsed){error_=ESP_ERR_INVALID_SIZE;status("云端消息过长","error");break;}
        used+=count;frameUsed+=count;if(frameUsed<frameLength)continue;frameUsed=0;
        if(!esp_transport_ws_get_fin_flag(ws_))continue;
        if(messageKind==WS_TRANSPORT_OPCODES_BINARY){
            if(ready_&&!listening_&&!dropTts_){
                const auto at=esp_timer_get_time();if(lastRx)rxGapUs_=std::max(rxGapUs_.load(),unsigned(at-lastRx));lastRx=at;
                if(used+4>2047){status("音频帧过长","error");break;}
                memmove(buffer.get()+4,buffer.get(),used);for(unsigned i=0;i<4;++i)buffer[i]=(rate_>>(8*i))&255;
                if(audio_.nativeInput(2,buffer.get(),used+4))++chatRx_;else{status("语音解码失败","error");break;}
                decodeUs_=std::max(decodeUs_.load(),unsigned(esp_timer_get_time()-at));
            }
        }else if(messageKind==WS_TRANSPORT_OPCODES_TEXT){
            buffer[used]=0;Json message(cJSON_Parse(reinterpret_cast<char*>(buffer.get())));const char* type=str(message.value,"type");
            if(!strcmp(type,"hello")){
                if(probe){cloudCheckOk_=true;return;} // Check TLS, authorization, WebSocket and hello; never open the codec.
                copy(session_,str(message.value,"session_id"));const double rate=num(item(message.value,"audio_params"),"sample_rate");rate_=(rate==16000||rate==24000||rate==48000)?rate:24000;
                audio_.nativeOutput(audioOutput,this);if(!audio_.start(App::Chat,current_.generation,true,rate_)){status("音频内存不足","error");break;}
                ready_=true;if(!memo||wantListen_){wantListen_=false;beginListen();}else status("云端就绪");
            }else if(memo&&!strcmp(type,"stt")){
                // Keep the text and close the connection: the reply the cloud now starts is never read.
                listening_=asr_=false;controlAudio(0);
                // 中英翻译: memo() translates once the connection is closed.
                if(current_.app==App::Translate){copy(page_.translation.source,str(message.value,"text"));return;}
                // 自选股 and 所在城市: memo() looks it up once the connection is closed.
                if(current_.app==App::Stock||current_.app==App::Settings){copy(spoken_,str(message.value,"text"));status(*spoken_?"已识别":"没有听清");return;}
                char heard[1024];copy(heard,str(message.value,"text"));memoHeard_=heard;status(*heard?"已识别":"没有听清");
                while(memoHeard_.load()&&!cancelled())vTaskDelay(pdMS_TO_TICKS(10));
                memoHeard_=nullptr;return;
            }else if(!strcmp(type,"stt")){listening_=autoListening_=false;controlAudio(0);copy(stt_,str(message.value,"text"));answer_[0]=0;dropTts_=false;if(*stt_){++speechTurn_;status("正在思考");}else beginListen();}
            else if(!strcmp(type,"llm")){status(listening_?"正在听":speaking_?"正在回答":"云端就绪");}
            else if(!strcmp(type,"tts")&&ready_&&!dropTts_){
                const char* state=str(message.value,"state");
                if(!strcmp(state,"start")){++ttsStarts_;lastRx=0;pumpUs_=decodeUs_=readUs_=rxGapUs_=0;listening_=autoListening_=false;controlAudio(0);speaking_=true;if(!controlAudio(2)){status("播放准备失败","error");break;}status("正在回答");}
                else if(!strcmp(state,"sentence_start")){
                    copy(answer_,str(message.value,"text"));
                    status("正在回答");}
                else if(!strcmp(state,"stop")){++ttsStops_;board::bufferedSpeakerFinish();resumeListening_=true;}
            }
        }
        used=0;stackFree_=uxTaskGetStackHighWaterMark(nullptr);
    }
    if(ready_)controlAudio(0);
}
// 语音笔记 (and 状态牌's spoken label): chat's connection, but each note listens from Confirm to Confirm and only the text the cloud heard is
// kept: chat() returns on it, so the connection closes before the cloud's reply arrives. The next connection opens
// at once; after the cloud drops an idle one, at the next Confirm, which then starts listening by itself.
void NativeServices::memo(){
    // A Confirm that found no task running (it ended on an error) starts one that listens at once.
    // 语音指令 listens as it opens (你好小智 has just been said): one phrase, then the task ends.
    const bool once=!strcmp(current_.operation,"speak")||current_.app==App::Voice;
    wantListen_=once||!strcmp(current_.operation,"toggle");
    const bool translating=current_.app==App::Translate;spoken_[0]=0;
    if(translating){page_.translation={};sayWanted_=false;}
    while(!cancelled()){
        idleDrop_=failed_=false;chat();closeSocket();audio_.stop();ready_=listening_=asr_=false;
        // 状态牌 and 随机转盘 ask for one phrase at a time: no connection is kept waiting for the next.
        if(!failed_&&!cancelled()&&*spoken_){if(current_.app==App::Stock)addStock(spoken_);else setCity(spoken_);}
        if(failed_||cancelled()||speaksOnce(current_.app)||once)return;
        if(translating&&*page_.translation.source)translate();
        if(translating&&sayWanted_.exchange(false))sayTranslation();
        if(cancelled())return;
        if(!idleDrop_)continue;
        status("云端待命");
        for(Job action{};!cancelled();)
            if(xQueueReceive(actions_,&action,pdMS_TO_TICKS(100))==pdTRUE&&action.generation==current_.generation){
                current_.request=action.request;
                if(translating&&!strcmp(action.operation,"say")){sayTranslation();status("云端待命");continue;}
                if(!strcmp(action.operation,"toggle")){wantListen_=true;break;}
            }
    }
}
namespace {
// Percent-encodes text into out (keeping whole characters), at most capacity - 1 bytes.
void urlEncode(char* out,size_t capacity,const char* text){
    size_t n=0;
    for(const auto* c=reinterpret_cast<const unsigned char*>(text);*c;){
        const unsigned length=*c<0x80?1:*c<0xe0?2:*c<0xf0?3:4;
        char piece[16];size_t used=0;
        for(unsigned k=0;k<length&&c[k];++k)used+=isalnum(c[k])||strchr("-_.~",c[k])?snprintf(piece+used,sizeof(piece)-used,"%c",c[k]):snprintf(piece+used,sizeof(piece)-used,"%%%02X",c[k]);
        if(n+used>=capacity)break;
        memcpy(out+n,piece,used);n+=used;c+=length;
    }
    out[n]=0;
}
// MyMemory returns quotes and ampersands as HTML entities.
void htmlEntities(char* text){
    static const char* const names[][2]{{"&#39;","'"},{"&quot;","\""},{"&amp;","&"},{"&lt;","<"},{"&gt;",">"}};
    for(char* at=text;(at=strchr(at,'&'));++at)
        for(const auto& e:names)if(!strncmp(at,e[0],strlen(e[0]))){*at=*e[1];memmove(at+1,at+strlen(e[0]),strlen(at+strlen(e[0]))+1);break;}
}
bool hasChinese(const char* text){
    for(const auto* c=reinterpret_cast<const unsigned char*>(text);*c;++c)if(*c>=0xe4&&*c<=0xe9)return true;  // U+4E00-U+9FFF
    return false;
}
}
// 中英翻译: text with Chinese in it goes to English, anything else to Chinese. 腾讯翻译君's web service (TranSmart:
// HTTPS, about 0.4 s) first, then MyMemory (plain HTTP, 500 bytes a request, 5000 characters a day); neither needs a key.
void NativeServices::translate(){
    auto& t=page_.translation;t.result[0]=0;t.engine=0;t.toEnglish=hasChinese(t.source);
    status("正在翻译");
    auto reply=std::unique_ptr<char[]>(new(std::nothrow)char[6144]);
    if(reply){
        Json body(cJSON_CreateObject());auto* header=cJSON_AddObjectToObject(body.value,"header");
        uint8_t mac[6];esp_read_mac(mac,ESP_MAC_WIFI_STA);char key[96];
        snprintf(key,sizeof(key),"browser-chrome-110.0.0-Mac OS-61692d70-6173-4000-8000-%02x%02x%02x%02x%02x%02x-%lld",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],static_cast<long long>(time(nullptr))*1000);
        cJSON_AddStringToObject(header,"fn","auto_translation");cJSON_AddStringToObject(header,"client_key",key);
        cJSON_AddStringToObject(body.value,"type","plain");cJSON_AddStringToObject(body.value,"model_category","normal");
        auto* source=cJSON_AddObjectToObject(body.value,"source");cJSON_AddStringToObject(source,"lang",t.toEnglish?"zh":"en");
        auto* list=cJSON_AddArrayToObject(source,"text_list");cJSON_AddItemToArray(list,cJSON_CreateString(t.source));
        cJSON_AddStringToObject(cJSON_AddObjectToObject(body.value,"target"),"lang",t.toEnglish?"en":"zh");
        char* raw=cJSON_PrintUnformatted(body.value);
        if(raw&&fetch("https://transmart.qq.com/api/imt",reply.get(),6144,nullptr,nullptr,raw)){
            Json json(cJSON_Parse(reply.get()));const auto* text=cJSON_GetArrayItem(item(json.value,"auto_translation"),0);
            if(cJSON_IsString(text)&&*text->valuestring){copy(t.result,text->valuestring);t.engine=1;}
        }
        cJSON_free(raw);
        if(!t.engine&&strlen(t.source)<=500&&!cancelled()){
            auto url=std::unique_ptr<char[]>(new(std::nothrow)char[1700]);
            if(url){
                const int n=snprintf(url.get(),1700,"http://api.mymemory.translated.net/get?langpair=%s&q=",t.toEnglish?"zh-CN%7Cen":"en%7Czh-CN");
                urlEncode(url.get()+n,1700-n,t.source);
                if(fetch(url.get(),reply.get(),6144)){
                    Json json(cJSON_Parse(reply.get()));const char* text=str(item(json.value,"responseData"),"translatedText");
                    if(num(json.value,"responseStatus")==200&&*text){copy(t.result,text);htmlEntities(t.result);t.engine=2;}
                }
            }
        }
    }
    // LiveServices copies it to the page; wait, as the status queue keeps only the latest update.
    translated_=true;
    while(translated_.load()&&!cancelled())vTaskDelay(pdMS_TO_TICKS(10));
    page_.translation.source[0]=0;
}
// Reads the translation aloud: Baidu Translate's speech (HTTPS only, MP3 at 16 kHz), English or Chinese.
void NativeServices::sayTranslation(){
    const auto& t=page_.translation;
    if(!*t.result){status("还没有译文");return;}
    auto url=std::unique_ptr<char[]>(new(std::nothrow)char[1800]);auto bytes=std::unique_ptr<uint8_t[]>(new(std::nothrow)uint8_t[1024]);
    if(!url||!bytes){status("内存不足");return;}
    status("正在朗读");
    const int n=snprintf(url.get(),1800,"https://fanyi.baidu.com/gettts?lan=%s&spd=5&source=web&text=",t.toEnglish?"en":"zh");
    urlEncode(url.get()+n,1800-n,t.result);
    esp_http_client_handle_t client=openStream(url.get(),8000,0,true);
    const int count=client?esp_http_client_read(client,reinterpret_cast<char*>(bytes.get()),1024):-1;
    const bool played=count>0&&playMp3(client,bytes.get(),1024,count);
    if(client&&count<=0){esp_http_client_close(client);esp_http_client_cleanup(client);}
    if(!cancelled())status(played?"朗读完毕":"朗读失败，长按上键重试");
}
esp_http_client_handle_t NativeServices::openStream(const char* url,int timeoutMs,uint32_t offset,bool secure){
    esp_http_client_config_t config{};config.url=url;config.timeout_ms=timeoutMs;config.buffer_size=1024;config.buffer_size_tx=2048;config.addr_type=HTTP_ADDR_TYPE_INET;config.user_agent="AI-Passport/0.3";
    if(secure)config.crt_bundle_attach=esp_crt_bundle_attach;
    auto connection=esp_http_client_init(&config);if(!connection)return nullptr;esp_http_client_set_header(connection,"Icy-MetaData","0");
    if(offset){char range[32];snprintf(range,sizeof(range),"bytes=%lu-",static_cast<unsigned long>(offset));esp_http_client_set_header(connection,"Range",range);}
    for(unsigned attempt=0;attempt<6&&!cancelled();++attempt){
        // Radio and word audio run on an 8 KiB stack that a TLS handshake would overflow; refuse HTTPS, redirects too.
        if(!secure&&esp_http_client_get_transport_type(connection)!=HTTP_TRANSPORT_OVER_TCP)break;
        ++httpCount_;const auto result=esp_http_client_open(connection,0);error_=result;
        if(result!=ESP_OK){int tlsCode=0,tlsFlags=0;tlsResult_=esp_http_client_get_and_clear_last_tls_error(connection,&tlsCode,&tlsFlags);tlsError_=tlsCode;tlsFlags_=tlsFlags;socketError_=esp_http_client_get_errno(connection);break;}
        esp_http_client_fetch_headers(connection);const int code=esp_http_client_get_status_code(connection);httpStatus_=code;
        if(code==200||(offset&&code==206))return connection;
        if(code<300||code>=400||esp_http_client_set_redirection(connection)!=ESP_OK)break;
        esp_http_client_close(connection);
    }esp_http_client_cleanup(connection);return nullptr;
}
void NativeServices::media(){
    unsigned index=0,stalls=0,retries=0;size_t streamed=0;bool playing=false,restart=false;esp_http_client_handle_t client=nullptr;
    auto bytes=std::unique_ptr<uint8_t[]>(new(std::nothrow)uint8_t[1024]);
    if(!bytes){publish(current_,"内存不足","error");return;}
    MediaPrefs prefs{};mediaLoad("prefs",prefs);
    const auto radios=pickRadios(prefs.city,prefs.province);const unsigned total=radios.count;
    const char* note=*prefs.city?"":"手机配置页可选择所在城市";
    const auto station=[&](unsigned i)->const RadioStation&{return kRadioStations[radios.index[i]];};
    const auto show=[&](const char* state,const char* phase="ready"){
        static constexpr const char* tiers[]{"本地","省内","全国"};const auto& s=station(index);
        char meta[40]{},area[96]{},upcoming[96]{};
        snprintf(meta,sizeof(meta),"%s%s",tiers[static_cast<unsigned>(radioTier(s,prefs.city,prefs.province))],s.url?"":" · 64k");
        if(*prefs.city)snprintf(area,sizeof(area),"所在城市：%s",prefs.city);else copy(area,"全国与网络电台");
        if(total>1)snprintf(upcoming,sizeof(upcoming),"下一个：%s",station((index+1)%total).name);
        const char* lines[]{meta,area,upcoming,note};
        mediaIndex_=index;mediaCount_=total;playing_=playing;
        publish(current_,state,phase,s.name,lines);
    };
    const auto close=[&](){if(client){esp_http_client_close(client);esp_http_client_cleanup(client);client=nullptr;}audio_.stop();};
    show("已停止");
    while(!cancelled()){
        Job action{};if(xQueueReceive(actions_,&action,pdMS_TO_TICKS(playing?0:20))==pdTRUE&&action.generation==current_.generation){
            current_.request=action.request;retries=0;
            if(!strcmp(action.operation,"toggle")){playing=!playing;restart=playing;close();if(!playing)show("已停止");}
            else if(!strcmp(action.operation,"next")||!strcmp(action.operation,"prev")){index=(index+(strcmp(action.operation,"next")?total-1:1))%total;restart=playing;close();if(!playing)show("已停止");}
            else show(playing?"正在播放":"已停止");
        }
        if(!playing)continue;
        if(restart){
            restart=false;stalls=0;streamed=0;
            show("电台连接中","loading");char url[256];
            radioUrl(station(index),url,sizeof(url));client=openStream(url);
            // Qingting serves the same relay from a second host.
            if(!client&&!station(index).url&&!cancelled()){radioUrl(station(index),url,sizeof(url),true);client=openStream(url);}
            if(!client){playing=false;show("媒体连接失败","error");continue;}
            if(!audio_.start(current_.app,current_.generation,true)){close();playing=false;show("播放内存不足","error");continue;}
            show("正在播放");
        }
        if(client){const int count=esp_http_client_read(client,reinterpret_cast<char*>(bytes.get()),1024);
            if(count>0){stalls=0;mediaBytes_+=count;if((streamed+=count)>65536)retries=0;if(!audio_.nativeInput(4,bytes.get(),count)){close();playing=false;show("当前音频无法解码","error");}}
            // A read timeout is a pause in a slow stream, not its end.
            else if(count==-ESP_ERR_HTTP_EAGAIN&&++stalls<4)continue;
            else{
                error_=count;socketError_=esp_http_client_get_errno(client);{int tlsCode=0,tlsFlags=0;tlsResult_=esp_http_client_get_and_clear_last_tls_error(client,&tlsCode,&tlsFlags);tlsError_=tlsCode;tlsFlags_=tlsFlags;}readFailHeap_=heap_caps_get_free_size(MALLOC_CAP_8BIT);close();
                // Live radio reconnects a few times before giving up.
                if(retries<3){++retries;show("电台重新连接","loading");restart=retryPause(retries);}
                else{playing=false;show(count==0?"播放结束":"媒体连接断开",count==0?"ready":"error");}
            }
        }
        stackFree_=uxTaskGetStackHighWaterMark(nullptr);
    }
    close();
}
// 播客: the program list, then one program's episodes, streamed like the radio (plain HTTP 64k MP3).
// "list" shows the programs; "play:N" plays program N where it was left (else its newest episode);
// "toggle" pauses and resumes from the byte reached (HTTP Range); "next"/"prev" step to the newer/older
// episode, and a finished episode moves on to the newer one. The place is kept on pause, on leaving and
// once a minute, so switching the badge off loses at most a minute.
void NativeServices::podcast(){
    static_assert(PodcastList::kMax==std::tuple_size_v<decltype(Model::PageData::podcasts)>);
    struct Episode{uint32_t track,duration,created;uint16_t index,total;bool paid;char title[96],url[160],backup[160];};
    struct State{PodcastList list;PodcastSpots spots;Episode ep,found;uint8_t bytes[1024];};
    auto state=std::unique_ptr<State>(new(std::nothrow)State{});
    if(!state){publish(current_,"内存不足","error");return;}
    auto& list=state->list;auto& spots=state->spots;auto& ep=state->ep;uint8_t* bytes=state->bytes;
    if(!mediaLoad("podcasts",list)||!list.count||list.count>PodcastList::kMax)list=defaultPodcasts();
    if(!mediaLoad("pod_spots",spots))spots={};
    unsigned program=0,stalls=0,retries=0;uint32_t offset=0,total=0;size_t skip=0;bool fresh=false,restart=false;
    int64_t shownAt=0,savedAt=0;char note[48]{};esp_http_client_handle_t client=nullptr;
    podcastPlayer_=playing_=false;
    const auto album=[&]{return list.items[program].album;};
    const auto rows=[&]{
        podcastCount_=list.count;
        for(unsigned i=0;i<list.count;++i){
            auto& row=page_.podcasts[i];copy(row.name,list.items[i].name);
            // 64 kbps: 8000 bytes a second.
            const auto* spot=podcastSpot(spots,list,list.items[i].album,false);
            if(spot&&spot->track)snprintf(row.note,sizeof(row.note),"听到第 %u 期 · %lu:%02lu",spot->episode+1,static_cast<unsigned long>(spot->offset/8000/60),static_cast<unsigned long>(spot->offset/8000%60));
            else copy(row.note,"从最新一期开始");
        }
    };
    const auto show=[&](const char* status,const char* phase="ready"){
        podcastPlayer_=ep.track!=0||strcmp(status,"选择节目");shownAt=esp_timer_get_time();
        if(!podcastPlayer_){rows();publish(current_,status,phase,"播客");return;}
        char info[64]{};
        if(ep.track){const time_t at=ep.created;tm date{};localtime_r(&at,&date);char day[16];strftime(day,sizeof(day),"%Y-%m-%d",&date);
            snprintf(info,sizeof(info),"第 %u 期%s · %s",ep.index+1,ep.index+1==ep.total?"（最新）":"",ep.created?day:"");}
        podcastPos_=total&&ep.duration?uint64_t(offset)*ep.duration/total:offset/8000;podcastDur_=ep.duration;
        const char* lines[]{list.items[program].name,info,note,""};
        publish(current_,status,phase,ep.track?ep.title:"",lines);
    };
    // Episode `index` (oldest = 0), or the newest when negative; ep is kept when it cannot be fetched.
    const auto look=[&](int index){
        static constexpr const char* keys[]{"totalCount","trackId","title","duration","createdAt","isPaid","playUrl64","playUrl32"};
        struct Sink{JsonScan scan;Episode* found;} sink{{keys,std::size(keys),"\"list\":[",[](void* context,unsigned key,const char* v){
            auto& e=*static_cast<Episode*>(context);
            switch(key){case 0:e.total=std::min(atol(v),65535L);break;case 1:e.track=strtoul(v,nullptr,10);break;case 2:copy(e.title,v);break;case 3:e.duration=atol(v);break;
                case 4:e.created=strtoull(v,nullptr,10)/1000;break;case 5:e.paid=!strcmp(v,"true");break;case 6:copy(e.url,v);break;default:copy(e.backup,v);}},&state->found},&state->found};
        char url[112];podcastTrackUrl(url,sizeof(url),album(),index);
        const bool fetched=fetch(url,nullptr,0,[](void* context,const char* data,size_t size){auto& s=*static_cast<Sink*>(context);if(!data){s.scan.reset();*s.found={};}else s.scan.feed(data,size);return true;},&sink);
        auto& found=state->found;
        if(!fetched||!found.track||!found.total||(!*found.url&&!*found.backup))return false;
        found.index=index<0?found.total-1:std::min(index,found.total-1);ep=found;return true;
    };
    const auto save=[&]{
        savedAt=esp_timer_get_time();if(!ep.track)return;
        if(auto* spot=podcastSpot(spots,list,album(),true)){spot->track=ep.track;spot->episode=ep.index;spot->offset=offset;mediaSave("pod_spots",spots);}
    };
    const auto close=[&]{if(client){esp_http_client_close(client);esp_http_client_cleanup(client);client=nullptr;}audio_.stop();playing_=false;};
    // Fetches an episode and starts it at `from`; paid ones are not played.
    const auto start=[&](int index,uint32_t from){
        close();show("正在获取节目","loading");
        if(!look(index)){if(!cancelled())show("节目获取失败，确认重试","error");return;}
        offset=from;total=0;
        if(ep.paid){show("付费节目，长按上下换一期");return;}
        playing_=restart=true;retries=0;
    };
    show("选择节目");
    while(!cancelled()){
        Job action{};
        if(xQueueReceive(actions_,&action,pdMS_TO_TICKS(playing_?0:20))==pdTRUE&&action.generation==current_.generation){
            current_.request=action.request;const char* op=action.operation;
            if(!strncmp(op,"play:",5)){
                if(ep.track){close();save();}
                program=std::min<unsigned>(atoi(op+5),list.count-1);ep={};note[0]=0;
                // Where it was left, a few seconds back; a removed episode starts its successor from the top.
                const auto* spot=podcastSpot(spots,list,album(),false);
                if(spot&&spot->track){
                    start(spot->episode,0);
                    if(ep.track==spot->track&&playing_)offset=spot->offset>24000?spot->offset-24000:0;
                }else start(-1,0);
            }else if(!strcmp(op,"list")){if(ep.track){close();save();}ep={};show("选择节目");}
            else if(!ep.track){
                // A failed first fetch: Confirm (sent as "enter") tries again.
                if(!strcmp(op,"enter")&&podcastPlayer_)start(-1,0);else show(podcastPlayer_?"节目获取失败，确认重试":"选择节目",podcastPlayer_?"error":"ready");
            }
            else if(!strcmp(op,"toggle")||!strcmp(op,"enter")){
                if(playing_){close();save();show("已暂停");}else if(!ep.paid){playing_=restart=true;retries=0;}
            }else if(!strcmp(op,"next")||!strcmp(op,"prev")){
                const bool newer=!strcmp(op,"next");const int want=int(ep.index)+(newer?1:-1);
                if(want<0||want>=ep.total){copy(note,newer?"已是最新一期":"已是第一期");show(playing_?"正在播放":"已暂停");}
                else{note[0]=0;const bool was=playing_;save();start(want,0);if(!was&&playing_&&ep.track){playing_=false;restart=false;show("已暂停");}}
            }else show(playing_?"正在播放":"已暂停");
        }
        if(!playing_)continue;
        if(restart){
            restart=false;stalls=0;
            show(offset?"继续播放中":"正在连接","loading");
            client=openStream(*ep.url?ep.url:ep.backup,3000,offset);
            if(!client){close();show("连接失败，确认重试","error");continue;}
            // A server that ignores the range sends the whole file from its start.
            if(offset&&esp_http_client_get_status_code(client)==200)offset=0;
            const int64_t length=esp_http_client_get_content_length(client);total=length>0?offset+length:0;
            fresh=!offset;skip=0;
            if(!audio_.start(App::Radio,current_.generation,true)){close();show("播放内存不足","error");continue;}
            playing_=true;show("正在播放");
        }
        const int count=esp_http_client_read(client,reinterpret_cast<char*>(bytes),sizeof(state->bytes));
        if(count>0){
            stalls=0;mediaBytes_+=count;
            // An ID3 tag (often with cover art) precedes the first frame.
            if(fresh){fresh=false;if(count>=10&&!memcmp(bytes,"ID3",3))skip=10+((bytes[6]&127)<<21|(bytes[7]&127)<<14|(bytes[8]&127)<<7|(bytes[9]&127));}
            const size_t drop=std::min<size_t>(skip,count);skip-=drop;offset+=count;
            if(count>int(drop)&&!audio_.nativeInput(4,bytes+drop,count-drop)){close();show("当前音频无法解码","error");continue;}
            if(esp_timer_get_time()-savedAt>60000000)save();
            if(esp_timer_get_time()-shownAt>1000000)show("正在播放");
        }
        else if(count==-ESP_ERR_HTTP_EAGAIN&&++stalls<4)continue;
        else if(total&&offset>=total){
            // The end: on to the newer episode, or stop at the newest (its spot stays at the end).
            vTaskDelay(pdMS_TO_TICKS(250));close();save();
            if(ep.index+1<ep.total)start(ep.index+1,0);else{copy(note,"已播完最新一期");show("已播完");}
        }else{
            error_=count;socketError_=esp_http_client_get_errno(client);readFailHeap_=heap_caps_get_free_size(MALLOC_CAP_8BIT);close();
            // A dropped connection resumes from the byte reached.
            if(retries<3){++retries;show("重新连接","loading");playing_=restart=retryPause(retries);}
            else{save();show("连接断开，确认重试","error");}
        }
        stackFree_=uxTaskGetStackHighWaterMark(nullptr);
    }
    close();save();
}
// 背单词 keeps only the headwords in flash. Each card's IPA and meaning come from Youdao's dictionary
// ("look:N", plain HTTP, the host that also serves the pronunciation); every publish carries the word
// as value and {IPA, meaning} as lines, so the card can tell the answer is for it.
// The pronunciation is British first, to match the IPA; Youdao serves some British clips as 48 kHz WAV,
// too big for the link and RAM, so those words fall back to the American MP3.
void NativeServices::words(){
    auto bytes=std::unique_ptr<uint8_t[]>(new(std::nothrow)uint8_t[1024]);
    if(!bytes){publish(current_,"内存不足","error");return;}
    struct Entry{int word=-1;char ipa[48]{},meaning[64]{};} entry;  // meaning: two lines of the card
    const auto show=[&](const char* state,const char* phase="ready"){const char* lines[]{entry.ipa,entry.meaning,"",""};publish(current_,state,phase,entry.word<0?"":wordAt(entry.word),lines);};
    const auto look=[&](unsigned word){
        entry={};entry.word=word;
        // The phonetics and the first sense sit in the first 2 KB of a 2-8 KB reply; read no further.
        struct Head{size_t used;char text[2048];};auto head=std::unique_ptr<Head>(new(std::nothrow)Head);if(!head)return false;
        char url[200];snprintf(url,sizeof(url),"http://dict.youdao.com/fsearch?client=deskdict&keyfrom=chrome.extension&q=%s&pos=-1&doctype=xml&xmlVersion=3.2&dogVersion=1.0&vendor=unknown&appVer=3.1.17.4208",wordAt(word));
        if(!fetch(url,nullptr,0,[](void* context,const char* data,size_t size){auto& h=*static_cast<Head*>(context);if(!data){h.used=0;h.text[0]=0;return true;}
            size=std::min(size,sizeof(h.text)-1-h.used);memcpy(h.text+h.used,data,size);h.used+=size;h.text[h.used]=0;
            return h.used<sizeof(h.text)-1&&!strstr(h.text,"</translation>");},head.get()))return false;
        const auto field=[&](const char* open,const char* close)->const char*{char* at=strstr(head->text,open);if(!at)return "";at+=strlen(open);if(char* end=strstr(at,close))*end=0;return at;};
        // field() ends each value in place, so take the later one (the meaning) first.
        const char* meaning=field("<content><![CDATA[","]]>");copy(entry.meaning,meaning);
        copy(entry.ipa,field("<phonetic-symbol>","</phonetic-symbol>"));
        // Too long for two lines: end at the last complete sense.
        if(strlen(meaning)>=sizeof(entry.meaning)){char* cut=nullptr;for(char* at=entry.meaning;(at=strstr(at,"；"));at+=3)cut=at;if(cut)*cut=0;}
        return *entry.meaning!=0;
    };
    Job action=current_;bool queued=true;  // the job that started the task is the first action
    while(!cancelled()){
        if(!queued&&(xQueueReceive(actions_,&action,pdMS_TO_TICKS(50))!=pdTRUE||action.generation!=current_.generation))continue;
        queued=false;current_.request=action.request;
        const bool say=!strncmp(action.operation,"say:",4);
        const unsigned word=say||!strncmp(action.operation,"look:",5)?atoi(strchr(action.operation,':')+1):kWordCount;
        if(word>=kWordCount){show("按确认朗读");continue;}
        if(entry.word!=static_cast<int>(word)||!*entry.meaning){
            show("正在查询释义","loading");
            if(!look(word)){if(!cancelled())show("释义获取失败，确认重试","error");continue;}
        }
        if(!say){show("按确认朗读");continue;}
        show("正在朗读","loading");
        esp_http_client_handle_t client=nullptr;int count=0;
        for(unsigned attempt=0;attempt<6&&!client&&!cancelled();++attempt){
            // The host is slow to accept now and then; a fresh connection usually answers at once.
            char url[96];snprintf(url,sizeof(url),"http://dict.youdao.com/dictvoice?audio=%s&type=%u",wordAt(word),attempt<3?1:2);
            client=openStream(url,2500);if(!client)continue;
            count=esp_http_client_read(client,reinterpret_cast<char*>(bytes.get()),1024);
            if(count<4||!memcmp(bytes.get(),"RIFF",4)){esp_http_client_close(client);esp_http_client_cleanup(client);client=nullptr;if(count>=4)attempt=std::max(attempt,2U);}
        }
        if(!client){if(!cancelled())show("发音下载失败，确认重试","error");continue;}
        const bool played=playMp3(client,bytes.get(),1024,count);
        if(!cancelled())show(played?"按确认朗读":"朗读失败，确认重试",played?"ready":"error");
    }
}
// Streams an opened MP3 response to the speaker (count bytes already read into bytes) and closes it.
bool NativeServices::playMp3(esp_http_client_handle_t client,uint8_t* bytes,size_t size,int count){
    bool played=audio_.start(App::Radio,current_.generation,true);
    // An ID3 tag precedes the first frame.
    size_t skip=count>=10&&!memcmp(bytes,"ID3",3)?10+((bytes[6]&127)<<21|(bytes[7]&127)<<14|(bytes[8]&127)<<7|(bytes[9]&127)):0;
    for(unsigned stalls=0;played&&!cancelled();){
        if(count>0){stalls=0;mediaBytes_+=count;const size_t drop=std::min<size_t>(skip,count);skip-=drop;
            if(count>int(drop)&&!audio_.nativeInput(4,bytes+drop,count-drop))played=false;}
        else if(count==-ESP_ERR_HTTP_EAGAIN&&++stalls<4){}
        else{if(count<0)played=false;break;}
        count=esp_http_client_read(client,reinterpret_cast<char*>(bytes),size);
    }
    esp_http_client_close(client);esp_http_client_cleanup(client);
    vTaskDelay(pdMS_TO_TICKS(250)); // The speaker's DMA still holds the last frames.
    audio_.stop();stackFree_=uxTaskGetStackHighWaterMark(nullptr);
    return played;
}
// 历史今日: query.asilu.com, plain HTTP and no key, about 2 KB of today's events (no date parameter).
void NativeServices::history(){
    publish(current_,"正在获取历史今日","loading");
    std::unique_ptr<char[]> data(new(std::nothrow) char[4096]);
    if(!data){publish(current_,"内存不足","error");return;}
    if(!fetch("http://query.asilu.com/today/list/",data.get(),4096)){publish(current_,"历史今日获取失败","error");return;}
    Json json(cJSON_Parse(data.get()));data.reset();
    eventCount_=0;const cJSON* entry;
    cJSON_ArrayForEach(entry,item(json.value,"data")){
        if(eventCount_==std::size(events_))break;
        auto& e=events_[eventCount_++];const double year=num(entry,"year");const char* type=str(entry,"type");
        e.year=std::isfinite(year)?std::clamp(year,-9999.0,9999.0):0;e.type=!strcmp(type,"birth")?1:!strcmp(type,"death")?2:0;copy(e.title,str(entry,"title"));
    }
    if(!eventCount_){publish(current_,"历史今日解析失败","error");return;}
    publish(current_,"已更新","ready");
}
// 每日一句: 金山词霸's sentence.iciba.com HTTPS JSON (no key; the old open.iciba.com/dsapi stopped answering). The task stays for the page: "day:N" fetches N days back and
// "say" plays the sentence, whose mp3 the CDN also serves over plain HTTP (openStream refuses TLS).
void NativeServices::daily(){
    daily_={};publish(current_,"正在获取每日一句","loading");
    if(!syncClock()){publish(current_,"网络校时失败","error");return;}
    auto bytes=std::unique_ptr<uint8_t[]>(new(std::nothrow)uint8_t[2048]);
    if(!bytes){publish(current_,"内存不足","error");return;}
    unsigned back=0;char audio[128]{};
    for(;;){
        if(!strcmp(current_.operation,"say")){
            esp_http_client_handle_t client=*audio?openStream(audio):nullptr;
            const int count=client?esp_http_client_read(client,reinterpret_cast<char*>(bytes.get()),1024):-1;
            if(client)publish(current_,"正在朗读","loading");
            const bool played=count>0&&playMp3(client,bytes.get(),1024,count);
            if(client&&count<=0){esp_http_client_close(client);esp_http_client_cleanup(client);}
            if(!cancelled())publish(current_,played?"按确认朗读":"朗读失败，确认重试",played?"ready":"error");
        }else{
            if(!strncmp(current_.operation,"day:",4))back=std::clamp(atoi(current_.operation+4),0,365);
            const time_t at=time(nullptr)-time_t(back)*86400;tm t{};localtime_r(&at,&t);
            char url[112];snprintf(url,sizeof(url),"https://sentence.iciba.com/index.php?c=dailysentence&m=getdetail&title=%04d-%02d-%02d",t.tm_year+1900,t.tm_mon+1,t.tm_mday);
            char* text=reinterpret_cast<char*>(bytes.get());
            Json json(fetch(url,text,2048)?cJSON_Parse(text):nullptr);
            if(!*str(json.value,"content")){if(!cancelled())publish(current_,"每日一句获取失败","error");}
            else{
                copy(daily_.en,str(json.value,"content"));copy(daily_.cn,str(json.value,"note"));copy(daily_.date,str(json.value,"title"));daily_.back=back;
                asciiPunctuation(daily_.en);
                const char* tts=str(json.value,"tts");if(!strncmp(tts,"https://",8))snprintf(audio,sizeof(audio),"http://%s",tts+8);else copy(audio,tts);
                publish(current_,"按确认朗读","ready");
            }
        }
        Job action{};
        while(!cancelled()&&(xQueueReceive(actions_,&action,pdMS_TO_TICKS(50))!=pdTRUE||action.generation!=current_.generation)){}
        if(cancelled())return;
        current_.request=action.request;copy(current_.operation,action.operation);
    }
}
// 地震速报: 中国地震台网's list as mirrored by wolfx (HTTPS, no key). The 14 KB reply holds 50 quakes; it is read
// as a stream and only the first 16 entries are kept, so no buffer holds the whole of it.
void NativeServices::quake(){
    publish(current_,"正在获取地震速报","loading");
    struct Parse{NativeServices* self;char entry[512];size_t used;unsigned depth;bool quoted,escaped;} ;
    auto parse=std::unique_ptr<Parse>(new(std::nothrow)Parse{this,{},0,0,false,false});
    if(!parse){publish(current_,"内存不足","error");return;}
    // {"No1":{...},"No2":{...}}: each depth-2 object is one quake; returning false ends the download.
    const Sink sink=[](void* context,const char* data,size_t size)->bool{
        auto& p=*static_cast<Parse*>(context);auto& self=*p.self;
        if(!data){p.used=p.depth=0;p.quoted=p.escaped=false;self.quakeCount_=0;return true;}
        for(size_t i=0;i<size;++i){
            const char c=data[i];
            if(p.depth==2&&p.used<sizeof(p.entry)-1)p.entry[p.used++]=c;
            if(p.quoted){if(p.escaped)p.escaped=false;else if(c=='\\')p.escaped=true;else if(c=='"')p.quoted=false;continue;}
            if(c=='"')p.quoted=true;
            else if(c=='{'&&++p.depth==2){p.entry[0]=c;p.used=1;}
            else if(c=='}'&&p.depth&&p.depth--==2){
                p.entry[p.used]=0;Json json(cJSON_Parse(p.entry));int y,mo,d,h,mi,sec;
                if(sscanf(str(json.value,"time"),"%d-%d-%d %d:%d:%d",&y,&mo,&d,&h,&mi,&sec)==6&&*str(json.value,"placeName")){
                    auto& q=self.page_.quakes[self.quakeCount_++];
                    q.at=dayNumber({y,mo,d})*86400LL+h*3600+mi*60+sec-8*3600;  // Beijing time
                    q.mag=std::clamp(std::lround(atof(str(json.value,"magnitude"))*10),0L,99L);q.depth=std::clamp(atoi(str(json.value,"depth")),0,999);copy(q.place,str(json.value,"placeName"));
                    if(self.quakeCount_==std::size(self.page_.quakes))return false;
                }
            }
        }
        return true;
    };
    if(!fetch("https://api.wolfx.jp/cenc_eqlist.json",nullptr,0,sink,parse.get())||!quakeCount_){if(!cancelled())publish(current_,"地震速报获取失败","error");return;}
    publish(current_,"已更新","ready");
}
// Hacker News front page through Algolia's search API: one HTTPS request, about 2.5 KB for ten stories.
void NativeServices::hacker(){
    publish(current_,"正在获取 Hacker News","loading");
    std::unique_ptr<char[]> data(new(std::nothrow) char[6144]);
    if(!data){publish(current_,"内存不足","error");return;}
    if(!fetch("https://hn.algolia.com/api/v1/search?tags=front_page&hitsPerPage=10&attributesToRetrieve=title,points,num_comments,url,created_at_i&attributesToHighlight=%5B%5D",data.get(),6144)){publish(current_,"Hacker News 获取失败","error");return;}
    Json json(cJSON_Parse(data.get()));data.reset();
    storyCount_=0;const cJSON* entry;
    cJSON_ArrayForEach(entry,item(json.value,"hits")){
        if(storyCount_==std::size(page_.stories))break;
        auto& s=page_.stories[storyCount_++];copy(s.title,str(entry,"title"));asciiPunctuation(s.title);
        // The site: the URL's host without "www."; Ask HN posts have none.
        const char* host=strstr(str(entry,"url"),"://");host=host?host+3:"";if(!strncmp(host,"www.",4))host+=4;
        copy(s.site,host);if(char* slash=strchr(s.site,'/'))*slash=0;
        const double points=num(entry,"points"),comments=num(entry,"num_comments"),at=num(entry,"created_at_i");
        s.points=std::isfinite(points)?std::clamp(points,0.0,65535.0):0;s.comments=std::isfinite(comments)?std::clamp(comments,0.0,65535.0):0;s.at=std::isfinite(at)&&at>0?at:0;
    }
    if(!storyCount_){publish(current_,"Hacker News 解析失败","error");return;}
    publish(current_,"已更新","ready");
}
// 台风路径: this season's typhoons from 中央气象台 (plain HTTP, no key; about 3 KB, newest first), then the track of
// row "show:<i>". Every fix there carries its own forecast (about 600 bytes each, 12 KB three days in), so the
// reply is read a fix at a time and only the last fix's forecast is kept.
void NativeServices::typhoon(){
    publish(current_,"正在获取台风","loading");
    unsigned want=0;sscanf(current_.operation,"show:%u",&want);
    auto& t=page_.typhoon;t={};t.shown=255;t.cityLat=INT16_MIN;
    if(MediaPrefs prefs{};mediaLoad("prefs",prefs)&&*prefs.city){
        copy(t.city,prefs.city);simplifyText(t.city);t.cityLon=int16_t(std::lround(prefs.longitude*10));t.cityLat=int16_t(std::lround(prefs.latitude*10));}
    // One reader for both replies: the list's rows are 3 brackets deep, the track's fixes 4.
    struct Parse{TyphoonView* view;TyphoonTrack track;unsigned depth;JsonSlices::Item item;JsonSlices slices;uint32_t ids[TyphoonView::kRows];char buffer[2048];};
    std::unique_ptr<Parse> parse(new(std::nothrow) Parse{&t,TyphoonTrack(t.track),3,nullptr,{0,nullptr,0,nullptr,nullptr},{},{}});
    if(!parse){publish(current_,"内存不足","error");return;}
    const Sink sink=[](void* context,const char* bytes,size_t size){
        auto& p=*static_cast<Parse*>(context);auto& v=*p.view;
        if(!bytes){
            if(p.depth==3)v.count=0;else{v.points=v.ahead=v.peakWind=0;v.lowest=0;p.track=TyphoonTrack(v.track);}
            p.slices=JsonSlices(p.depth,p.buffer,sizeof(p.buffer),p.item,&p);return true;
        }
        p.slices.feed(bytes,size);return true;
    };
    // [3368875,"IN-FA","烟花","2631","2631",null,"烟花","start"]; a nameless depression is "热带低压", "20260027".
    parse->item=[](void* context,char* text){
        auto& p=*static_cast<Parse*>(context);auto& v=*p.view;if(v.count==TyphoonView::kRows)return;
        Json json(cJSON_Parse(text));const auto* a=json.value;
        const char* name=cJSON_GetStringValue(cJSON_GetArrayItem(a,2));const char* number=cJSON_GetStringValue(cJSON_GetArrayItem(a,3));
        const double id=cJSON_GetNumberValue(cJSON_GetArrayItem(a,0));if(!name||!number||!(id>0))return;
        auto& r=v.rows[v.count];p.ids[v.count++]=uint32_t(id);
        copy(r.name,name);for(char* c=r.name;*c;++c)if(static_cast<unsigned char>(*c)<' ')*c=0;  // "小熊\n"
        copy(r.number,strlen(number)==4?number:"");
        const char* status=cJSON_GetStringValue(cJSON_GetArrayItem(a,7));r.active=status&&!strcmp(status,"start");
    };
    if(!fetch("http://typhoon.nmc.cn/weatherservice/typhoon/jsons/list_default",nullptr,0,sink,parse.get())||!t.count){if(!cancelled())publish(current_,"台风列表获取失败","error");return;}
    if(want>=t.count)want=0;
    if(cancelled())return;
    // A fix: [id, "202610260300" (UTC), epoch ms, grade, lon, lat, hPa, m/s, heading, km/h, wind radii, {"BABJ": forecast}, ...];
    // a forecast point: [hours, time, lon, lat, hPa, m/s, agency, grade]. The season's index (depth 4 too) is skipped.
    parse->depth=4;
    parse->item=[](void* context,char* text){
        auto& p=*static_cast<Parse*>(context);auto& v=*p.view;
        Json json(cJSON_Parse(text));const auto* a=json.value;
        const char* when=cJSON_GetStringValue(cJSON_GetArrayItem(a,1));const char* grade=cJSON_GetStringValue(cJSON_GetArrayItem(a,3));
        const double lon=cJSON_GetNumberValue(cJSON_GetArrayItem(a,4)),lat=cJSON_GetNumberValue(cJSON_GetArrayItem(a,5));
        if(!when||strlen(when)!=12||!grade||!std::isfinite(lon)||!std::isfinite(lat))return;
        const auto number=[&](const cJSON* array,int i,double high){const double n=cJSON_GetNumberValue(cJSON_GetArrayItem(array,i));return std::isfinite(n)?std::clamp(n,0.0,high):0.0;};
        const auto tenths=[](double degrees,double limit){return int16_t(std::lround(std::clamp(degrees,-limit,limit)*10));};
        const uint8_t level=typhoonGrade(grade);
        p.track.add({tenths(lon,360),tenths(lat,90),level});v.points=p.track.count;
        v.at=uint32_t(number(a,2,4e12)/1000);v.pressure=uint16_t(number(a,6,1100));v.wind=uint8_t(number(a,7,99));v.speed=uint8_t(number(a,9,99));
        const char* heading=cJSON_GetStringValue(cJSON_GetArrayItem(a,8));copy(v.heading,heading?heading:"");
        if(v.wind>=v.peakWind){v.peakWind=v.wind;v.peakGrade=level;}
        if(v.pressure&&(!v.lowest||v.pressure<v.lowest))v.lowest=v.pressure;
        v.ahead=0;const cJSON* point;
        cJSON_ArrayForEach(point,cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(a,11),"BABJ")){
            const double x=cJSON_GetNumberValue(cJSON_GetArrayItem(point,2)),y=cJSON_GetNumberValue(cJSON_GetArrayItem(point,3));const char* g=cJSON_GetStringValue(cJSON_GetArrayItem(point,7));
            if(v.ahead<TyphoonView::kAhead&&std::isfinite(x)&&std::isfinite(y))v.forecast[v.ahead++]={tenths(x,360),tenths(y,90),typhoonGrade(g?g:""),uint8_t(number(point,0,255))};
        }
    };
    char url[96];snprintf(url,sizeof(url),"http://typhoon.nmc.cn/weatherservice/typhoon/jsons/view_%lu",static_cast<unsigned long>(parse->ids[want]));
    if(!fetch(url,nullptr,0,sink,parse.get())||!t.points){if(!cancelled())publish(current_,"台风路径获取失败","error");return;}
    t.shown=want;
    if(cancelled())return;
    publish(current_,"已更新","ready");
}
// 百度热搜: the board's PC feed (HTTPS, no key, about 57 KB for 50 items with summaries), read as it arrives and the
// first kHotRows kept, the pinned item ("topContent", after the rest) first. The task stays for the page: "item:<i>"
// sends that item in full from what was read, anything else fetches the board again.
bool NativeServices::hotBoard(){
    std::unique_ptr<HotBoardScan> board(new(std::nothrow) HotBoardScan(hot_));
    if(!board)return false;
    hotCount_=0;  // a failed read leaves the items half overwritten
    const Sink sink=[](void* context,const char* bytes,size_t size){
        auto& b=*static_cast<HotBoardScan*>(context);if(!bytes)b.start();else b.feed(bytes,size);return true;
    };
    if(!fetch("https://top.baidu.com/api/board?platform=pc&tab=realtime",nullptr,0,sink,board.get())||!board->count())return false;
    hotCount_=board->count();hotUpdated_=time(nullptr)>1700000000?time(nullptr):0;
    return true;
}
void NativeServices::hot(){
    publish(current_,"正在获取百度热搜","loading");
    std::unique_ptr<HotItem[]> items(new(std::nothrow) HotItem[kHotRows]);
    if(!items){publish(current_,"内存不足","error");return;}
    hot_=items.get();hotCount_=hotIndex_=0;
    for(;;){
        bool ok=true;
        if(!strncmp(current_.operation,"item:",5)&&hotCount_)hotIndex_=std::min<unsigned>(atoi(current_.operation+5),hotCount_-1);
        else if(!(ok=hotBoard())&&!cancelled())publish(current_,"百度热搜获取失败","error");
        if(cancelled())break;
        if(ok)publish(current_,"已更新","ready");
        Job action{};
        while(!cancelled()&&(xQueueReceive(actions_,&action,pdMS_TO_TICKS(50))!=pdTRUE||action.generation!=current_.generation)){}
        if(cancelled())break;
        current_.request=action.request;copy(current_.operation,action.operation);
    }
    hot_=nullptr;hotCount_=0;
}
// 知识问答: Open Trivia DB (HTTPS, no key, English). Ten shuffled questions per request; the task stays for the
// page and publishes the next on "next". The source refuses a second request within 5 s (response_code 5).
void NativeServices::quiz(){
    page_.quiz.serial=0;publish(current_,"正在获取题目","loading");
    Json batch;const cJSON* next=nullptr;
    // Only "next" moves on; any other request (a repeated "enter") shows the same question again.
    for(bool advance=true;;advance=!strcmp(current_.operation,"next")){
        if(advance&&!next){
            cJSON_Delete(batch.value);batch.value=nullptr;
            for(unsigned attempt=0;attempt<3&&!next&&!cancelled();++attempt){
                if(attempt&&!retryPause(7))return;
                std::unique_ptr<char[]> data(new(std::nothrow) char[4096]);
                if(!data){publish(current_,"内存不足","error");return;}
                if(!fetch("https://opentdb.com/api.php?amount=10&type=multiple&encode=url3986",data.get(),4096))continue;
                cJSON_Delete(batch.value);batch.value=cJSON_Parse(data.get());
                if(num(batch.value,"response_code")==0)next=cJSON_GetArrayItem(item(batch.value,"results"),0);
            }
            if(!next){if(!cancelled())publish(current_,"题目获取失败，确认重试","error");return;}
        }
        if(advance){
            auto& q=page_.quiz;const unsigned serial=q.serial+1;q={};q.serial=serial;
            decoded(q.question,str(next,"question"));decoded(q.category,str(next,"category"));decoded(q.level,str(next,"difficulty"));
            // The right answer goes to a random place among the three wrong ones.
            q.correct=esp_random()%4;const cJSON* wrong=item(next,"incorrect_answers");
            for(unsigned i=0,w=0;i<4;++i){if(i==q.correct)decoded(q.answers[i],str(next,"correct_answer"));else{const auto* a=cJSON_GetArrayItem(wrong,w++);decoded(q.answers[i],cJSON_IsString(a)?a->valuestring:"");}}
            next=next->next;
        }
        publish(current_,"已更新","ready");
        Job action{};
        while(!cancelled()&&(xQueueReceive(actions_,&action,pdMS_TO_TICKS(50))!=pdTRUE||action.generation!=current_.generation)){}
        if(cancelled())return;
        current_.request=action.request;copy(current_.operation,action.operation);
    }
}
void NativeServices::snapshot(char* out,size_t size)const{
    char playback[240];board::bufferedSpeakerSnapshot(playback,sizeof(playback));
    unsigned refRate{},refDropped{};board::referenceStats(refRate,refDropped);
    snprintf(out,size,"{\"last_failed_allocation\":%u,\"busy\":%s,\"location_allowed\":%s,\"http_requests\":%u,\"http_status\":%u,\"error\":%d,\"tls_result\":%d,\"tls_code\":%d,\"tls_flags\":%d,\"socket_error\":%d,\"weather_updates\":%u,\"chat_ready\":%s,\"chat_tx\":%u,\"chat_rx\":%u,\"tts_starts\":%u,\"tts_stops\":%u,\"listen_starts\":%u,\"listening\":%s,\"speaking\":%s,\"playback\":%s,\"media_bytes\":%u,\"stack_free\":%u}",passport_last_failed_allocation(),busy_?"true":"false",allowed_?"true":"false",httpCount_.load(),httpStatus_.load(),error_.load(),tlsResult_.load(),tlsError_.load(),tlsFlags_.load(),socketError_.load(),weatherUpdates_.load(),ready_?"true":"false",chatTx_.load(),chatRx_.load(),ttsStarts_.load(),ttsStops_.load(),listenStarts_.load(),listening_?"true":"false",speaking_?"true":"false",playback,mediaBytes_.load(),stackFree_.load());
    const size_t used=strlen(out);if(used&&used<size)snprintf(out+used-1,size-used+1,",\"generation\":%u,\"pump_us\":%u,\"decode_us\":%u,\"read_us\":%u,\"rx_gap_us\":%u,\"decoder_result\":%d,\"ref_rate\":%u,\"ref_dropped\":%u,\"mic_gain_db\":%u}",unsigned(owner_.load()),pumpUs_.load(),decodeUs_.load(),readUs_.load(),rxGapUs_.load(),audio_.decoderResult.load(),refRate,refDropped,board::microphoneGain());
    char gate[230];audio_.gateSnapshot(gate,sizeof(gate));const size_t tail=strlen(out);
    if(tail&&tail<size)snprintf(out+tail-1,size-tail+1,",\"gate\":%s,\"voice_barge_in\":false,\"barge_aborts\":0,\"barge_acks\":0,\"barge_recoveries\":0,\"barge_mem\":[0,0]}",gate);
    const size_t end=strlen(out);if(end&&end<size)snprintf(out+end-1,size-end+1,",\"prepare\":[%u,%u,%u]}",audio_.prepareError.load(),audio_.prepareFree.load(),audio_.prepareLargest.load());
    const size_t check=strlen(out);if(check&&check+100<size)snprintf(out+check-1,size-check+1,",\"cloud_check\":[%u,%u,%u],\"http_retries\":%u,\"read_fail_heap\":%u}",cloudChecks_.load(),cloudCheckOk_?1U:0U,connectAttempts_.load(),httpRetries_.load(),readFailHeap_.load());
}
}
