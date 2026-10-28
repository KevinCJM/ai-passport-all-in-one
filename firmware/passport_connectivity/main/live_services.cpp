#include "live_services.hpp"
#include "network_policy.hpp"
#include "json_output.hpp"
#ifdef PASSPORT_DEVICE
#include "board.hpp"
#include "native_services.hpp"
#include "voice_add.hpp"
#endif
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>
#include <new>
#include <string>
#include <sys/time.h>
#include "cJSON.h"
#include "esp_random.h"
#include "nvs.h"
#include "mbedtls/platform_util.h"

namespace passport {
namespace {
const cJSON* field(const cJSON* object, const char* name) { return cJSON_GetObjectItemCaseSensitive(object,name); }
const char* string(const cJSON* object,const char* name) {
    const auto* item=field(object,name); return cJSON_IsString(item) ? item->valuestring : "";
}
double number(const cJSON* object,const char* name,double fallback=-1) {
    const auto* item=field(object,name); return cJSON_IsNumber(item) && std::isfinite(item->valuedouble) && std::trunc(item->valuedouble)==item->valuedouble ? item->valuedouble : fallback;
}
// 自选股 asks for what the page shows: the list, or one row with its minutes (0) or 30 days (1).
void stockOperation(const Model& m,char (&out)[16]) {
    if(m.stockDetail)snprintf(out,sizeof(out),"chart:%u:%u",m.stockIndex,unsigned(m.stockDays));else snprintf(out,sizeof(out),"list");
}
template<size_t N> void copy(char (&to)[N],const char* from) {
    size_t count=std::min(strlen(from),N-1);
    // Truncate at a UTF-8 boundary, never halfway through a Chinese character.
    while (count && (static_cast<unsigned char>(from[count])&0xc0)==0x80) --count;
    memcpy(to,from,count); to[count]=0;
}
struct LegacyTodo {char title[80]{};bool done{};};
struct LegacyFocusRecord {uint32_t version{1};uint64_t remaining{1500};uint8_t count{};std::array<LegacyTodo,16> todos{};};
struct FocusRecord {uint32_t version{2},phase{},rounds{};std::array<uint32_t,3> periods{1500,300,900};uint64_t remaining{1500};};
esp_err_t route(httpd_req_t* request) { return static_cast<LiveServices*>(request->user_ctx)->receive(request); }
esp_err_t statusRoute(httpd_req_t* request) { return static_cast<LiveServices*>(request->user_ctx)->snapshot(request); }
}
void LiveServices::init(httpd_handle_t server) {
#ifdef PASSPORT_DEVICE
    // Same two-message budget: replace UI snapshots, never owner controls.
    messages_=xQueueCreate(1,sizeof(Message));
    nativeMessages_=xQueueCreate(1,sizeof(Message));configASSERT(nativeMessages_);
#else
    messages_=xQueueCreate(2,sizeof(Message));
#endif
    configASSERT(messages_);
    viewMutex_=xSemaphoreCreateMutexStatic(&viewStorage_);configASSERT(viewMutex_);
    auto& model=runtime_.model;model.live=true;model.clockValid=false;model.bridgeAlive=false;
#ifdef PASSPORT_DEVICE
    native_=new(std::nothrow)NativeServices(audio_,[](void* context,const char* json){return static_cast<LiveServices*>(context)->enqueueNative(json);},this);configASSERT(native_);
#endif
    nvs_handle_t handle;
    if(nvs_open("suite_live",NVS_READONLY,&handle)==ESP_OK){
        FocusRecord record{};size_t size=sizeof(record);
        if(nvs_get_blob(handle,"focus_v2",&record,&size)==ESP_OK&&size==sizeof(record)&&record.version==2&&record.phase<3&&record.remaining<=86400){
            bool valid=true;for(auto seconds:record.periods)valid&=seconds>=1&&seconds<=86400;
            if(valid&&record.remaining<=record.periods[record.phase]){model.focusPeriods=record.periods;model.focusPhase=record.phase;model.focusRounds=record.rounds;model.focusRemaining=record.remaining;model.focusDuration=record.periods[record.phase];model.focusFinished=record.remaining==0;}
        }else{
            // Read only the old timer fields; retired todo contents are never loaded into the model.
            auto* legacy=new(std::nothrow)LegacyFocusRecord{};size_t length=sizeof(LegacyFocusRecord);
            if(legacy&&nvs_get_blob(handle,"focus_v1",legacy,&length)==ESP_OK&&length==sizeof(*legacy)&&legacy->version==1&&legacy->remaining<=86400){uint32_t duration=1500;nvs_get_u32(handle,"focus_duration",&duration);if(duration>=1&&duration<=86400){model.focusDuration=model.focusPeriods[0]=duration;model.focusRemaining=std::min<uint64_t>(legacy->remaining,duration);model.focusFinished=model.focusRemaining==0;}}
            delete legacy;
        }
        nvs_close(handle);
    }
    httpd_uri_t endpoint{}; endpoint.uri="/service"; endpoint.method=HTTP_POST; endpoint.handler=route; endpoint.user_ctx=this;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&endpoint));
    endpoint.uri="/app_state"; endpoint.method=HTTP_GET; endpoint.handler=statusRoute;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&endpoint));
    setenv("TZ",runtime_.settings().timezone,1); tzset();
}
esp_err_t LiveServices::snapshot(httpd_req_t* request) {
#ifdef PASSPORT_DEVICE
    if(!board::authorized(request))return httpd_resp_send_err(request,HTTPD_401_UNAUTHORIZED,"unauthorized");
#endif
    char output[2048];
    if(!snapshotJson(output,sizeof(output)))return httpd_resp_send_err(request,HTTPD_500_INTERNAL_SERVER_ERROR,"response size");
    httpd_resp_set_type(request,"application/json");httpd_resp_set_hdr(request,"Cache-Control","no-store");return httpd_resp_sendstr(request,output);
}
bool LiveServices::snapshotJson(char* output,size_t size){
    // Return the current local timer and service acknowledgement state.
    snprintf(output,size,"{\"focus_remaining\":%llu,\"focus_running\":%s,\"focus_finished\":%s,\"focus_phase\":%u,\"focus_rounds\":%u,\"focus_duration\":%u,\"clock_valid\":%s,\"control_id\":%u,\"control_result\":%u,\"clock_epoch\":%lld,\"last_applied_request\":%u,\"responses_applied\":%u,\"stale_rejected\":%u,\"notifications_received\":%u,\"live_phase\":%u,\"encoder_result\":%d,\"encoder_free\":%u}",
        focusRemaining_.load(),focusRunning_?"true":"false",focusFinished_?"true":"false",focusPhase_.load(),focusRounds_.load(),focusDuration_.load(),clockValid_?"true":"false",controlId_.load(),controlResult_.load(),static_cast<long long>(time(nullptr)),acceptedId_.load(),receivedCount_.load(),staleCount_.load(),noticeCount_.load(),phase_.load(),audio_.encoderResult.load(),audio_.encoderFree.load());
#ifdef PASSPORT_DEVICE
    if(native_&&runtime_.active()!=App::Settings){char native[1200];native_->snapshot(native,sizeof(native));const size_t used=strlen(output);snprintf(output+used-1,size-used+1,",\"native\":%s}",native);}
#endif
    View view;xSemaphoreTake(viewMutex_,portMAX_DELAY);view=view_;xSemaphoreGive(viewMutex_);
    JsonOutput json(output,size,strlen(output)-1);
    if(runtime_.active()==App::Settings){char values[210];snprintf(values,sizeof(values),",\"settings\":{\"page\":%u,\"selection\":%u,\"value\":%u,\"brightness\":%u,\"volume\":%u,\"bluetooth\":%s,\"idle_seconds\":%u}",unsigned(runtime_.model.settingsPage),runtime_.model.settingsSelection,unsigned(runtime_.model.settingsValue),runtime_.settings().brightness,runtime_.model.volume,runtime_.settings().bluetoothEnabled?"true":"false",unsigned(runtime_.settings().idleSeconds));json.append(values);}
    if(runtime_.active()==App::Daily){char values[48];snprintf(values,sizeof(values),",\"daily_back\":%u,\"daily_bytes\":%u",runtime_.model.dailyBack,unsigned(strlen(runtime_.model.page.daily.en)));json.append(values);}
    if(runtime_.active()==App::History){char values[24];snprintf(values,sizeof(values),",\"events\":%u",runtime_.model.eventCount);json.append(values);}
    {const auto& m=runtime_.model;const auto a=runtime_.active();char values[96]{};
     if(a==App::Quake)snprintf(values,sizeof(values),",\"quakes\":%u",m.quakeCount);
     if(a==App::Hacker)snprintf(values,sizeof(values),",\"stories\":%u",m.storyCount);
     // rows, the one picked, the one whose track is shown, track points, forecast points.
     if(a==App::Typhoon){const auto& t=m.page.typhoon;snprintf(values,sizeof(values),",\"typhoon\":[%u,%u,%u,%u,%u]",t.count,m.typhoonIndex,t.shown,t.points,t.ahead);}
     // items, the one picked, detail open, the one held in full, summary bytes.
     if(a==App::Hot)snprintf(values,sizeof(values),",\"hot\":[%u,%u,%u,%u,%u]",m.hotCount,m.hotIndex,unsigned(m.hotDetail),m.page.hot.detailFor,unsigned(strlen(m.page.hot.desc)));
     // alarms, the row picked, its clock open, delete asked, ringing, snooze epoch, then the picked one's hour, minute, repeat.
     if(a==App::Alarm){const auto& l=m.alarms;const AlarmPrefs picked=m.alarmIndex&&m.alarmIndex<=l.count?l.items[m.alarmIndex-1]:AlarmPrefs{};
         snprintf(values,sizeof(values),",\"alarm\":[%u,%u,%u,%u,%u,%lu,%u,%u,%u]",l.count,m.alarmIndex,unsigned(m.alarmEdit),unsigned(m.alarmArmed),unsigned(m.alarmRinging),static_cast<unsigned long>(m.alarmSnooze),picked.hour,picked.minute,picked.mode);}
     // notes, the row picked, a note open, its scroll, delete asked, listening (1) or recognising (2), stored mounted.
     if(a==App::Memo)snprintf(values,sizeof(values),",\"memo\":[%u,%u,%u,%u,%u,%u,%u]",m.memoCount,m.memoIndex,unsigned(m.memoDetail),m.memoScroll,unsigned(m.memoArmed),m.listening()?1u:m.voiceBusy()?2u:0u,unsigned(m.memoMounted));
     if(a==App::Noise)snprintf(values,sizeof(values),",\"noise\":[%u,%u,%u]",m.noiseKind,unsigned(m.noisePlaying),m.noiseTimer);
     if(a==App::Stock)snprintf(values,sizeof(values),",\"stock\":[%u,%u,%u,%u,%u,%u,%u]",m.stockCount,m.stockIndex,unsigned(m.stockDetail),unsigned(m.stockDays),m.chartFor,m.chartCount,unsigned(m.stockArmed));
     // rates shown, the one picked, delete asked; 倒数日's own events, the one on show, delete asked.
     if(a==App::Rate)snprintf(values,sizeof(values),",\"rate\":[%u,%u,%u]",m.rateCount,m.rateIndex,unsigned(m.rateArmed));
     if(a==App::Countdown){unsigned own=0;for(auto& e:m.countdown.events)own+=*e.name!=0;snprintf(values,sizeof(values),",\"countdown\":[%u,%u,%u]",own,m.countdownIndex,unsigned(m.countdownArmed));}
     if(a==App::Forecast)snprintf(values,sizeof(values),",\"forecast\":[%u,%u,%u,%u,%d,%u,%u]",m.page.forecast.days,m.page.forecast.hours,unsigned(m.forecastView),m.page.forecast.airHours,m.page.forecast.airHours?m.page.forecast.aqi[0]:-1,m.page.forecast.sunrise,m.page.forecast.warnings);
     if(a==App::Quiz)snprintf(values,sizeof(values),",\"quiz\":[%u,%u,%u,%u]",unsigned(m.quizSerial),m.quizCount,m.quizRight,m.quizAsked);
     // tab, networks, ping rows, then router and DNS (sent, lost, average ms), devices, the badge's row, wake sent.
     if(a==App::Wifi){const auto& p=m.page.wifi.pings;const bool pinged=m.pingCount==3;
         snprintf(values,sizeof(values),",\"wifi\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]",m.wifiTab,m.wifiApCount,m.pingCount,pinged?p[0].sent:0,pinged?p[0].lost:0,pinged?p[0].average:0,
                  pinged?p[1].sent:0,pinged?p[1].lost:0,pinged?p[1].average:0,m.lanCount,m.lanSelf,unsigned(m.wakeSentAt!=0));}
     // phase, level, calibration seconds left, packets/s, router RSSI, movements, sensitivity, history length, alarm.
     if(a==App::Motion)snprintf(values,sizeof(values),",\"motion\":[%u,%u,%u,%u,%d,%u,%u,%u,%u]",m.motionPhase,m.motionLevel,m.motionCalLeft,m.motionRate,m.motionRssi,m.motionEvents,m.tools.sense(),m.motionHistoryCount,m.tools.alarm());
     if(a==App::Sound)snprintf(values,sizeof(values),",\"sound\":[%u,%d,%u,%u,%u]",m.soundView,m.soundDb,m.soundHz,unsigned(m.soundRunning),m.soundBeat);
     json.append(values);
     // What the last spoken phrase or delete did, on the pages that take one.
     if(a==App::Rate||a==App::Stock||a==App::Countdown||a==App::Alarm||a==App::Settings){json.append(",\"notice\":");json.string(m.lastNotice);}
     if(a==App::Settings){json.append(",\"city\":");json.string(m.cityName);}
     // The start of the top note on show (the newest after a save), for tests.
     if(a==App::Memo){json.append(",\"memo_head\":");json.string(m.page.memo.rows[0].head);}
     // 状态牌: the label on show, its place, how many, lit, and the notice under it.
     if(a==App::Status){snprintf(values,sizeof(values),",\"status\":[%u,%u,%u]",m.status.index,m.statusCount(),unsigned(m.status.awake));json.append(values);
         json.append(",\"status_label\":");json.string(m.statusLabel(m.status.index).text);json.append(",\"notice\":");json.string(m.lastNotice);}
     // 待办提醒: count, selected row, first row shown, delete asked, ringing, next reminder; the selected todo's text.
     if(a==App::Todo){const auto& l=m.page.todo;snprintf(values,sizeof(values),",\"todo\":[%u,%u,%u,%u,%u,%lu]",l.count,m.todoIndex,m.todoTop,unsigned(m.todoArmed),unsigned(m.todoRinging),static_cast<unsigned long>(m.todoNextAt));json.append(values);
         json.append(",\"todo_item\":");json.string(m.todoIndex&&m.todoIndex<=l.count?l.items[m.todoIndex-1].text:"");
         json.append(",\"todo_next\":");json.string(m.todoNext);json.append(",\"notice\":");json.string(m.lastNotice);}
     // 语音记账: entries in the book, today's shown, selected row, delete asked, waiting for the computer; the notice.
     if(a==App::Ledger){const auto& l=m.page.ledger;uint8_t order[LedgerBook::kMax];snprintf(values,sizeof(values),",\"ledger\":[%u,%u,%u,%u,%u]",l.count,l.shown(ledgerToday(),order),m.ledgerIndex,unsigned(m.ledgerArmed),ledgerStatus().pending);json.append(values);
         json.append(",\"notice\":");json.string(m.lastNotice);}
     // 语音计算器: sums, the one on show; that sum and its result; the notice.
     if(a==App::Calc){snprintf(values,sizeof(values),",\"calc\":[%u,%u]",m.calcCount,m.calcIndex);json.append(values);
         const auto* row=m.calcCount?&m.page.calc[m.calcIndex]:nullptr;json.append(",\"calc_sum\":");json.string(row?row->shown:"");
         json.append(",\"calc_result\":");json.string(row?calc::format(row->value).c_str():"");json.append(",\"notice\":");json.string(m.lastNotice);}
     // 离座锁屏: row, distance, delay, keep, wake, then the computer's phase, signal, line, seconds left, calibrated level.
     if(a==App::Guard){const auto& g=m.guard;snprintf(values,sizeof(values),",\"guard\":[%u,%u,%u,%u,%u,%u,%d,%d,%u,%d]",m.guardRow,g.distance,g.delay,g.keep,g.wake,m.guardPhase,m.guardRssi,m.guardLine,m.guardLeft,m.guardBase);json.append(values);}
     // 中英翻译: toward English, engine, scroll; what was heard and its translation.
     if(a==App::Translate){const auto& t=m.page.translation;snprintf(values,sizeof(values),",\"translate\":[%u,%u,%u]",unsigned(t.toEnglish),t.engine,m.translateScroll);json.append(values);
         json.append(",\"translate_source\":");json.string(t.source);json.append(",\"translate_result\":");json.string(t.result);}
     // 随机转盘: modes, the one on show, its options, the pick (255 none), spinning, delete asked; its name and options.
     if(a==App::Wheel){const auto& w=m.page.wheel;const auto& mode=w.modes[w.current];
         snprintf(values,sizeof(values),",\"wheel\":[%u,%u,%u,%u,%u,%u]",w.count,w.current,mode.count,m.wheelPick,unsigned(m.wheelSpinning()),unsigned(m.wheelArmed));json.append(values);
         json.append(",\"wheel_mode\":");json.string(mode.name);json.append(",\"wheel_options\":[");
         for(unsigned i=0;i<mode.count;++i){if(i)json.append(",");json.string(mode.options[i]);}
         json.append("],\"notice\":");json.string(m.lastNotice);}}
    json.append(",\"state\":");json.string(view.state);json.append(",\"value\":");json.string(view.value);json.append(",\"connection\":");json.string(view.connection);
    json.append(view.computer?",\"requires_computer\":true":",\"requires_computer\":false");json.append(view.bridge?",\"bridge_alive\":true":",\"bridge_alive\":false");
    json.append(",\"lines\":[");for(unsigned i=0;i<4;++i){if(i)json.append(",");json.string(view.lines[i]);}json.append("]}");
    return json.valid();
}
esp_err_t LiveServices::receive(httpd_req_t* request) {
#ifdef PASSPORT_DEVICE
    if(!board::authorized(request))return httpd_resp_send_err(request,HTTPD_401_UNAUTHORIZED,"unauthorized");
#endif
    hostSeen();
    if(request->content_len<=0 || request->content_len>=static_cast<int>(sizeof(Message::json)))
        return httpd_resp_send_err(request,HTTPD_400_BAD_REQUEST,"invalid size");
    // Heartbeats must not compete with TLS/Opus for a full 2KiB message.
    char small[128]{};int used=0;
    if(request->content_len<static_cast<int>(sizeof(small))){
        while(used<request->content_len){const int count=httpd_req_recv(request,small+used,request->content_len-used);if(count<=0)return ESP_FAIL;used+=count;}
        auto* json=cJSON_Parse(small);const bool heartbeat=json&&!strcmp(string(json,"control"),"heartbeat");cJSON_Delete(json);
        if(heartbeat){httpd_resp_set_type(request,"application/json");return httpd_resp_sendstr(request,"{\"queued\":true}");}
    }
    // Other commands retain their bounded queue and main-task ownership.
    auto* message=new(std::nothrow) Message{};
    if(!message) return httpd_resp_send_err(request,HTTPD_500_INTERNAL_SERVER_ERROR,"memory");
    if(used)memcpy(message->json,small,used);
    while(used<request->content_len) {
        const int count=httpd_req_recv(request,message->json+used,request->content_len-used);
        if(count<=0) { delete message; return ESP_FAIL; } used+=count;
    }
    const bool queued=xQueueSend(messages_,message,0)==pdTRUE;
    mbedtls_platform_zeroize(message,sizeof(*message)); delete message;
    if(!queued) return httpd_resp_send_err(request,HTTPD_500_INTERNAL_SERVER_ERROR,"queue full");
    httpd_resp_set_type(request,"application/json");
    return httpd_resp_sendstr(request,"{\"queued\":true}");
}
void LiveServices::request(const char* operation) {
    // A refresh (by the timer, an error retry or entering) of 自选股 is for what is on screen.
    char stock[16];
    // A delete ("del:<row>") fetches the list too.
    if(runtime_.active()==App::Stock){if(!strcmp(operation,"refresh")||!strcmp(operation,"enter")){stockOperation(runtime_.model,stock);operation=stock;}
        if(!strncmp(operation,"del:",4))stockOperation(runtime_.model,stockAsked_);else copy(stockAsked_,operation);}
    // 离座锁屏 sends its settings with every request: "cfg:DLKW", and "cal:"/"lock:" for calibrate and lock now.
    if(runtime_.active()==App::Guard){const auto& g=runtime_.model.guard;const char* verb=!strcmp(operation,"cal")||!strcmp(operation,"lock")?operation:"cfg";
        const char digits[]{char('0'+g.distance%10),char('0'+g.delay%10),char('0'+g.keep%10),char('0'+g.wake%10),0};
        snprintf(stock,sizeof(stock),"%.4s:%s",verb,digits);operation=stock;}
    // 台风 too: its list with the picked typhoon's track.
    if(runtime_.active()==App::Typhoon&&(!strcmp(operation,"refresh")||!strcmp(operation,"enter"))){typhoonAsked_=runtime_.model.typhoonIndex;snprintf(stock,sizeof(stock),"show:%u",runtime_.model.typhoonIndex);operation=stock;}
    // 语音笔记's task (and 状态牌's) reads every Confirm itself, in order; a deferred one would start or stop listening late.
    if(pending_&&!keepsListening(runtime_.active())&&!speaksOnce(runtime_.active())&&!voiceJob_&&(!strcmp(operation,"toggle")||!strcmp(operation,"next")||!strcmp(operation,"prev"))) { copy(deferred_,operation);return; }
    pending_=++requestId_; pendingGeneration_=runtime_.generation();
    const bool voice=!strcmp(operation,"speak")||!strcmp(operation,"locate");
    if(voice)voiceJob_=pending_;
#ifdef PASSPORT_DEVICE
    // 状态牌 and 随机转盘 are local pages; only what is spoken goes to the cloud (语音笔记's task).
    if(needsInternet(runtime_.active())||voice||((speaksOnce(runtime_.active())||speaksToo(runtime_.active()))&&!strcmp(operation,"toggle"))){native_->network(networkReady_);if(!native_->request(runtime_.active(),pendingGeneration_,pending_,operation)){copy(runtime_.model.liveState,"操作太快，请重试");runtime_.model.livePhase=LivePhase::Error;}return;}
    if(!needsComputer(runtime_.active())){pending_=0;return;}
    board::event(operation,pending_,static_cast<unsigned>(runtime_.active()),pendingGeneration_);
#endif
    printf("SERVICE_REQUEST {\"request\":%lu,\"app\":%u,\"generation\":%lu,\"operation\":\"%s\"}\n",
        static_cast<unsigned long>(pending_),static_cast<unsigned>(runtime_.active()),
        static_cast<unsigned long>(pendingGeneration_),operation);
}
#ifdef PASSPORT_DEVICE
// 语音指令: 小智语音 or, with 说功能名跳转 on, another badge page by name; else (打开 Mac 程序 on) a program the Mac
// opens, the answer showing at home; else what was heard stays on the page.
App LiveServices::command(const char* heard,uint64_t now){
    auto& m=runtime_.model;
    const App page=spokenApp(heard);
    if(page==App::Chat||(page!=App::Count&&m.voiceJump))return page;
    if(m.macOpen)if(const auto name=spokenMac(heard);!name.empty()){openOnMac(name.c_str(),now);return App::Home;}
    copy(m.lastNotice,*heard?(std::string("没听懂：")+heard).c_str():"没有听清");
    return App::Count;
}
#endif
void LiveServices::openOnMac(const char* name,uint64_t now){
    auto& model=runtime_.model;
    macRequest_=++requestId_;char operation[48];snprintf(operation,sizeof(operation),"open:%s",name);
    snprintf(model.macNote,sizeof(model.macNote),"Mac 正在打开「%s」",name);model.macAt=now;
#ifdef PASSPORT_DEVICE
    board::event(operation,macRequest_,static_cast<unsigned>(App::Home),runtime_.generation());
#endif
    printf("MAC_OPEN {\"request\":%lu,\"operation\":\"%s\"}\n",static_cast<unsigned long>(macRequest_),operation);
}
void LiveServices::persistFocus() {
    const auto& model=runtime_.model;
    auto* record=new(std::nothrow) FocusRecord{}; if(!record) return;
    record->remaining=model.focusRemaining;record->phase=model.focusPhase;record->rounds=model.focusRounds;record->periods=model.focusPeriods;
    nvs_handle_t handle;
    if(nvs_open("suite_live",NVS_READWRITE,&handle)==ESP_OK) {
        if(nvs_set_blob(handle,"focus_v2",record,sizeof(*record))==ESP_OK &&
           nvs_set_u32(handle,"focus_duration",model.focusDuration)==ESP_OK) nvs_commit(handle);
        nvs_close(handle);
    }
    delete record;
}
int LiveServices::control(void* value) {
    auto* object=static_cast<cJSON*>(value); const char* op=string(object,"control"); auto& model=runtime_.model;
#ifdef PASSPORT_DEVICE
    if(!strcmp(op,"ip_location")){const auto* allowed=field(object,"allowed");if(!cJSON_IsBool(allowed)||!native_->allowLocation(cJSON_IsTrue(allowed)))return 1;if(runtime_.active()==App::Home)request("refresh");return 0;}
    if(!strcmp(op,"cloud_probe")){
        if(runtime_.active()!=App::Home||!networkReady_||audio_.active()||!native_->idle())return 1;
        native_->network(true);return native_->request(App::Chat,runtime_.generation(),0,"probe")?0:1;
    }
#endif
    if(!strcmp(op,"idle_minutes")) {
        const auto minutes=number(object,"minutes");
        return minutes>=0&&minutes<=1440&&minutes==static_cast<uint32_t>(minutes)&&runtime_.setIdleMinutes(static_cast<uint32_t>(minutes))?0:1;
    }
    if(!strcmp(op,"focus_seconds")) {
        int seconds=number(object,"seconds"); if(seconds<1||seconds>86400||model.focusRunning) return 1;
        model.focusRemaining=model.focusDuration=model.focusPeriods[model.focusPhase]=seconds; model.focusFinished=false; persistFocus(); return 0;
    }
    return 1;
}
void LiveServices::action(Key key) {
    const auto active=runtime_.active();
    if(active==App::Focus) { persistFocus(); return; }
    if(active==App::Words) {
        const auto* card=wordsCurrent(runtime_.model.words,runtime_.model.wordSession);
        // "say" looks the word up again first if its meaning failed to load.
        if(key==Key::Confirm&&card&&runtime_.model.wordSession.revealed){char operation[16];snprintf(operation,sizeof(operation),"say:%u",card->word);request(operation);}
        return;
    }
    // 状态牌: Confirm starts speaking a label and ends it. A key after an error puts the sign back (unless the cloud
    // task still runs: it may be showing an activation code), and Confirm then tries again.
    // 离座锁屏: Confirm changed a setting (Runtime::key); the computer gets it at once.
    if(active==App::Guard){if(key==Key::Confirm)request("cfg");return;}
    // 随机转盘 the same, except that Confirm spins (Runtime::key): speaking starts with a long Up (speak()).
    if(speaksOnce(active)){
        auto& m=runtime_.model;
        if(m.livePhase==LivePhase::Error){if(!nativeIdle())return;m.livePhase=LivePhase::Ready;copy(m.liveState,"本地功能");}
        // 语音计算器 speaks on any Confirm; 待办提醒, 语音记账 and 闹钟 speak from their 录音 row (Confirm on a todo ticks it off, on an entry changes its
        // category, on an alarm opens it); a Confirm that stopped a ring or a snooze does nothing more.
        if(key==Key::Confirm&&m.livePhase==LivePhase::Ready&&!m.confirmTaken&&(active==App::Status||active==App::Calc||(active==App::Todo&&!m.todoIndex&&!m.todoRinging)||(active==App::Ledger&&!m.ledgerIndex)
           ||(active==App::Alarm&&!m.alarmIndex&&!m.alarmRinging&&!m.alarmEdit)||m.voiceBusy()))request("toggle");
        return;
    }
    // 汇率 and 自选股: Confirm ends a phrase spoken after a long Up. 所在城市: Confirm speaks the city or looks it up by IP
    // (not the Confirm that opened the page).
    if(key==Key::Confirm&&speaksToo(active)&&runtime_.model.voiceBusy()){request("toggle");return;}
    if(key==Key::Confirm&&active==App::Settings&&runtime_.model.settingsPage==SettingsPage::City&&!runtime_.model.confirmTaken){
        auto& m=runtime_.model;
#ifdef PASSPORT_DEVICE
        if(!nativeIdle()){copy(m.lastNotice,"上一次还没完成，请稍候");return;}
#endif
        copy(m.lastNotice,m.cityRow?"正在按 IP 查询…":"正在连接小智…");request(m.cityRow?"locate":"speak");return;
    }
    if(key==Key::Confirm&&runtime_.model.livePhase==LivePhase::Error) { request("enter");runtime_.model.livePhase=LivePhase::Loading;loadingAt_=requestedAt_=esp_timer_get_time()/1000000;return; }
    if(key==Key::Confirm&&active==App::Daily) { request("say"); return; }
    // Confirm on the list opens the player (tick asks for the program); in the player it plays or pauses.
    if(key==Key::Confirm&&active==App::Podcast) { if(runtime_.model.podcastPlayer&&podcastAsked_>=0)request("toggle"); return; }
    // On the 预警 tab Confirm steps through the warnings' texts instead (Runtime::key).
    const bool warningsShown=active==App::Forecast&&runtime_.model.forecastView==3&&runtime_.model.page.forecast.warnings&&runtime_.model.page.forecast.warnings!=0xff;
    if(key==Key::Confirm&&!warningsShown&&(active==App::Home||active==App::Monitor||active==App::Codex||active==App::News||active==App::Rate||active==App::History||active==App::Quake||active==App::Hacker||active==App::Forecast||active==App::Typhoon))request("refresh");
    if(key==Key::Confirm && (active==App::Chat||active==App::Dictation||active==App::Radio)) request("toggle");
    // 语音笔记: Confirm on the 录音 row starts listening, and while listening ends it (Runtime::key leaves both alone).
    const auto& m=runtime_.model;
    if(key==Key::Confirm&&active==App::Memo&&m.livePhase!=LivePhase::Error&&(m.voiceBusy()||(!m.memoDetail&&!m.memoIndex)))request("toggle");
    // 中英翻译: Confirm starts and ends a phrase, which the device's task then translates.
    if(key==Key::Confirm&&active==App::Translate&&m.livePhase!=LivePhase::Error)request("toggle");
    // 语音指令: Confirm says another (or ends the one being said).
    if(key==Key::Confirm&&active==App::Voice&&m.livePhase!=LivePhase::Error){runtime_.model.lastNotice[0]=0;request("toggle");}
}
// 中英翻译: a long Up reads the translation aloud (not while listening).
void LiveServices::say() {
    const auto& m=runtime_.model;
    if(runtime_.active()==App::Translate&&m.livePhase!=LivePhase::Error&&!m.voiceBusy()&&*m.page.translation.result)request("say");
}
void LiveServices::speak() {
    auto& m=runtime_.model;const auto active=runtime_.active();
    if(active==App::Wheel||active==App::Countdown){
        if(m.livePhase==LivePhase::Error){if(!nativeIdle())return;m.livePhase=LivePhase::Ready;copy(m.liveState,"本地功能");}
        if(m.livePhase==LivePhase::Ready&&!m.voiceBusy()&&!m.wheelSpinning()){m.wheelArmed=m.countdownArmed=false;copy(m.lastNotice,active==App::Countdown?"正在连接小智…":"");request("toggle");}
        return;
    }
    // 汇率 and 自选股 (its list) fetch online: the phrase waits for a fetch under way, then the page fetches again.
    if((active==App::Rate||(active==App::Stock&&!m.stockDetail))&&!m.voiceBusy()&&!voiceJob_){m.rateArmed=m.stockArmed=false;copy(m.lastNotice,"正在连接小智…");request("speak");}
}
void LiveServices::dropStock() {
    auto& m=runtime_.model;
    if(runtime_.active()!=App::Stock||m.stockDetail||!m.stockCount||m.voiceBusy()||voiceJob_){m.stockArmed=false;return;}
    if(m.stockCount==1){copy(m.lastNotice,"至少保留 1 只");return;}
    if(!m.stockArmed){m.stockArmed=true;m.lastNotice[0]=0;return;}
    m.stockArmed=false;char operation[16];snprintf(operation,sizeof(operation),"del:%u",m.stockIndex);
    copy(m.lastNotice,"正在删除…");request(operation);
}
// Pages that need neither the internet nor the computer start Ready, so no stale loading screen covers them.
bool local(App app){return !needsInternet(app)&&!needsComputer(app);}
void LiveServices::renderLocal(uint64_t) {
    auto& model=runtime_.model; const auto active=runtime_.active();
    if(time(nullptr)>1700000000)model.clockValid=true;
    if(active==App::Focus){
        snprintf(model.liveValue,sizeof(model.liveValue),"%02llu:%02llu",model.focusRemaining/60,model.focusRemaining%60);
        copy(model.liveState,model.focusFinished?"本轮完成":model.focusRunning?"计时中":"本地计时");
        copy(model.liveLines[0],model.focusRunning?"确认暂停":"确认开始或继续");
        copy(model.liveLines[1],"上下切换专注、短休、长休");
        copy(model.liveLines[2],"离开页面自动暂停");
    }
    if(active==App::Home && model.clockValid) {
        const auto now=time(nullptr); tm date{}; localtime_r(&now,&date);
        strftime(model.liveValue,sizeof(model.liveValue),"%H:%M:%S",&date);
        strftime(model.liveLines[0],sizeof(model.liveLines[0]),"%Y-%m-%d",&date);
    }
}
bool LiveServices::tick(uint64_t now) {
    const auto active=runtime_.active(); auto& model=runtime_.model;
    bool dirty=false;
    const bool entered=active!=observed_||runtime_.generation()!=observedGeneration_;
    const auto previousPhase=model.livePhase;char previousConnection[64];copy(previousConnection,model.connectionState);
    // 打开 Mac 程序: give the computer 20 s (Bluetooth may have to connect first); the answer shows for 5 s.
    if(macRequest_&&now>=model.macAt+20){macRequest_=0;copy(model.macNote,"Mac 没有回应：请连 USB 或蓝牙");model.macAt=now;dirty=true;}
    else if(!macRequest_&&*model.macNote&&now>=model.macAt+5){model.macNote[0]=0;dirty=true;}
    if(entered) {
        dirty=true;
        if(observed_==App::Focus) persistFocus();
        observed_=active; observedGeneration_=runtime_.generation();lastSpeechTurn_=0; requestedAt_=now; pending_=0; accepted_=0;deferred_[0]=0;voiceJob_=0;
        bool audioReady=true;
#ifdef PASSPORT_DEVICE
        native_->network(networkReady_);native_->cancel(runtime_.generation());deferredAudio_=false;
        if(!needsInternet(active)){if(native_->idle()&&uiPreparedFor(active))audioReady=audio_.start(active,runtime_.generation());else deferredAudio_=true;}
#else
        audioReady=audio_.start(active,runtime_.generation());
#endif
        copy(model.liveValue,"--"); copy(model.liveState,"连接中"); for(auto& line:model.liveLines) line[0]=0;
        model.mediaCount=0;model.mediaPlaying=false;model.podcastCount=0;newsAsked_=0;newsItemAsked_=-1;dailyAsked_=wordAsked_=podcastAsked_=-1;quizAsked_=false;stockAsked_[0]=0;warnAsked_=0;typhoonAsked_=hotAsked_=-1;
#ifdef PASSPORT_DEVICE
        if(active==App::Settings)native_->cityName(model.cityName,sizeof(model.cityName));
#endif
        model.livePhase=local(active)?LivePhase::Ready:LivePhase::Loading;
        if(local(active)&&active!=App::Focus)copy(model.liveState,"本地功能");
        loadingAt_=now;
        request(audioReady ? "enter" : "audio_unavailable"); renderLocal(now);
    }
    if(runtime_.ledgerSynced()&&active==App::Ledger)dirty=true;
#ifdef PASSPORT_DEVICE
    native_->poll();
    // 语音笔记: the text the cloud heard, saved once; the device's task waits until it is.
    if(active==App::Memo||speaksOnce(active)||active==App::Rate||active==App::Voice)if(const char* heard=native_->memoHeard()){
        App go=App::Count;
        if(active==App::Voice)go=command(heard,now);
        else if(active==App::Status)runtime_.addStatus(heard);else if(active==App::Wheel)runtime_.addWheel(heard);else if(active==App::Todo)runtime_.addTodo(heard);else if(active==App::Ledger)runtime_.addLedger(heard);else if(active==App::Calc)runtime_.addCalc(heard);
        else if(active==App::Alarm)runtime_.addAlarm(heard);else if(active==App::Countdown)runtime_.addCountdown(heard);
        else if(active==App::Rate){if(runtime_.addRate(heard)){voiceJob_=0;request("refresh");}}
        else if(*heard)runtime_.addMemo(heard,now);else copy(model.lastNotice,"没有听清，请再说一次");
        native_->memoTaken();dirty=true;
        if(go!=App::Count){runtime_.switchTo(go);return true;}
    }
    if(voiceJob_&&native_->idle()&&!native_->busy()){voiceJob_=0;dirty=true;}
    if(active==App::Translate)if(const auto* t=native_->translated()){
        auto& p=model.page.translation;copy(p.source,t->source);copy(p.result,t->result);p.toEnglish=t->toEnglish;p.engine=t->engine;p.at=now;
        model.translateScroll=0;native_->translationTaken();dirty=true;
    }
    if(deferredAudio_&&native_->idle()&&uiPreparedFor(active)){deferredAudio_=false;if(!audio_.start(active,runtime_.generation())){copy(model.liveState,"音频启动失败");model.livePhase=LivePhase::Error;}}
#endif
    // One queued reply per turn keeps UI and buttons responsive.
    static Message message{};
    bool haveMessage=xQueueReceive(messages_,&message,0)==pdTRUE;
#ifdef PASSPORT_DEVICE
    if(!haveMessage)haveMessage=xQueueReceive(nativeMessages_,&message,0)==pdTRUE;
#endif
    if(haveMessage) {
        dirty=true;
        auto* json=cJSON_Parse(message.json);
        if(json&&!strcmp(string(json,"control"),"heartbeat")) {
            hostSeen();
        } else if(json && *string(json,"control")) {
            const auto id=static_cast<unsigned>(number(json,"id",0)); const auto result=control(json);
            controlId_=id; controlResult_=result;
            printf("CONTROL_DONE {\"id\":%u,\"result\":%d}\n",id,result);
        } else if(json&&cJSON_IsString(field(json,"mac"))) {
            // 打开 Mac 程序: what the computer did, whichever page is on screen by now.
            if(macRequest_&&number(json,"request")==macRequest_){copy(model.macNote,string(json,"mac"));model.macAt=now;macRequest_=0;}
        } else if(json &&
#ifdef PASSPORT_DEVICE
                  (!needsInternet(active)||message.native)&&
#endif
                  number(json,"app")==static_cast<int>(active) && number(json,"generation")==runtime_.generation()
                  && (number(json,"request")==pending_ || (!pending_ && cJSON_IsTrue(field(json,"stream")) && number(json,"request")==accepted_))) {
            const bool wasListening=model.listening(),wasBusy=model.voiceBusy();
            copy(model.liveValue,string(json,"value")); copy(model.liveState,string(json,"state"));
            // Actual user speech is input; UI updates and assistant heartbeats are not.
            const auto turn=number(json,"speech_turn",0);
            const bool spoke=message.native&&active==App::Chat&&turn>lastSpeechTurn_&&turn<=UINT32_MAX;
            if(spoke){lastSpeechTurn_=static_cast<unsigned>(turn);runtime_.wake(now);}
            model.livePhase=!strcmp(string(json,"phase"),"loading")?LivePhase::Loading:!strcmp(string(json,"phase"),"error")?LivePhase::Error:LivePhase::Ready;
            if(message.native)copy(model.connectionState,string(json,"state"));
            // 自选股 and 所在城市: what the device's lookup did (a security added and picked, the city set).
            if(message.native&&*string(json,"notice"))copy(model.lastNotice,string(json,"notice"));
            if(message.native&&*string(json,"city"))copy(model.cityName,string(json,"city"));
            if(message.native&&active==App::Stock&&cJSON_IsNumber(field(json,"select")))model.stockIndex=std::clamp(number(json,"select",0),0.0,11.0);
            if(model.livePhase==LivePhase::Ready)loadingAt_=0;
            if(active==App::Codex && *string(json,"notice")) { runtime_.codexNotice(string(json,"notice")); ++noticeCount_; }
            const auto* lines=field(json,"lines");
            for(unsigned i=0;i<4;++i) { const auto* line=cJSON_GetArrayItem(lines,i); copy(model.liveLines[i],cJSON_IsString(line) ? line->valuestring : ""); }
            if(message.native&&active==App::Chat&&turn>=0&&turn<=UINT32_MAX)model.chatRecord(static_cast<unsigned>(turn),model.liveLines[0],model.liveLines[1]);
            if(active==App::Guard){
                const auto* g=field(json,"guard");const auto value=[&](const char* key,int low,int high){return static_cast<int>(std::clamp(number(g,key,0),double(low),double(high)));};
                if(cJSON_IsObject(g)){model.guardPhase=value("phase",0,4);model.guardRssi=value("rssi",-127,0);model.guardLine=value("line",-127,0);model.guardBase=value("base",-127,0);model.guardLeft=value("left",0,60);}
            }
            if(active==App::Radio){
                const auto* m=field(json,"media");const auto value=[&](const char* key,uint8_t limit){const auto v=number(m,key,0);return static_cast<uint8_t>(v>=0&&v<=limit?v:0);};
                model.mediaCount=value("count",255);model.mediaIndex=value("index",model.mediaCount?model.mediaCount-1:0);
            }
            // 语音笔记 and 语音输入 (whose computer reports it): the recording screen times from the start of listening.
            if(((message.native&&(keepsListening(active)||speaksOnce(active)||speaksToo(active)||active==App::Voice))||active==App::Dictation)&&model.listening()&&!wasListening)model.listenAt=now;
            if(active==App::Dictation&&wasBusy&&!model.voiceBusy())model.dictationAt=now;
            if(active==App::Radio||active==App::Podcast)model.mediaPlaying=cJSON_IsTrue(field(json,"playing"));
            if(active==App::Podcast){
                auto& m=model;const auto* p=field(json,"podcast");
                m.podcastPos=std::max(0.0,number(p,"pos",0));m.podcastDur=std::max(0.0,number(p,"dur",0));
                if(cJSON_IsArray(field(p,"items"))){
                    m.podcastCount=0;const cJSON* item;
                    cJSON_ArrayForEach(item,field(p,"items")){if(m.podcastCount==m.page.podcasts.size())break;auto& r=m.page.podcasts[m.podcastCount++];copy(r.name,string(item,"n"));copy(r.note,string(item,"s"));}
                    if(m.podcastIndex>=m.podcastCount)m.podcastIndex=0;
                }
            }
            if(active==App::Home){
                if(message.native&&model.livePhase==LivePhase::Ready)requestedAt_=now;
                const auto* w=field(json,"weather");
                const auto decimal=[&](const char* key){const auto* v=field(w,key);return cJSON_IsNumber(v)?v->valuedouble:NAN;};
                if(cJSON_IsObject(w)&&model.weatherReading.update(string(w,"city"),string(w,"condition"),decimal("temperature"),decimal("minimum"),decimal("maximum"),decimal("humidity"),decimal("code"),decimal("aqi"))){
                    auto& r=model.weatherReading;copy(r.warning,string(w,"warn"));r.warningLevel=*r.warning?int8_t(std::clamp(number(w,"warn_level",-1),-1.0,3.0)):-1;}
            }
            const auto epoch=number(json,"epoch");
            if(epoch>1700000000 && epoch<4102444800) { timeval stamp{static_cast<time_t>(epoch),0}; settimeofday(&stamp,nullptr); model.clockValid=true; }
            if(!message.native)model.bridgeAlive=true;
            ++received_; accepted_=number(json,"request"); pending_=0;
            const auto* metrics=field(json,"metrics");
            if(active==App::Monitor&&cJSON_IsObject(metrics)) {
                model.cpu=std::clamp(int(number(metrics,"cpu",0)),0,100);model.ram=std::clamp(int(number(metrics,"ram",0)),0,100);model.metricsKnown=true;
                model.memoryUsed=std::max(0.0,number(metrics,"memory_used",0));model.memoryTotal=std::max(0.0,number(metrics,"memory_total",0));
                model.netUp=std::max(0.0,number(metrics,"net_up",0));model.netDown=std::max(0.0,number(metrics,"net_down",0));
                model.disk=std::clamp(int(number(metrics,"disk",0)),0,100);model.diskFree=std::max(0.0,number(metrics,"disk_free",0));
                model.battery=std::clamp(int(number(metrics,"battery",-1)),-1,100);model.charging=cJSON_IsTrue(field(metrics,"charging"));
                model.load=std::max(0.0,number(metrics,"load",0));model.cores=std::clamp(int(number(metrics,"cores",0)),0,255);model.uptime=std::max(0.0,number(metrics,"uptime",0));
                if(model.historyCount==24){std::move(model.cpuHistory.begin()+1,model.cpuHistory.end(),model.cpuHistory.begin());--model.historyCount;}
                model.cpuHistory[model.historyCount++]=model.cpu;
            }
            if(active==App::News&&cJSON_IsArray(field(json,"news"))){
                auto& m=model;m.newsCount=0;const cJSON* item;
                cJSON_ArrayForEach(item,field(json,"news")){
                    if(m.newsCount==m.page.news.items.size())break;
                    auto& n=m.page.news.items[m.newsCount++];
                    copy(n.time,string(item,"t"));copy(n.title,string(item,"h"));
                }
                // Item i's text: another text starts from its top.
                newsItemAsked_=-1;auto& t=m.page.news;const double i=number(json,"i",-1);t.textFor=i>=0&&i<m.newsCount?uint8_t(i):255;
                if(strcmp(t.text,string(json,"d"))){copy(t.text,string(json,"d"));m.newsScroll=0;}
                // A new page starts at its first headline (its last when walking back); a refresh keeps the place.
                const double raw=number(json,"page",1);const uint8_t page=raw>=1&&raw<=20?raw:1;
                if(page!=m.newsPage)m.newsIndex=m.newsFromEnd&&m.newsCount?m.newsCount-1:0;else if(m.newsIndex>=m.newsCount)m.newsIndex=0;
                m.newsPage=m.newsWant=page;m.newsFromEnd=false;
            }
            // Only a final answer frees the page request: re-asking on the "loading" update would make the real one stale.
            if(active==App::News&&model.livePhase!=LivePhase::Loading){newsAsked_=0;if(model.livePhase==LivePhase::Error)model.newsWant=model.newsPage;}
            if(active==App::Rate&&cJSON_IsArray(field(json,"rates"))){
                auto& m=model;m.rateCount=0;copy(m.rateBase,string(json,"base"));m.rateUpdated=std::max(0.0,number(json,"updated",0));const cJSON* item;
                cJSON_ArrayForEach(item,field(json,"rates")){
                    if(m.rateCount==m.rates.size())break;
                    // number() only passes whole numbers; rates are fractions.
                    const auto* v=field(item,"v");if(!cJSON_IsNumber(v)||!(v->valuedouble>0))continue;
                    auto& r=m.rates[m.rateCount++];copy(r.code,string(item,"c"));r.value=v->valuedouble;
                }
                if(m.rateIndex>=m.rateCount)m.rateIndex=0;
            }
            if(active==App::Forecast&&cJSON_IsObject(field(json,"forecast"))){
                // Days: [low, high, code, rain]; hours: [temperature, code, rain], from hour h0.
                auto& f=model.page.forecast;const auto* o=field(json,"forecast");f.days=f.hours=0;copy(f.city,string(o,"city"));f.hour0=std::clamp(number(o,"h0",0),0.0,23.0);
                const auto value=[](const cJSON* a,int i,double low,double high){const auto* v=cJSON_GetArrayItem(a,i);return cJSON_IsNumber(v)?std::clamp(v->valuedouble,low,high):low;};
                const cJSON* item;
                cJSON_ArrayForEach(item,field(o,"d"))if(f.days<std::size(f.day))f.day[f.days++]={int8_t(value(item,0,-99,99)),int8_t(value(item,1,-99,99)),uint8_t(value(item,2,0,99)),uint8_t(value(item,3,0,255))};
                cJSON_ArrayForEach(item,field(o,"h"))if(f.hours<std::size(f.hour))f.hour[f.hours++]={int8_t(value(item,0,-99,99)),uint8_t(value(item,1,0,99)),uint8_t(value(item,2,0,255))};
                // Air: China AQI per hour from h0; x: [primary pollutant, PM2.5, PM10, sunrise, sunset, UV x10].
                f.airHours=0;cJSON_ArrayForEach(item,field(o,"air"))if(f.airHours<std::size(f.aqi))f.aqi[f.airHours++]=int16_t(cJSON_IsNumber(item)?std::clamp(item->valuedouble,-1.0,999.0):-1);
                const auto* x=field(o,"x");f.primary=value(x,0,0,5);f.pm25=value(x,1,0,65535);f.pm10=value(x,2,0,65535);f.sunrise=value(x,3,0,65535);f.sunset=value(x,4,0,65535);f.uv=value(x,5,0,255);
                if(!cJSON_IsArray(x))f.pm25=f.pm10=f.sunrise=f.sunset=0xffff,f.uv=255;
                // Warnings: [level, places, kind, where, time]; none read leaves 0xff.
                const auto* warn=field(o,"warn");f.warnings=cJSON_IsArray(warn)?0:0xff;
                const auto text=[](const cJSON* a,int i){const char* v=cJSON_GetStringValue(cJSON_GetArrayItem(a,i));return v?v:"";};
                cJSON_ArrayForEach(item,warn)if(f.warnings<std::size(f.warning)){auto& w=f.warning[f.warnings++];w.level=value(item,0,0,3);w.places=value(item,1,1,255);
                    copy(w.type,text(item,2));copy(w.where,text(item,3));copy(w.time,text(item,4));}
                const auto* detail=field(o,"detail");f.detailFor=cJSON_IsArray(detail)?value(detail,0,0,255):0xff;copy(f.detail,text(detail,1));
            }
            if(active==App::Stock&&cJSON_IsObject(field(json,"stock"))){
                // Rows: [name, code, market tag, decimals, price, change, change %, open, high, low, previous close,
                // turnover], null when not traded. With "i" only that row came, with its chart in "c".
                auto& m=model;const auto* o=field(json,"stock");
                const auto real=[](const cJSON* v){return cJSON_IsNumber(v)?float(v->valuedouble):NAN;};
                const auto put=[&](Model::StockRow& row,const cJSON* a){
                    const auto text=[&](int i){const auto* v=cJSON_GetArrayItem(a,i);return cJSON_IsString(v)?v->valuestring:"";};
                    copy(row.name,text(0));copy(row.code,text(1));copy(row.tag,text(2));
                    const float decimals=real(cJSON_GetArrayItem(a,3));row.decimals=decimals>=0&&decimals<=4?uint8_t(decimals):2;
                    float* values[]{&row.price,&row.change,&row.pct,&row.open,&row.high,&row.low,&row.prev,&row.amount};
                    for(unsigned i=0;i<std::size(values);++i)*values[i]=real(cJSON_GetArrayItem(a,4+i));
                };
                m.stockUpdated=std::max(0.0,number(o,"t",0));
                const double one=number(o,"i",-1);const auto* rows=field(o,"r");const cJSON* row;
                if(one>=0){if(one<m.stockCount)put(m.page.stocks.rows[unsigned(one)],cJSON_GetArrayItem(rows,0));}
                else{
                    m.stockCount=0;
                    cJSON_ArrayForEach(row,rows){if(m.stockCount<m.page.stocks.rows.size())put(m.page.stocks.rows[m.stockCount++],row);}
                    if(m.stockIndex>=m.stockCount)m.stockIndex=0;
                    if(!m.stockCount)m.stockDetail=false;
                }
                if(const auto* c=field(o,"c");cJSON_IsObject(c)){
                    m.chartFor=std::clamp(number(c,"i",255),0.0,255.0);m.chartDays=cJSON_IsTrue(field(c,"k"));m.chartTotal=std::clamp(number(c,"n",0),0.0,1440.0);
                    m.chartLow=real(field(c,"lo"));m.chartHigh=real(field(c,"hi"));copy(m.chartFrom,string(c,"from"));copy(m.chartMid,string(c,"mid"));copy(m.chartTo,string(c,"to"));
                    m.chartCount=0;const cJSON* point;
                    cJSON_ArrayForEach(point,field(c,"p"))if(m.chartCount<m.page.stocks.chart.size())m.page.stocks.chart[m.chartCount++]=int16_t(std::clamp(real(point),0.f,999.f));
                    if(!std::isfinite(m.chartLow)||!std::isfinite(m.chartHigh))m.chartCount=0;
                }
            }
            if(active==App::History&&cJSON_IsArray(field(json,"events"))){
                auto& m=model;m.eventCount=0;const cJSON* item;
                cJSON_ArrayForEach(item,field(json,"events")){
                    if(m.eventCount==m.page.events.size())break;
                    auto& e=m.page.events[m.eventCount++];e.year=std::clamp(number(item,"y",0),-9999.0,9999.0);e.type=std::clamp(number(item,"k",0),0.0,2.0);copy(e.title,string(item,"t"));
                }
                if(m.eventIndex>=m.eventCount)m.eventIndex=0;
            }
            if(active==App::Daily&&cJSON_IsObject(field(json,"daily"))){
                const auto* d=field(json,"daily");const double back=number(d,"back",0);
                copy(model.page.daily.en,string(d,"en"));copy(model.page.daily.cn,string(d,"cn"));copy(model.page.daily.date,string(d,"date"));model.dailyBack=model.dailyWant=back>=0&&back<=365?back:0;
            }
            if(active==App::Daily&&model.livePhase!=LivePhase::Loading){dailyAsked_=-1;if(model.livePhase==LivePhase::Error)model.dailyWant=model.dailyBack;}
            if(active==App::Quake&&cJSON_IsArray(field(json,"quakes"))){
                auto& m=model;m.quakeCount=0;const cJSON* item;
                cJSON_ArrayForEach(item,field(json,"quakes")){
                    if(m.quakeCount==m.page.quakes.size())break;
                    auto& q=m.page.quakes[m.quakeCount++];q.at=std::max(0.0,number(item,"at",0));q.mag=std::clamp(number(item,"m",0),0.0,99.0);q.depth=std::clamp(number(item,"d",0),0.0,999.0);copy(q.place,string(item,"p"));
                }
                if(m.quakeIndex>=m.quakeCount)m.quakeIndex=0;
            }
            if(active==App::Hacker&&cJSON_IsArray(field(json,"stories"))){
                auto& m=model;m.storyCount=0;const cJSON* item;
                cJSON_ArrayForEach(item,field(json,"stories")){
                    if(m.storyCount==m.page.stories.size())break;
                    auto& s=m.page.stories[m.storyCount++];copy(s.title,string(item,"t"));copy(s.site,string(item,"s"));
                    s.points=std::clamp(number(item,"p",0),0.0,65535.0);s.comments=std::clamp(number(item,"c",0),0.0,65535.0);s.at=std::max(0.0,number(item,"at",0));
                }
                if(m.storyIndex>=m.storyCount)m.storyIndex=0;
            }
            if(active==App::Typhoon&&cJSON_IsObject(field(json,"typhoon"))){
                // Rows: [number, name, active]; the shown one's track (lon, lat, grade) and forecast (lon, lat, grade,
                // hours) in tenths of a degree; x: [epoch, hPa, lowest hPa, m/s, km/h, peak m/s, peak grade]; c: [city, lon, lat].
                auto& t=model.page.typhoon;const auto* o=field(json,"typhoon");const cJSON* item;
                const auto at=[](const cJSON* a,int i,double low,double high){const auto* v=cJSON_GetArrayItem(a,i);return cJSON_IsNumber(v)?std::clamp(v->valuedouble,low,high):low;};
                const auto text=[](const cJSON* a,int i){const char* v=cJSON_GetStringValue(cJSON_GetArrayItem(a,i));return v?v:"";};
                t.count=0;cJSON_ArrayForEach(item,field(o,"l"))if(t.count<TyphoonView::kRows){auto& r=t.rows[t.count++];copy(r.number,text(item,0));copy(r.name,text(item,1));r.active=at(item,2,0,1);}
                t.shown=std::clamp(number(o,"i",255),0.0,255.0);
                const auto* track=field(o,"t");t.points=std::min<unsigned>(cJSON_GetArraySize(track)/3,TyphoonView::kTrack);
                for(unsigned i=0;i<t.points;++i)t.track[i]={int16_t(at(track,3*i,-3600,3600)),int16_t(at(track,3*i+1,-900,900)),uint8_t(at(track,3*i+2,0,6))};
                const auto* ahead=field(o,"f");t.ahead=std::min<unsigned>(cJSON_GetArraySize(ahead)/4,TyphoonView::kAhead);
                for(unsigned i=0;i<t.ahead;++i)t.forecast[i]={int16_t(at(ahead,4*i,-3600,3600)),int16_t(at(ahead,4*i+1,-900,900)),uint8_t(at(ahead,4*i+2,0,6)),uint8_t(at(ahead,4*i+3,0,255))};
                const auto* x=field(o,"x");t.at=at(x,0,0,4294967295.0);t.pressure=at(x,1,0,1100);t.lowest=at(x,2,0,1100);t.wind=at(x,3,0,99);t.speed=at(x,4,0,99);t.peakWind=at(x,5,0,99);t.peakGrade=at(x,6,0,6);
                copy(t.heading,string(o,"d"));
                const auto* c=field(o,"c");copy(t.city,text(c,0));t.cityLon=at(c,1,-3600,3600);t.cityLat=cJSON_IsArray(c)?int16_t(at(c,2,-900,900)):INT16_MIN;
                if(t.shown>=t.count||!t.points)t.shown=255;
                if(model.typhoonIndex>=t.count)model.typhoonIndex=0;
            }
            if(active==App::Hot&&cJSON_IsObject(field(json,"hot"))){
                // Rows: [title, heat, tag]; item i in full.
                auto& m=model;auto& h=m.page.hot;const auto* o=field(json,"hot");const cJSON* row;
                m.hotCount=0;
                cJSON_ArrayForEach(row,field(o,"r"))if(m.hotCount<kHotRows){auto& r=h.rows[m.hotCount++];const char* title=cJSON_GetStringValue(cJSON_GetArrayItem(row,0));copy(r.title,title?title:"");
                    const auto* heat=cJSON_GetArrayItem(row,1);r.score=cJSON_IsNumber(heat)?std::clamp(heat->valuedouble,0.0,4e9):0;const auto* tag=cJSON_GetArrayItem(row,2);r.tag=cJSON_IsNumber(tag)?std::clamp(tag->valuedouble,0.0,255.0):0;}
                const double i=number(o,"i",-1);h.detailFor=i>=0&&i<m.hotCount?uint8_t(i):255;copy(h.title,string(o,"t"));copy(h.desc,string(o,"d"));
                m.hotUpdated=std::max(0.0,number(o,"at",0));
                if(m.hotIndex>=m.hotCount){m.hotIndex=0;}if(!m.hotCount){m.hotDetail=false;}
            }
            // A new question (its serial changes) resets the pick; the score carries on.
            if(active==App::Quiz&&cJSON_IsObject(field(json,"quiz"))){
                auto& m=model;const auto* q=field(json,"quiz");const double serial=number(q,"n",0);
                if(serial>0&&serial!=m.quizSerial){
                    m.quizSerial=serial;copy(m.page.quiz.question,string(q,"q"));copy(m.page.quiz.category,string(q,"cat"));copy(m.page.quiz.level,string(q,"lv"));
                    m.quizCount=0;const cJSON* answer;cJSON_ArrayForEach(answer,field(q,"a"))if(m.quizCount<4&&cJSON_IsString(answer))copy(m.page.quiz.answers[m.quizCount++],answer->valuestring);
                    m.quizCorrect=std::clamp(number(q,"c",0),0.0,3.0);m.quizPick=0;m.quizShown=m.quizWant=false;
                }
            }
            if(active==App::Quiz&&model.livePhase!=LivePhase::Loading){quizAsked_=false;if(model.livePhase==LivePhase::Error)model.quizWant=false;}
            if(active==App::Codex) for(unsigned i=0;i<4;++i){const auto* window=cJSON_GetArrayItem(field(json,"windows"),i);auto& quota=model.quotas[i];
                const double used=number(window,"used",-1),minutes=number(window,"minutes",0);
                quota.known=used>=0&&used<=100&&minutes>0;quota.used=quota.known?used:0;quota.minutes=quota.known?minutes:0;quota.resets=number(window,"resets",0);
            }
        } else ++stale_;
        cJSON_Delete(json); mbedtls_platform_zeroize(&message,sizeof(message)); renderLocal(now);
        printf("SERVICE_APPLIED received=%lu stale=%lu\n",static_cast<unsigned long>(received_),static_cast<unsigned long>(stale_));
    }
    const uint64_t interval=active==App::Monitor ? 2 : active==App::Codex ? 10 : active==App::Guard ? 30 : active==App::News ? (model.newsPage==1?120:0) : active==App::Rate||active==App::Forecast ? 1800 : active==App::Stock ? 20 : active==App::Quake ? 300 : active==App::Hacker ? 900 : active==App::Typhoon ? 1800 : active==App::Hot ? 600 : (active==App::Home) ?
#ifdef PASSPORT_DEVICE
        weatherRefreshSeconds(model.weatherReading.known)
#else
        2
#endif
        :0;
    if(interval && now-requestedAt_>=interval && (!pending_ || now-requestedAt_>20) && !voiceJob_
#ifdef PASSPORT_DEVICE
       &&(active!=App::Home||native_->idle())
#endif
    ) { requestedAt_=now; request("refresh"); }
    // 语音指令: home 10 s after the phrase (or a failure), where 你好小智 listens again.
    if(active==App::Voice){if(entered||model.voiceBusy()||model.livePhase==LivePhase::Loading||!nativeIdle())voiceAt_=now;else if(now>=voiceAt_+10){runtime_.switchTo(App::Home);return true;}}
    if(now!=renderedAt_) {
        renderedAt_=now; renderLocal(now);
        if(active==App::Home||active==App::Focus||active==App::Slides||active==App::Countdown||active==App::Sonar||active==App::Noise||active==App::Alarm||(active==App::Memo&&(model.listening()||model.memoSavedAt+5>now))||((speaksOnce(active)||speaksToo(active)||active==App::Translate||active==App::Voice)&&model.listening())||(active==App::Dictation&&(model.listening()||model.dictationAt+5>now))) dirty=true;
    }
    focusRemaining_=model.focusRemaining; focusRunning_=model.focusRunning; focusFinished_=model.focusFinished;
    focusPhase_=model.focusPhase;focusRounds_=model.focusRounds;focusDuration_=model.focusDuration;
    clockValid_=model.clockValid;
    const bool needsHost=needsComputer(active);
    const bool hostMissing=!hostContact_.load()||now>hostContact_.load()+5;
    model.bridgeAlive=!hostMissing;
    if(needsHost){
        const char* stage=active==App::Chat?"云端连接中":active==App::Dictation?(usb_?"USB 连接中":"蓝牙连接中"):active==App::Codex?"读取额度中":active==App::Monitor?"读取电脑状态":active==App::Guard?"正在连接电脑":"天气更新中";
        const bool computerLinked=usb_||model.bleConnected||bleReady_;
        const char* hint=computerConnectionHint(usb_,model.bleConnected||bleReady_,!hostMissing);
        if(hint)stage=hint;
        else if(active==App::Dictation&&(bleReady_||usb_))stage="准备语音输入";
        copy(model.connectionState,stage);
        if(hostMissing||!computerLinked){if(!loadingAt_)loadingAt_=now;if(model.livePhase!=LivePhase::Error)model.livePhase=LivePhase::Loading;}
        if(hostWasMissing_&&!hostMissing&&computerLinked&&!pending_)request("enter");
        if(model.livePhase==LivePhase::Loading&&now>loadingAt_+20){model.livePhase=LivePhase::Error;copy(model.liveState,hint?hint:"电脑助手响应超时");}
        if(model.livePhase==LivePhase::Error&&hint)copy(model.liveState,hint);
    }
    hostWasMissing_=hostMissing;
#ifdef PASSPORT_DEVICE
    if(needsInternet(active)&&networkReady_&&!previousNetwork_&&!entered){requestedAt_=now;request("enter");dirty=true;}
#endif
    previousNetwork_=networkReady_;
    if(active==App::Daily&&model.dailyWant!=model.dailyBack&&model.dailyWant!=dailyAsked_&&!pending_){
        dailyAsked_=model.dailyWant;char operation[12];snprintf(operation,sizeof(operation),"day:%u",model.dailyWant);request(operation);dirty=true;}
    // 播客: the device plays the program the page shows, or shows its list.
    if(active==App::Podcast&&!pending_){const int want=model.podcastPlayer?model.podcastIndex:-1;
        if(want!=podcastAsked_){podcastAsked_=want;char operation[12];if(want<0)copy(operation,"list");else snprintf(operation,sizeof(operation),"play:%d",want);request(operation);dirty=true;}}
    if(active==App::Stock&&!pending_&&!voiceJob_){char want[16];stockOperation(model,want);if(strcmp(want,stockAsked_)){request(want);dirty=true;}}
    if(active==App::Quiz&&model.quizWant&&!quizAsked_&&!pending_){quizAsked_=true;request("next");dirty=true;}
    // 台风: the picked typhoon's track; 热榜: the picked item in full (the device holds the board, so no download).
    if(active==App::Typhoon&&!pending_&&model.page.typhoon.count&&model.typhoonIndex!=typhoonAsked_){request("refresh");dirty=true;}
    if(active==App::Hot&&!pending_&&model.hotCount&&model.hotIndex!=model.page.hot.detailFor&&model.hotIndex!=hotAsked_){
        hotAsked_=model.hotIndex;char operation[12];snprintf(operation,sizeof(operation),"item:%u",model.hotIndex);request(operation);dirty=true;}
    // 预警: the text of the warning shown, once per opening.
    if(active==App::Forecast&&!pending_){const auto& f=model.page.forecast;const uint8_t want=model.forecastWarn;
        if(!want)warnAsked_=0;
        else if(want!=warnAsked_&&f.warnings!=0xff&&want<=f.warnings&&f.detailFor!=want-1){warnAsked_=want;char operation[12];snprintf(operation,sizeof(operation),"warn:%u",want-1);request(operation);dirty=true;}}
    if(active==App::Words&&!pending_)if(const auto* card=wordsCurrent(model.words,model.wordSession);card&&card->word!=wordAsked_){
        wordAsked_=card->word;char operation[16];snprintf(operation,sizeof(operation),"look:%u",card->word);request(operation);dirty=true;}
    if(active==App::News&&model.newsWant!=model.newsPage&&model.newsWant!=newsAsked_&&!pending_){
        newsAsked_=model.newsWant;char operation[12];snprintf(operation,sizeof(operation),"page:%u",model.newsWant);request(operation);dirty=true;
    }
    // 财经新闻: the picked headline's text (the device holds the page, so no download).
    else if(active==App::News&&!pending_&&model.newsCount&&model.newsWant==model.newsPage&&model.newsIndex!=model.page.news.textFor&&model.newsIndex!=newsItemAsked_){
        newsItemAsked_=model.newsIndex;char operation[12];snprintf(operation,sizeof(operation),"item:%u",model.newsIndex);request(operation);dirty=true;}
    if(!pending_&&*deferred_){char operation[12];copy(operation,deferred_);deferred_[0]=0;request(operation);}
    phase_=static_cast<unsigned>(model.livePhase);
    dirty|=previousPhase!=model.livePhase||strcmp(previousConnection,model.connectionState);
    // HTTP reads these atomics; publish the values owned by the main task.
    acceptedId_=accepted_;receivedCount_=received_;staleCount_=stale_;
    xSemaphoreTake(viewMutex_,portMAX_DELAY);copy(view_.state,model.liveState);copy(view_.value,model.liveValue);copy(view_.connection,model.connectionState);memcpy(view_.lines,model.liveLines,sizeof(view_.lines));view_.computer=needsComputer(active);view_.bridge=model.bridgeAlive;xSemaphoreGive(viewMutex_);
    return dirty;
}
bool LiveServices::enqueueNative(const char* json){
    if(strlen(json)>=sizeof(Message::json)){return false;}
    Message message{};
    strcpy(message.json,json);message.native=true;
#ifdef PASSPORT_DEVICE
    const bool sent=xQueueOverwrite(nativeMessages_,&message)==pdTRUE;
#else
    const bool sent=xQueueSend(messages_,&message,0)==pdTRUE;
#endif
    mbedtls_platform_zeroize(&message,sizeof(message));return sent;
}
bool LiveServices::enqueueComputer(const char* raw){
    if(strlen(raw)>=sizeof(Message::json))return false;
    auto* json=cJSON_Parse(raw);const bool heartbeat=json&&!strcmp(string(json,"control"),"heartbeat");cJSON_Delete(json);
    hostSeen();if(heartbeat)return true;
    auto* message=new(std::nothrow)Message{};if(!message)return false;
    strcpy(message->json,raw);const bool queued=xQueueSend(messages_,message,0)==pdTRUE;
    mbedtls_platform_zeroize(message,sizeof(*message));delete message;return queued;
}
bool LiveServices::nativeIdle()const{
#ifdef PASSPORT_DEVICE
    return !native_||native_->idle();
#else
    return true;
#endif
}
bool LiveServices::nativeBusy()const{
#ifdef PASSPORT_DEVICE
    return native_&&native_->busy();
#else
    return false;
#endif
}
bool LiveServices::stopAudio(){
#ifdef PASSPORT_DEVICE
    if(native_&&!native_->idle()){native_->cancel(runtime_.generation()+1);return true;}
#endif
    return audio_.stop();
}
bool LiveServices::allowLocation(bool allowed){
#ifdef PASSPORT_DEVICE
    return native_&&native_->allowLocation(allowed);
#else
    (void)allowed;return false;
#endif
}
}
