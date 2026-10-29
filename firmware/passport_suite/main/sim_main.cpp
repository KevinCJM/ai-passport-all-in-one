#include <vector>
#include "suite.hpp"
#include "voice_add.hpp"
#include <tuple>
#include "ble_model.hpp"
#include "identity.hpp"
#include "esp_rom_crc.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <ctime>
#include <cstdlib>
#include <cmath>
#include <sys/time.h>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

namespace {
using namespace passport;
unsigned checks;
constexpr unsigned kCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    printf("SUITE_FAIL line=%d condition=%s\n", __LINE__, #condition); abort(); } } while (0)
EventGroupHandle_t wifiEvents;
void networkEvent(void*, esp_event_base_t base, int32_t id, void*) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(wifiEvents, 1);
}
void networkStart(const SystemSettings& settings) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    CHECK(esp_netif_create_default_wifi_sta());
    wifiEvents = xEventGroupCreate(); CHECK(wifiEvents);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, networkEvent, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, networkEvent, nullptr));
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t config{};
    memcpy(config.sta.ssid, settings.wifiSsid, strnlen(settings.wifiSsid, sizeof(config.sta.ssid)));
    memcpy(config.sta.password, settings.wifiPassword, strnlen(settings.wifiPassword, sizeof(config.sta.password)));
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    CHECK(xEventGroupWaitBits(wifiEvents, 1, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000)) & 1);
    puts("NETWORK_OK real_wifi_driver=1 associated=1 ipv4=1 external_services=fixtures");
}
size_t freeHeap() { return heap_caps_get_free_size(kCaps); }
void sample(const char* stage, const Runtime& runtime) {
    printf("MEM {\"stage\":\"%s\",\"app\":\"%s\",\"free\":%u,\"largest\":%u,\"minimum_ever\":%u,\"arena\":%u,\"stack_free_bytes\":%u}\n",
        stage, profile(runtime.active()).id, static_cast<unsigned>(freeHeap()),
        static_cast<unsigned>(heap_caps_get_largest_free_block(kCaps)),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(kCaps)),
        static_cast<unsigned>(runtime.workspaceBytes()), static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    CHECK(heap_caps_check_integrity_all(true));
}
void domainTests(Runtime& runtime) {
    CHECK(runtime.switchTo(App::RetiredOtp)==Result::Invalid);
    CHECK(runtime.switchTo(App::Focus) == Result::Ok);
    runtime.tick(100);
    runtime.model.action(App::Focus, 100);
    CHECK(runtime.model.focusRunning && runtime.model.focusDeadline == 1600);
    runtime.model.action(App::Focus, 200);
    CHECK(!runtime.model.focusRunning && runtime.model.focusRemaining == 1400);
    runtime.model.action(App::Focus, 200);
    CHECK(runtime.model.focusRunning && runtime.model.focusDeadline == 1600);
    runtime.tick(200);
    CHECK(runtime.switchTo(App::Radio) == Result::Ok);
    const auto notifications = runtime.model.notificationsReceived;
    runtime.tick(1600);
    CHECK(!runtime.model.focusFinished && !runtime.model.focusRunning && runtime.model.focusRemaining == 1400);
    CHECK(runtime.model.notificationsReceived == notifications);
    CHECK(!runtime.codexNotice("hidden background event"));
    CHECK(runtime.active() == App::Radio);
    CHECK(runtime.switchTo(App::Focus) == Result::Ok);
    CHECK(!runtime.model.focusRunning);
    runtime.model.action(App::Focus, 1600);
    runtime.tick(3000);
    CHECK(runtime.model.focusFinished && !runtime.model.focusRunning && runtime.model.noticeCount);
    CHECK(runtime.switchTo(App::Home) == Result::Ok);
    CHECK(runtime.switchTo(App::Codex) == Result::Ok);
    for (int i = 0; i < 100; ++i) CHECK(runtime.codexNotice("Codex：任务已完成，结果保存在电脑"));
    CHECK(runtime.model.noticeCount == 8 && runtime.model.notificationsDropped == 93);
    CHECK(!runtime.model.quotaKnown);
    CHECK(runtime.switchTo(App::Home) == Result::Ok);
    static Model pomo;
    pomo.focusPeriods={1,1,1};pomo.focusDuration=pomo.focusRemaining=1;
    uint64_t step=10;
    for(unsigned round=1;round<=4;++round){
        pomo.action(App::Focus,step);CHECK(pomo.focusRunning);CHECK(!pomo.selectFocus(1));
        pomo.tick(App::Focus,++step);CHECK(pomo.focusFinished&&pomo.focusRounds==round);
        pomo.action(App::Focus,step);CHECK(pomo.focusPhase==(round==4?2:1));
        pomo.tick(App::Focus,++step);CHECK(pomo.focusFinished&&pomo.focusRounds==round);
        pomo.action(App::Focus,step);CHECK(pomo.focusPhase==0&&pomo.focusRunning);
        pomo.action(App::Focus,step);CHECK(!pomo.focusRunning);
    }
    CHECK(pomo.selectFocus(-1)&&pomo.focusPhase==2);CHECK(pomo.selectFocus(1)&&pomo.focusPhase==0);
    CHECK(runtime.switchTo(App::Chat)==Result::Ok);
    runtime.key(Key::Up,step);CHECK(runtime.model.volume==60);
    runtime.key(Key::Down,step);CHECK(runtime.model.volume==55);
    for(unsigned i=0;i<24;++i){runtime.key(Key::Up,step);}CHECK(runtime.model.volume==100);
    for(unsigned i=0;i<24;++i){runtime.key(Key::Down,step);}CHECK(runtime.model.volume==0);
    for(unsigned i=0;i<11;++i){runtime.key(Key::Up,step);}CHECK(runtime.model.volume==55);
    CHECK(runtime.switchTo(App::Home)==Result::Ok);runtime.key(Key::Up,step);CHECK(runtime.model.volume==55);runtime.key(Key::Down,step);
    puts("CASE_PASS pomodoro_phases_four_rounds_pause_wrap_and_no_background");
    puts("CASE_PASS foreground_only_timer_pause_notifications");
}
void settingsTests(Runtime& runtime){
    CHECK(kFunctions.size()==42&&kFunctions.back()==App::Settings&&kFunctions[0]==App::Chat);
    // Physical long-confirm emits Back: each step must follow the hierarchy.
    for(auto app:kFunctions){
        CHECK(runtime.switchTo(app)==Result::Ok);runtime.model.settingsPage=SettingsPage::List;
        runtime.key(Key::Back,0);CHECK(runtime.active()==App::Menu);
        runtime.key(Key::Back,0);CHECK(runtime.active()==App::Home);
    }
    for(unsigned child=0;child<kSettingsRows;++child){
        CHECK(runtime.switchTo(App::Settings)==Result::Ok);runtime.model.settingsPage=SettingsPage::List;runtime.model.settingsSelection=child;
        runtime.key(Key::Confirm,0);CHECK(runtime.model.settingsPage!=SettingsPage::List);
        runtime.key(Key::Back,0);CHECK(runtime.active()==App::Settings&&runtime.model.settingsPage==SettingsPage::List);
        runtime.key(Key::Back,0);CHECK(runtime.active()==App::Menu);runtime.key(Key::Back,0);CHECK(runtime.active()==App::Home);
    }
    CHECK(runtime.switchTo(App::Settings)==Result::Ok);runtime.model.settingsPage=SettingsPage::List;runtime.model.settingsSelection=2;
    const auto brightness=runtime.settings().brightness;
    runtime.key(Key::Confirm,1);CHECK(runtime.model.settingsPage==SettingsPage::Brightness);
    runtime.key(Key::Down,2);CHECK(runtime.model.settingsValue==brightness-10);runtime.key(Key::Back,3);CHECK(runtime.settings().brightness==brightness);
    runtime.key(Key::Confirm,4);runtime.key(Key::Down,5);runtime.key(Key::Confirm,6);CHECK(runtime.settings().brightness==brightness-10);
    SystemSettings saved;SettingsStore store;CHECK(store.load(saved)&&saved.brightness==brightness-10);
    runtime.key(Key::Confirm,7);runtime.key(Key::Up,8);runtime.key(Key::Confirm,9);CHECK(runtime.settings().brightness==brightness);
    runtime.key(Key::Down,10);runtime.key(Key::Confirm,11);const auto volume=runtime.model.volume;runtime.key(Key::Up,12);runtime.key(Key::Confirm,13);CHECK(runtime.model.volume==volume+5);
    runtime.key(Key::Confirm,14);runtime.key(Key::Down,15);runtime.key(Key::Confirm,16);CHECK(runtime.model.volume==volume);
    // 自动息屏: Confirm on the time row changes it in place (long Confirm cancels); the second row is 连电脑时常亮.
    runtime.key(Key::Down,17);runtime.key(Key::Confirm,18);CHECK(runtime.model.settingsPage==SettingsPage::Idle&&runtime.model.idleRow==0);
    runtime.key(Key::Confirm,18);runtime.key(Key::Up,19);runtime.key(Key::Confirm,20);CHECK(runtime.settings().idleSeconds==420&&runtime.model.idleRow==0&&runtime.model.settingsPage==SettingsPage::Idle);
    runtime.key(Key::Confirm,21);runtime.key(Key::Down,22);runtime.key(Key::Back,22);CHECK(runtime.settings().idleSeconds==420&&runtime.model.settingsValue==7&&runtime.model.settingsPage==SettingsPage::Idle);
    runtime.key(Key::Confirm,23);runtime.key(Key::Down,23);runtime.key(Key::Confirm,23);CHECK(runtime.settings().idleSeconds==360);
    CHECK(runtime.model.usbAwake);runtime.key(Key::Down,23);CHECK(runtime.model.idleRow==1);
    runtime.key(Key::Confirm,23);CHECK(!runtime.model.usbAwake&&SettingsStore::loadByte("usb_dim")==1);uiRender(runtime);uiCapture("settings_idle_usb");
    runtime.key(Key::Confirm,23);CHECK(runtime.model.usbAwake&&SettingsStore::loadByte("usb_dim")==0);
    runtime.key(Key::Back,23);CHECK(runtime.model.settingsPage==SettingsPage::List);
    runtime.model.settingsSelection=1;runtime.key(Key::Confirm,24);CHECK(runtime.model.settingsPage==SettingsPage::Bluetooth);runtime.key(Key::Confirm,25);CHECK(!runtime.settings().bluetoothEnabled);runtime.key(Key::Confirm,26);CHECK(runtime.settings().bluetoothEnabled);
    runtime.key(Key::Back,27);runtime.model.settingsSelection=0;runtime.key(Key::Confirm,28);CHECK(runtime.model.settingsPage==SettingsPage::Wifi);runtime.key(Key::Back,29);CHECK(runtime.model.settingsPage==SettingsPage::List);
    // 已保存的 WiFi: the last joined first, five at most (a sixth pushes the oldest out); joining one makes it the one
    // tried at boot; a long Down twice forgets the picked one, and forgetting the boot one falls back to the next.
    {
        auto& m=runtime.model;const auto before=runtime.settings();
        WifiList list;CHECK(list.remember("Home","homepass")&&list.remember("Phone","phonepass"));
        CHECK(list.count==2&&!strcmp(list.items[0].ssid,"Phone")&&!list.remember("Phone","phonepass"));
        CHECK(list.remember("Home","newpass")&&list.count==2&&!strcmp(list.items[0].password,"newpass")&&!strcmp(list.items[1].ssid,"Phone"));
        for(const char* n:{"A","B","C","D"})CHECK(list.remember(n,"p"));
        CHECK(list.count==5&&!strcmp(list.items[0].ssid,"D")&&list.find("Phone")<0&&list.find("Home")==4);
        list.remove(0);CHECK(list.count==4&&!strcmp(list.items[0].ssid,"C")&&!*list.items[4].ssid);
        CHECK(!list.remember("","")&&!list.remember("012345678901234567890123456789012","p"));
        CHECK(SettingsStore::saveWifi(WifiList{}));
        runtime.rememberWifi("Office","officepass");runtime.rememberWifi("Kevin的iPhone热点 15 Pro","pp");
        CHECK(m.wifiSavedCount==2&&!strcmp(m.wifiSaved[1],"Office")&&!strcmp(runtime.settings().wifiSsid,"Kevin的iPhone热点 15 Pro"));
        SystemSettings boot;CHECK(store.load(boot)&&!strcmp(boot.wifiPassword,"pp"));
        runtime.rememberWifi("我的手机热点 Kevin's iPhone 15 Pro","x");CHECK(m.wifiSavedCount==2&&!strcmp(runtime.settings().wifiPassword,"pp"));  // 40 bytes: no WiFi name
        runtime.key(Key::Confirm,30);CHECK(m.settingsPage==SettingsPage::Wifi&&m.wifiRow==0);
        m.wifiConnected=true;uiRender(runtime);uiCapture("settings_wifi_list");
        runtime.key(Key::Down,30);runtime.key(Key::Down,30);CHECK(m.wifiRow==2);runtime.key(Key::Down,30);CHECK(m.wifiRow==0);
        runtime.key(Key::Up,30);CHECK(m.wifiRow==2);runtime.key(Key::Up,30);CHECK(m.wifiRow==1);
        runtime.forgetWifi();CHECK(m.wifiArmed&&strstr(m.lastNotice,"再长按下键删除"));uiRender(runtime);uiCapture("settings_wifi_forget");
        runtime.key(Key::Down,30);CHECK(!m.wifiArmed&&m.wifiRow==2&&m.wifiSavedCount==2);runtime.key(Key::Up,30);
        runtime.forgetWifi();runtime.forgetWifi();CHECK(!strcmp(m.lastNotice,"已删除")&&m.wifiSavedCount==1&&!strcmp(m.wifiSaved[0],"Office"));
        CHECK(!strcmp(runtime.settings().wifiSsid,"Office")&&!strcmp(runtime.settings().wifiPassword,"officepass")&&m.wifiRow==1);
        WifiList left;CHECK(SettingsStore::loadWifi(left)&&left.count==1&&store.load(boot)&&!strcmp(boot.wifiSsid,"Office"));
        m.wifiRow=0;runtime.forgetWifi();CHECK(!m.wifiArmed&&m.wifiSavedCount==1);  // 添加 WiFi is not a network
        m.wifiConnected=false;runtime.rememberWifi(before.wifiSsid,before.wifiPassword);CHECK(!strcmp(runtime.settings().wifiSsid,before.wifiSsid));
        runtime.key(Key::Back,30);CHECK(m.settingsPage==SettingsPage::List);
    }
    // 语音唤醒: the row above the last (所在城市), reached upward from the first; Confirm toggles and saves it.
    runtime.key(Key::Up,29);runtime.key(Key::Up,29);CHECK(runtime.model.settingsSelection==kSettingsRows-2);runtime.key(Key::Confirm,29);CHECK(runtime.model.settingsPage==SettingsPage::Wake);
    runtime.key(Key::Confirm,29);CHECK(runtime.model.wakeWord&&SettingsStore::loadByte("wake_word")==1);uiRender(runtime);uiCapture("settings_wake");
    runtime.key(Key::Confirm,29);CHECK(!runtime.model.wakeWord&&SettingsStore::loadByte("wake_word")==0);
    // The second row, 说功能名跳转, is on until switched off (saved as jump_off) and back on.
    CHECK(runtime.model.voiceJump&&runtime.model.wakeRow==0);runtime.key(Key::Down,29);CHECK(runtime.model.wakeRow==1);
    runtime.key(Key::Confirm,29);CHECK(!runtime.model.voiceJump&&SettingsStore::loadByte("jump_off")==1);uiRender(runtime);uiCapture("settings_wake_jump");
    runtime.key(Key::Confirm,29);CHECK(runtime.model.voiceJump&&SettingsStore::loadByte("jump_off")==0&&!runtime.model.wakeWord);
    // The third, 打开 Mac 程序, likewise (mac_off); Down from it and Up from the first wrap round.
    CHECK(runtime.model.macOpen);runtime.key(Key::Down,29);CHECK(runtime.model.wakeRow==2);
    runtime.key(Key::Confirm,29);CHECK(!runtime.model.macOpen&&SettingsStore::loadByte("mac_off")==1&&runtime.model.voiceJump);uiRender(runtime);uiCapture("settings_wake_mac");
    runtime.key(Key::Confirm,29);CHECK(runtime.model.macOpen&&SettingsStore::loadByte("mac_off")==0);
    // The fourth, 后台接收对讲, is off until switched on (walkie_bg); Down from it and Up from the first wrap round.
    CHECK(!runtime.model.walkieListen);runtime.key(Key::Down,29);CHECK(runtime.model.wakeRow==3);
    runtime.key(Key::Confirm,29);CHECK(runtime.model.walkieListen&&SettingsStore::loadByte("walkie_bg")==1&&runtime.model.macOpen);uiRender(runtime);uiCapture("settings_wake_walkie");
    runtime.key(Key::Confirm,29);CHECK(!runtime.model.walkieListen&&SettingsStore::loadByte("walkie_bg")==0);
    runtime.key(Key::Down,29);CHECK(runtime.model.wakeRow==0);runtime.key(Key::Up,29);CHECK(runtime.model.wakeRow==3);
    runtime.key(Key::Back,29);runtime.model.settingsSelection=0;
    uiRender(runtime);uiCapture("settings_menu");runtime.model.settingsSelection=2;runtime.key(Key::Confirm,30);uiRender(runtime);uiCapture("settings_brightness");runtime.key(Key::Back,31);runtime.key(Key::Back,32);CHECK(runtime.active()==App::Menu);
    CHECK(runtime.switchTo(App::Home)==Result::Ok);
    // 打开 Mac 程序: what the computer did shows in the footer on any page, in place of the key hints.
    strcpy(runtime.model.macNote,"已在 Mac 打开 微信");uiRender(runtime);uiCapture("mac_note");runtime.model.macNote[0]=0;
    puts("CASE_PASS settings_menu_edit_cancel_persist_and_restore");
}
void powerTests(Runtime& runtime) {
    runtime.connection(true, 0);
    CHECK(runtime.power(1000000) == Power::Awake);
    runtime.suspended(true);
    CHECK(runtime.power(1000001) == Power::Awake);
    runtime.connection(false, 100);
    CHECK(runtime.power(459) == Power::Awake);
    CHECK(runtime.power(460) == Power::DeepSleepIntent);
    runtime.key(Key::Up, 460);
    CHECK(runtime.power(819) == Power::Awake);
    runtime.flashBusy(true); CHECK(runtime.power(1000) == Power::Awake);
    runtime.flashBusy(false);
    CHECK(runtime.switchTo(App::Radio) == Result::Ok);
    CHECK(runtime.power(1000) == Power::DeepSleepIntent);
    CHECK(runtime.switchTo(App::Chat) == Result::Ok);
    runtime.model.bleConnected=true;
    CHECK(runtime.power(1000) == Power::DeepSleepIntent);
    runtime.model.bleConnected=false;
    CHECK(runtime.switchTo(App::Focus) == Result::Ok);
    runtime.model.focusRunning=true;
    CHECK(runtime.power(1000) == Power::ScreenOff);
    runtime.model.focusRunning=false;
    // A talk with the computer linked only turns the screen off; unlinked, the PPT page sleeps.
    CHECK(runtime.switchTo(App::Slides) == Result::Ok);
    CHECK(runtime.power(1000) == Power::DeepSleepIntent);
    runtime.model.bleConnected=true;
    CHECK(runtime.power(1000) == Power::ScreenOff);
    runtime.model.bleConnected=false;
    // So does 语音输入: deep sleep would drop its Bluetooth link.
    CHECK(runtime.switchTo(App::Dictation) == Result::Ok);
    CHECK(runtime.power(1000) == Power::DeepSleepIntent);
    runtime.model.bleConnected=true;CHECK(runtime.power(1000) == Power::ScreenOff);
    runtime.model.bleConnected=false;
    CHECK(runtime.switchTo(App::Home) == Result::Ok);
    // 语音唤醒 listens on the home page and the menu: idle only turns the screen off there, and other pages still sleep.
    CHECK(runtime.power(1000) == Power::DeepSleepIntent);
    runtime.model.wakeWord=true;CHECK(runtime.power(1000) == Power::ScreenOff);
    CHECK(runtime.switchTo(App::Menu) == Result::Ok);CHECK(runtime.power(1000) == Power::ScreenOff);
    CHECK(runtime.switchTo(App::Radio) == Result::Ok);CHECK(runtime.power(1000) == Power::DeepSleepIntent);
    // A playing radio or podcast turns only the screen off.
    runtime.model.mediaPlaying=true;CHECK(runtime.power(1000) == Power::ScreenOff);
    CHECK(runtime.switchTo(App::Podcast) == Result::Ok);CHECK(runtime.power(1000) == Power::ScreenOff);runtime.model.mediaPlaying=false;
    CHECK(runtime.switchTo(App::Home) == Result::Ok);runtime.model.wakeWord=false;
    // 对讲机 listens on its page, and on every page with 后台接收对讲 on; talking or hearing keeps the screen lit.
    CHECK(runtime.switchTo(App::Walkie) == Result::Ok);CHECK(runtime.power(1000) == Power::ScreenOff);
    runtime.model.walkiePhase=2;CHECK(runtime.power(1000) == Power::Awake);runtime.model.walkiePhase=0;
    CHECK(runtime.switchTo(App::Radio) == Result::Ok);runtime.model.walkieListen=true;CHECK(runtime.power(1000) == Power::ScreenOff);
    runtime.model.walkieListen=false;CHECK(runtime.switchTo(App::Home) == Result::Ok);
    // Long Confirm on the home page turns the screen off, on USB too, until the next key; it stays on Home.
    runtime.connection(true,1000);CHECK(runtime.power(1000) == Power::Awake);
    runtime.key(Key::Back,1000);CHECK(runtime.active() == App::Home && runtime.power(1000) == Power::ScreenOff);
    runtime.wake(1001);CHECK(runtime.power(1001) == Power::Awake);
    runtime.key(Key::Back,1002);runtime.key(Key::Up,1003);CHECK(runtime.power(1003) == Power::Awake);
    runtime.connection(false,1004);
    CHECK(runtime.setIdleMinutes(0));
    CHECK(runtime.power(999999) == Power::Awake);
    CHECK(!runtime.setIdleMinutes(1441));
    CHECK(runtime.setIdleMinutes(1));
    runtime.connection(true, 10);runtime.connection(false, 20);
    runtime.connection(false, 70); // Duplicate events cannot reset the deadline.
    CHECK(runtime.power(79) == Power::Awake);
    CHECK(runtime.power(80) == Power::DeepSleepIntent);
    CHECK(runtime.setIdleMinutes(6));
    runtime.connection(true, 1001);
    CHECK(runtime.power(9000) == Power::Awake);
    // 连电脑时常亮 off: on USB the idle time darkens the screen but never sleeps it.
    runtime.model.usbAwake=false;CHECK(runtime.power(9000) == Power::ScreenOff);runtime.model.usbAwake=true;
    puts("CASE_PASS usb_suspend_unplug_360s_replug_active_audio_flash_guard");
}
void alertTests(Runtime& runtime){
    auto& m=runtime.model;m.alertMode=0;
    runtime.tick(300);CHECK(runtime.switchTo(App::Alert)==Result::Ok&&m.alertChanged==300);
    // Long idle never dims or sleeps the warning light.
    CHECK(runtime.power(999999)==Power::Awake);
    runtime.key(Key::Up,301);CHECK(m.alertMode==std::size(kAlertModes)-1&&m.alertChanged==301);
    runtime.key(Key::Down,302);runtime.key(Key::Down,303);CHECK(m.alertMode==1);
    runtime.key(Key::Up,304);CHECK(m.alertMode==0);
    m.alertChanged=esp_timer_get_time()/1000000;uiRender(runtime);uiCapture("alert_named");
    m.alertChanged=0;uiRender(runtime);CHECK(uiAnimating(runtime));
    runtime.key(Key::Back,305);CHECK(runtime.active()==App::Menu);
    puts("CASE_PASS alert_light");
}
// 离座锁屏: settings stepped and kept, then each verdict the computer can report.
void guardTests(Runtime& runtime){
    auto& m=runtime.model;
    {nvs_handle_t h;CHECK(nvs_open("suite_system",NVS_READWRITE,&h)==ESP_OK);nvs_erase_key(h,"guard_v1");nvs_commit(h);nvs_close(h);}
    CHECK(runtime.switchTo(App::Guard)==Result::Ok);const auto& g=m.guard;
    CHECK(g.distance==1&&g.delay==1&&g.keep==2&&g.wake==1&&m.guardRow==0);
    m.livePhase=LivePhase::Loading;strcpy(m.connectionState,"蓝牙连接中");uiRender(runtime);uiCapture("guard_waiting");
    runtime.key(Key::Confirm,800);CHECK(g.distance==2);runtime.key(Key::Confirm,800);CHECK(g.distance==0);
    runtime.key(Key::Down,800);runtime.key(Key::Confirm,800);CHECK(m.guardRow==1&&g.delay==2);
    runtime.key(Key::Up,800);runtime.key(Key::Up,800);CHECK(m.guardRow==3);runtime.key(Key::Confirm,800);CHECK(!g.wake);
    {GuardPrefs stored;CHECK(SettingsStore::loadGuard(stored)&&stored.distance==0&&stored.delay==2&&!stored.wake);}
    runtime.connection(false,800);CHECK(runtime.power(800+99999)==Power::ScreenOff);runtime.connection(true,800);  // never deep sleep: the link must stay
    m.livePhase=LivePhase::Ready;m.guardRow=0;
    m.guardPhase=2;m.guardRssi=-58;m.guardLine=-66;m.guardBase=-58;uiRender(runtime);uiCapture("guard_present");
    m.guardPhase=3;m.guardRssi=-79;m.guardLeft=7;uiRender(runtime);uiCapture("guard_leaving");
    m.guardPhase=4;m.guardRssi=0;uiRender(runtime);uiCapture("guard_locked");
    m.guardPhase=1;m.guardLine=0;m.guardBase=0;uiRender(runtime);uiCapture("guard_usb");
    {nvs_handle_t h;CHECK(nvs_open("suite_system",NVS_READWRITE,&h)==ESP_OK);nvs_erase_key(h,"guard_v1");nvs_commit(h);nvs_close(h);}
    runtime.key(Key::Back,801);CHECK(runtime.active()==App::Menu);
    puts("CASE_PASS guard");
}
// 待办提醒: spoken todos with their times, ticking off, clearing, deleting, and reminders that ring from any page.
void todoTests(Runtime& runtime){
    auto& m=runtime.model;auto& l=m.page.todo;
    const auto forget=[]{nvs_handle_t h;CHECK(nvs_open("suite_system",NVS_READWRITE,&h)==ESP_OK);nvs_erase_key(h,"todo_v1");nvs_commit(h);nvs_close(h);};
    timeval saved{};gettimeofday(&saved,nullptr);
    const auto setClock=[](time_t at){timeval tv{at,0};settimeofday(&tv,nullptr);};
    const time_t now=1793080800;setClock(now);  // Tuesday 2026-10-27 14:00 (the simulator's local time is UTC)
    forget();runtime.reloadTodo();CHECK(!m.todoNextAt);
    CHECK(runtime.switchTo(App::Todo)==Result::Ok&&!l.count);m.livePhase=LivePhase::Ready;m.lastNotice[0]=0;
    uiRender(runtime);uiCapture("todo_empty");
    CHECK(runtime.addTodo("20分钟后提醒我拿外卖")&&l.count==1&&l.items[0].at==now+1200&&!strcmp(l.items[0].text,"拿外卖")&&strstr(m.lastNotice,"今天 14:20"));
    CHECK(runtime.addTodo("明天下午3点在会议室开会，记得带电脑。")&&runtime.addTodo("买牛奶")&&runtime.addTodo("周五交周报"));
    CHECK(l.count==4&&!strcmp(l.items[1].text,"在会议室开会，记得带电脑")&&!strcmp(l.items[2].text,"交周报")&&!strcmp(l.items[3].text,"买牛奶")&&!l.items[3].at);
    CHECK(strstr(m.lastNotice,"周五 09:00")&&m.todoIndex==3);
    CHECK(m.todoNextAt==now+1200&&!strcmp(m.todoNext,"拿外卖"));
    CHECK(!runtime.addTodo("。")&&strstr(m.lastNotice,"没有听清"));
    m.lastNotice[0]=0;runtime.key(Key::Up,900);runtime.key(Key::Up,900);CHECK(m.todoIndex==1);uiRender(runtime);uiCapture("todo_list");
    // Ticking one off moves it last, the selection with it; the next reminder moves on.
    runtime.key(Key::Confirm,900);CHECK((l.items[3].flags&TodoList::kDone)&&!strcmp(l.items[3].text,"拿外卖")&&m.todoIndex==4);
    CHECK(!strcmp(m.todoNext,"在会议室开会，记得带电脑"));uiRender(runtime);uiCapture("todo_done");
    {TodoList stored;CHECK(SettingsStore::loadTodo(stored)&&stored.count==4&&(stored.items[3].flags&TodoList::kDone));}
    // A long Up clears the done ones; a long Down twice deletes the selected one.
    CHECK(runtime.longKey(true,901)&&l.count==3&&strstr(m.lastNotice,"清除 1 条"));
    runtime.key(Key::Down,901);runtime.key(Key::Down,901);runtime.key(Key::Down,901);CHECK(m.todoIndex==3&&!strcmp(l.items[2].text,"买牛奶"));
    CHECK(runtime.longKey(false,902)&&m.todoArmed&&l.count==3);uiRender(runtime);uiCapture("todo_delete");
    CHECK(runtime.longKey(false,902)&&l.count==2&&!m.todoArmed&&strstr(m.lastNotice,"已删除"));
    // A reminder rings from another page; Confirm marks it done.
    CHECK(runtime.addTodo("1分钟后提醒我喝水")&&m.todoNextAt>=now+60&&m.todoNextAt<=now+75);
    CHECK(runtime.switchTo(App::Calendar)==Result::Ok);setClock(m.todoNextAt+1);runtime.tick(903);
    CHECK(runtime.active()==App::Todo&&m.todoRinging&&runtime.power(903+99999)==Power::Awake);uiRender(runtime);uiCapture("todo_ringing");
    runtime.key(Key::Confirm,904);CHECK(!m.todoRinging);
    {bool done=false;for(unsigned i=0;i<l.count;++i)if(!strcmp(l.items[i].text,"喝水"))done=l.items[i].flags&TodoList::kDone;CHECK(done);}
    // Up/Down snooze it 10 minutes; long Confirm only stops it.
    CHECK(runtime.addTodo("1分钟后吃药"));setClock(now+130);runtime.tick(905);CHECK(m.todoRinging);
    runtime.key(Key::Down,906);CHECK(!m.todoRinging&&m.todoNextAt>=now+130+600&&!strcmp(m.todoNext,"吃药")&&strstr(m.lastNotice,"10 分钟"));
    setClock(now+130+601);runtime.tick(907);CHECK(m.todoRinging);
    runtime.key(Key::Back,908);CHECK(!m.todoRinging&&runtime.active()==App::Todo&&strcmp(m.todoNext,"吃药"));
    // A reminder missed by more than six hours is not rung, only marked.
    CHECK(runtime.addTodo("1分钟后收衣服"));setClock(now+8*3600);runtime.tick(909);CHECK(!m.todoRinging&&strcmp(m.todoNext,"收衣服"));
    // Sixteen at most.
    while(l.count<TodoList::kMax)CHECK(runtime.addTodo("整理桌面"));
    CHECK(!runtime.addTodo("再加一条")&&strstr(m.lastNotice,"最多 16 条"));
    m.lastNotice[0]=0;strcpy(m.liveValue,"rec");m.listenAt=909;uiRender(runtime);uiCapture("todo_listening");strcpy(m.liveValue,"--");
    runtime.key(Key::Back,910);CHECK(runtime.active()==App::Menu);
    forget();runtime.reloadTodo();settimeofday(&saved,nullptr);
    puts("CASE_PASS todo");
}
// 语音记账: spoken entries, categories, deleting, and the computer taking them (acknowledged by revision).
void ledgerTests(Runtime& runtime){
    auto& m=runtime.model;auto& b=m.page.ledger;
    const auto forget=[]{nvs_handle_t h;CHECK(nvs_open("suite_system",NVS_READWRITE,&h)==ESP_OK);nvs_erase_key(h,"ledger_v1");nvs_commit(h);nvs_close(h);};
    timeval saved{};gettimeofday(&saved,nullptr);
    const auto setClock=[](time_t at){timeval tv{at,0};settimeofday(&tv,nullptr);};
    const time_t now=1793080800;setClock(now);  // Tuesday 2026-10-27 14:00 (the simulator's local time is UTC)
    forget();
    CHECK(runtime.switchTo(App::Ledger)==Result::Ok&&!b.count);m.livePhase=LivePhase::Ready;m.lastNotice[0]=0;m.clockValid=true;
    uiRender(runtime);uiCapture("ledger_empty");
    CHECK(runtime.addLedger("午饭 35")&&b.count==1&&b.items[0].cents==3500&&!strcmp(b.items[0].note,"午饭")&&strstr(m.lastNotice,"午饭"));
    CHECK(runtime.addLedger("打车花了二十八块五")&&runtime.addLedger("收到红包200")&&runtime.addLedger("咖啡32元星巴克"));
    CHECK(!runtime.addLedger("三明治")&&strstr(m.lastNotice,"没听到金额")&&b.count==4&&ledgerStatus().pending==4);
    // The computer takes them in batches and sends its month; an entry changed on the way goes again.
    char out[2304];ledgerExchange(nullptr,0,345600,20000,202610,out,sizeof(out));
    CHECK(strstr(out,"\"note\":\"打车\"")&&strstr(out,"\"cat\":\"收入\"")&&strstr(out,"\"left\":0}")&&ledgerStatus().monthKnown);
    std::vector<LedgerAck> acks;for(unsigned i=0;i<b.count;++i)acks.push_back({b.items[i].id,b.items[i].rev});
    m.lastNotice[0]=0;runtime.key(Key::Down,900);CHECK(m.ledgerIndex==1);
    runtime.key(Key::Confirm,900);CHECK(b.items[3].category==1&&b.items[3].rev==1&&strstr(m.lastNotice,"交通"));
    ledgerExchange(acks.data(),acks.size(),345600,20000,202610,out,sizeof(out));
    CHECK(runtime.ledgerSynced()&&ledgerStatus().pending==1&&strstr(out,"\"rev\":1")&&(b.items[0].flags&LedgerBook::kSynced));
    m.lastNotice[0]=0;uiRender(runtime);uiCapture("ledger_list");
    // A long Down twice deletes: the computer is told, then it goes.
    CHECK(runtime.longKey(false,901)&&m.ledgerArmed);uiRender(runtime);uiCapture("ledger_delete");
    CHECK(runtime.longKey(false,901)&&!m.ledgerArmed&&b.count==4&&(b.items[3].flags&LedgerBook::kDeleted)&&strstr(m.lastNotice,"已删除"));
    {uint8_t order[LedgerBook::kMax];CHECK(b.shown(ledgerToday(),order)==3);}
    LedgerAck gone{b.items[3].id,b.items[3].rev};ledgerExchange(&gone,1,0,0,0,out,sizeof(out));CHECK(runtime.ledgerSynced()&&b.count==3&&!ledgerStatus().pending);
    // Tomorrow the badge keeps nothing the computer has.
    setClock(now+86400);runtime.key(Key::Up,902);CHECK(runtime.switchTo(App::Menu)==Result::Ok&&runtime.switchTo(App::Ledger)==Result::Ok&&!b.count);
    m.lastNotice[0]=0;strcpy(m.liveValue,"rec");m.listenAt=903;uiRender(runtime);uiCapture("ledger_listening");strcpy(m.liveValue,"--");
    runtime.key(Key::Back,904);CHECK(runtime.active()==App::Menu);
    for(unsigned i=0;i<kFunctions.size()&&runtime.selection()!=kFunctions.size()-1;++i)runtime.key(Key::Down,904);
    CHECK(runtime.selection()==kFunctions.size()-1);uiRender(runtime);uiCapture("menu_page5");
    forget();settimeofday(&saved,nullptr);
    puts("CASE_PASS ledger");
}
// 语音计算器: spoken sums, going on from the picked result, the list of this visit's sums, what it refuses.
void calcTests(Runtime& runtime){
    auto& m=runtime.model;auto& rows=m.page.calc;
    CHECK(runtime.switchTo(App::Calc)==Result::Ok&&!m.calcCount);m.livePhase=LivePhase::Ready;m.lastNotice[0]=0;
    uiRender(runtime);uiCapture("calc_empty");
    CHECK(runtime.addCalc("帮我算一下128乘以37等于多少？")&&m.calcCount==1&&rows[0].value==4736&&!strcmp(rows[0].shown,"128 × 37"));
    uiRender(runtime);uiCapture("calc_result");
    CHECK(runtime.addCalc("再乘以3")&&rows[0].value==14208&&!strcmp(rows[0].shown,"4,736 × 3"));
    CHECK(runtime.addCalc("一百除以七")&&runtime.addCalc("123456789乘以9")&&rows[0].value==1111111101);
    uiRender(runtime);uiCapture("calc_big");
    CHECK(!runtime.addCalc("10除以0")&&strstr(m.lastNotice,"除数不能为 0：10除以0")&&m.calcCount==4);
    uiRender(runtime);uiCapture("calc_error");
    // Down picks an older sum (Up a newer one, round the ends); the next sum goes on from it.
    runtime.key(Key::Down,910);CHECK(!*m.lastNotice);runtime.key(Key::Down,910);CHECK(m.calcIndex==2&&rows[2].value==14208);
    uiRender(runtime);uiCapture("calc_list");
    CHECK(runtime.addCalc("减208")&&m.calcIndex==0&&rows[0].value==14000&&m.calcCount==5);
    runtime.key(Key::Up,911);CHECK(m.calcIndex==4);runtime.key(Key::Down,911);CHECK(m.calcIndex==0);
    // The newest eight stay.
    for(int i=0;i<4;++i)CHECK(runtime.addCalc("1加1"));
    CHECK(m.calcCount==8&&rows[7].value==14208);
    CHECK(!runtime.addCalc("")&&strstr(m.lastNotice,"没有听清"));
    m.lastNotice[0]=0;strcpy(m.liveValue,"rec");m.listenAt=912;uiRender(runtime);uiCapture("calc_listening");
    runtime.key(Key::Down,913);CHECK(m.calcIndex==0);strcpy(m.liveValue,"--");  // keys wait while listening
    // Leaving forgets them.
    CHECK(runtime.switchTo(App::Menu)==Result::Ok&&runtime.switchTo(App::Calc)==Result::Ok&&!m.calcCount);
    puts("CASE_PASS calc");
}
// 对讲机: a call opens it from any page and goes back when over; keys keep it; the channel and talks.
void walkieTests(Runtime& runtime){
    auto& m=runtime.model;
    CHECK(runtime.switchTo(App::Radio)==Result::Ok);
    runtime.walkieCall(500);CHECK(runtime.active()==App::Walkie&&m.walkieAuto&&m.walkieBack==App::Radio&&!m.walkieTalk&&m.walkieChannel==1);
    runtime.walkieCall(501);CHECK(runtime.active()==App::Walkie&&m.walkieBack==App::Radio);  // already open: nothing changes
    m.walkiePhase=2;strcpy(m.walkieFrom,"张三");m.wifiConnected=true;runtime.tick(520);CHECK(runtime.active()==App::Walkie);  // still hearing
    uiRender(runtime);uiCapture("walkie_hearing");
    m.walkiePhase=0;m.walkieAt=530;runtime.tick(534);CHECK(runtime.active()==App::Walkie);
    runtime.tick(535);CHECK(runtime.active()==App::Radio);  // 5 s after the call
    // Long Confirm goes back at once; any other key keeps the page.
    runtime.walkieCall(600);runtime.key(Key::Back,601);CHECK(runtime.active()==App::Radio);
    runtime.walkieCall(700);m.walkiePhase=2;runtime.key(Key::Confirm,701);CHECK(!m.walkieTalk&&strstr(m.lastNotice,"对方 正在说话")&&!m.walkieAuto);
    m.walkiePhase=0;runtime.tick(800);CHECK(runtime.active()==App::Walkie);
    // Up/Down set the volume in tens; long Up/Down step the channel round 1-9 and save it.
    const auto volume=m.volume;m.volume=95;runtime.key(Key::Up,801);CHECK(m.volume==100);runtime.key(Key::Down,801);runtime.key(Key::Down,801);CHECK(m.volume==80);m.volume=volume;
    runtime.longKey(true,801);CHECK(m.walkieChannel==9&&SettingsStore::loadByte("walkie_ch")==9);
    runtime.longKey(false,802);CHECK(m.walkieChannel==1);runtime.longKey(false,802);CHECK(m.walkieChannel==2&&SettingsStore::loadByte("walkie_ch")==2);
    uiRender(runtime);uiCapture("walkie_idle");
    // Confirm starts and ends a talk; the channel stays while talking; a talk ends by itself after 30 s.
    runtime.key(Key::Confirm,810);CHECK(m.walkieTalk&&m.walkieAt==810);runtime.longKey(true,811);CHECK(m.walkieChannel==2&&strstr(m.lastNotice,"不能换频道"));
    m.walkiePhase=1;runtime.tick(822);uiRender(runtime);uiCapture("walkie_talking");
    runtime.key(Key::Confirm,823);CHECK(!m.walkieTalk);m.walkiePhase=0;
    runtime.key(Key::Confirm,830);runtime.tick(859);CHECK(m.walkieTalk);runtime.tick(860);CHECK(!m.walkieTalk);
    // Not opened by a call: long Confirm goes to the menu, and leaving forgets the talk.
    runtime.key(Key::Confirm,861);runtime.key(Key::Back,862);CHECK(runtime.active()==App::Menu);
    CHECK(runtime.switchTo(App::Walkie)==Result::Ok&&!m.walkieTalk&&!m.walkiePhase&&!*m.walkieFrom);
    m.walkieChannel=1;SettingsStore::saveByte("walkie_ch",1);CHECK(runtime.switchTo(App::Home)==Result::Ok);
    puts("CASE_PASS walkie");
}
// 语音指令 (你好小智 opens it; not in the menu): listening, then what was not understood; Back goes home.
void voiceCommandTests(Runtime& runtime){
    auto& m=runtime.model;
    CHECK(std::find(kFunctions.begin(),kFunctions.end(),App::Voice)==kFunctions.end());
    CHECK(runtime.switchTo(App::Voice)==Result::Ok);m.livePhase=LivePhase::Ready;
    strcpy(m.liveValue,"rec");m.listenAt=920;uiRender(runtime);uiCapture("voice_listening");
    strcpy(m.liveValue,"--");strcpy(m.lastNotice,"没听懂：今天中午吃什么比较好");uiRender(runtime);uiCapture("voice_missed");
    m.macOpen=false;uiRender(runtime);uiCapture("voice_missed_no_mac");m.macOpen=true;
    runtime.key(Key::Back,921);CHECK(runtime.active()==App::Home);
    puts("CASE_PASS voice");
}
// 汇率, 倒数日, 自选股 and 所在城市: what a spoken phrase adds, a long Down twice deleting, the listening screens.
void voiceAddTests(Runtime& runtime){
    auto& m=runtime.model;
    // 说功能名跳转 knows every page by the name the menu shows (and the home page and the menu themselves).
    for(App app:kFunctions)CHECK(spokenApp(profile(app).title)==app&&spokenApp(menuLabel(app))==app);
    CHECK(spokenApp(profile(App::Home).title)==App::Home&&spokenApp(profile(App::Menu).title)==App::Menu);
    // A page opened any way is the one the menu shows on the way back.
    CHECK(runtime.switchTo(App::Ledger)==Result::Ok);runtime.key(Key::Back,40);CHECK(runtime.active()==App::Menu&&kFunctions[runtime.selection()]==App::Ledger);
    timeval saved{};gettimeofday(&saved,nullptr);
    {timeval tv{1793080800,0};settimeofday(&tv,nullptr);}  // Tuesday 2026-10-27 14:00 (the simulator's local time is UTC)
    m.clockValid=true;m.livePhase=LivePhase::Ready;
    // 汇率: currencies join the board, the base stays; a long Down twice deletes the picked one.
    RateBoard rates{};for(auto& t:rates.targets)*t=0;strcpy(rates.targets[0],"USD");strcpy(rates.targets[1],"JPY");CHECK(SettingsStore::saveRates(rates));
    CHECK(runtime.switchTo(App::Rate)==Result::Ok);m.lastNotice[0]=0;
    CHECK(runtime.addRate("添加泰铢和韩币")&&SettingsStore::loadRates(rates)&&!strcmp(rates.targets[2],"THB")&&!strcmp(rates.targets[3],"KRW")&&m.rateIndex==3&&strstr(m.lastNotice,"泰铢"));
    CHECK(!runtime.addRate("人民币")&&strstr(m.lastNotice,"计价货币"));
    CHECK(!runtime.addRate("美元")&&strstr(m.lastNotice,"已经有"));
    CHECK(!runtime.addRate("今天天气不错")&&strstr(m.lastNotice,"没听出货币"));
    strcpy(m.rateBase,"CNY");m.rateCount=4;m.rateUpdated=1792800151;m.rates[0]={"USD",0.1403};m.rates[1]={"JPY",21.31};m.rates[2]={"THB",4.592};m.rates[3]={"KRW",197.2};
    m.rateIndex=1;m.lastNotice[0]=0;
    CHECK(runtime.longKey(false,950)&&m.rateArmed);uiRender(runtime);uiCapture("rate_delete");
    CHECK(runtime.longKey(false,950)&&!m.rateArmed&&m.rateCount==3&&!strcmp(m.rates[1].code,"THB")&&SettingsStore::loadRates(rates)&&!strcmp(rates.targets[1],"THB")&&!*rates.targets[3]);
    m.lastNotice[0]=0;strcpy(m.liveValue,"rec");m.listenAt=951;uiRender(runtime);uiCapture("rate_listening");strcpy(m.liveValue,"--");
    m.rateCount=0;rates=RateBoard{};CHECK(SettingsStore::saveRates(rates));
    // 倒数日: a spoken event shows at once; an added one can be deleted, a built-in one cannot.
    CountdownBoard none{};CHECK(SettingsStore::saveCountdown(none));
    CHECK(runtime.switchTo(App::Countdown)==Result::Ok);
    CHECK(!runtime.addCountdown("考研")&&strstr(m.lastNotice,"没听到日期"));
    CHECK(runtime.addCountdown("妈妈生日 农历八月十五")&&!strcmp(m.countdown.events[0].name,"妈妈生日")&&m.countdown.events[0].flags==(CountdownBoard::kYearly|CountdownBoard::kLunar)&&strstr(m.lastNotice,"妈妈生日"));
    CHECK(runtime.addCountdown("考研 12月20日")&&m.countdown.events[1].date==20261220&&strstr(m.lastNotice,"考研"));
    {CountdownItem items[20];const unsigned n=m.countdownItems({2026,10,27},14*60,items,20);CHECK(items[m.countdownIndex%n].name==m.countdown.events[1].name);}
    uiRender(runtime);uiCapture("countdown_added");
    m.lastNotice[0]=0;CHECK(runtime.longKey(false,960)&&m.countdownArmed);uiRender(runtime);uiCapture("countdown_delete");
    CHECK(runtime.longKey(false,960)&&!*m.countdown.events[1].name&&strstr(m.lastNotice,"已删除"));
    {CountdownBoard back;CHECK(SettingsStore::loadCountdown(back)&&!*back.events[1].name&&!strcmp(back.events[0].name,"妈妈生日"));}
    m.countdownIndex=0;m.lastNotice[0]=0;CHECK(runtime.longKey(false,961)&&!m.countdownArmed&&strstr(m.lastNotice,"内置"));
    m.lastNotice[0]=0;strcpy(m.liveValue,"rec");m.listenAt=962;uiRender(runtime);uiCapture("countdown_listening");strcpy(m.liveValue,"--");
    CHECK(SettingsStore::saveCountdown(none));
    // 自选股: what the device added, the delete question (LiveServices asks the device), the listening screen.
    CHECK(runtime.switchTo(App::Stock)==Result::Ok);
    {auto& rows=m.page.stocks.rows;
     rows[0]={"贵州茅台","600519","SH",2,1335.0f,16.51f,1.25f,1311.84f,1343.88f,1310.0f,1318.49f,7.68e9f};
     rows[1]={"比亚迪","002594","SZ",2,102.6f,-1.2f,-1.16f,103.5f,104.1f,101.9f,103.8f,3.1e9f};
     m.stockCount=2;m.stockIndex=1;m.stockUpdated=1793080800;}
    strcpy(m.lastNotice,"已添加比亚迪（深A）");uiRender(runtime);uiCapture("stock_added");
    m.lastNotice[0]=0;m.stockArmed=true;uiRender(runtime);uiCapture("stock_delete");runtime.key(Key::Down,970);CHECK(!m.stockArmed&&m.stockIndex==0);
    strcpy(m.liveValue,"rec");m.listenAt=971;uiRender(runtime);uiCapture("stock_listening");runtime.key(Key::Down,972);CHECK(m.stockIndex==0);strcpy(m.liveValue,"--");m.stockCount=0;
    // 所在城市: the last Settings row; the Confirm that opens it does not also start listening.
    CHECK(runtime.switchTo(App::Settings)==Result::Ok);m.settingsPage=SettingsPage::List;m.settingsSelection=kSettingsRows-1;
    strcpy(m.cityName,"杭州 · 浙江");uiRender(runtime);uiCapture("settings_city_row");
    runtime.key(Key::Confirm,980);CHECK(m.settingsPage==SettingsPage::City&&m.confirmTaken&&!m.cityRow);
    strcpy(m.lastNotice,"所在城市已改为杭州");uiRender(runtime);uiCapture("settings_city");
    runtime.key(Key::Down,981);CHECK(m.cityRow==1&&!m.confirmTaken&&!*m.lastNotice);runtime.key(Key::Up,981);CHECK(!m.cityRow);
    strcpy(m.liveValue,"rec");uiRender(runtime);uiCapture("settings_city_listening");strcpy(m.liveValue,"--");
    runtime.key(Key::Back,982);CHECK(m.settingsPage==SettingsPage::List);m.settingsSelection=0;m.cityName[0]=0;
    CHECK(runtime.switchTo(App::Home)==Result::Ok);
    settimeofday(&saved,nullptr);
    puts("CASE_PASS voice_add_rate_countdown_stock_city");
}
// 中英翻译: the ready screen, a translation (scrolled when long), a failure, listening.
void translateTests(Runtime& runtime){
    auto& m=runtime.model;auto& t=m.page.translation;
    CHECK(runtime.switchTo(App::Translate)==Result::Ok&&!t.at&&!*t.result);m.livePhase=LivePhase::Ready;strcpy(m.liveState,"云端就绪");
    uiRender(runtime);uiCapture("translate_ready");
    strcpy(t.source,"明天下午三点在会议室开会，记得带电脑。");strcpy(t.result,"Meeting in the conference room at 3 p.m. tomorrow, remember to bring your computer.");
    t.toEnglish=true;t.engine=1;t.at=700;uiRender(runtime);uiCapture("translate_result");
    runtime.key(Key::Down,700);CHECK(m.translateScroll==0);  // it fits: nothing to scroll
    strcpy(t.source,"We had a long meeting this morning to discuss the product plan for next quarter.");
    strcpy(t.result,"今天上午我们开了一个很长的会，讨论了下个季度的产品规划，包括新功能的优先级、上线时间以及市场推广的预算，大家都觉得时间比较紧张，需要尽快确定方案，下周一再开一次会。");
    t.toEnglish=false;t.engine=2;CHECK(uiWrappedLines(t.result)>Model::kTranslateLines);
    runtime.key(Key::Down,701);runtime.key(Key::Down,701);runtime.key(Key::Down,701);
    CHECK(m.translateScroll==uiWrappedLines(t.result)-Model::kTranslateLines);runtime.key(Key::Up,701);CHECK(m.translateScroll+1==uiWrappedLines(t.result)-Model::kTranslateLines);
    uiRender(runtime);uiCapture("translate_long");
    t.engine=0;t.result[0]=0;uiRender(runtime);uiCapture("translate_failed");
    strcpy(m.liveValue,"rec");m.listenAt=701;runtime.key(Key::Down,701);uiRender(runtime);uiCapture("translate_listening");strcpy(m.liveValue,"--");
    runtime.key(Key::Back,702);CHECK(runtime.active()==App::Menu);
    puts("CASE_PASS translate");
}
// 随机转盘: built-in modes, a spin that stops on its pick, mode switching, spoken modes and options, deleting.
void wheelTests(Runtime& runtime){
    auto& m=runtime.model;auto& w=m.page.wheel;
    const auto forget=[]{nvs_handle_t h;CHECK(nvs_open("suite_system",NVS_READWRITE,&h)==ESP_OK);nvs_erase_key(h,"wheel_v1");nvs_commit(h);nvs_close(h);};
    forget();
    CHECK(runtime.switchTo(App::Wheel)==Result::Ok);m.livePhase=LivePhase::Ready;m.lastNotice[0]=0;
    CHECK(w.count==3&&w.current==0&&!strcmp(w.modes[0].name,"吃饭")&&w.modes[0].count==5&&!strcmp(w.modes[0].options[4],"快餐"));
    uiRender(runtime);uiCapture("wheel_modes");
    // Confirm spins; keys wait it out; the highlight ends on the pick.
    runtime.key(Key::Confirm,600);CHECK(m.wheelSpinning()&&m.wheelPick<5&&m.wheelSteps>=20);
    const uint8_t pick=m.wheelPick;runtime.key(Key::Down,600);runtime.key(Key::Confirm,600);CHECK(w.current==0&&m.wheelPick==pick);
    m.wheelSpunAt=esp_timer_get_time()-Model::kWheelSpin/2;CHECK(m.wheelSpinning()&&m.wheelLit()<5);uiRender(runtime);uiCapture("wheel_spinning");
    m.wheelSpunAt=esp_timer_get_time()-Model::kWheelSpin;CHECK(!m.wheelSpinning()&&m.wheelLit()==pick);
    uiRender(runtime);uiCapture("wheel_picked");
    // The next spin starts from the last pick and still lands on its own.
    runtime.key(Key::Confirm,600);CHECK(m.wheelFrom==pick&&(m.wheelFrom+m.wheelSteps)%5==m.wheelPick);m.wheelSpunAt=1;
    // Up/Down switch the mode (kept) and clear the pick.
    runtime.key(Key::Down,601);CHECK(w.current==1&&m.wheelPick==255);
    {WheelBoard stored;CHECK(SettingsStore::loadWheel(stored)&&stored.current==1);}
    runtime.key(Key::Up,602);runtime.key(Key::Up,602);CHECK(w.current==2);
    // Spoken: a name and options make a mode (or replace that mode's options); one or two items join the mode on show.
    CHECK(runtime.addWheel("周末去哪，爬山、看电影、逛街。")&&w.count==4&&w.current==3&&!strcmp(w.modes[3].name,"周末去哪")&&w.modes[3].count==3&&!strcmp(w.modes[3].options[2],"逛街"));
    CHECK(runtime.addWheel("露营")&&w.modes[3].count==4&&strstr(m.lastNotice,"已加到"));
    CHECK(!runtime.addWheel("爬山。")&&strstr(m.lastNotice,"已经有"));
    // 有 after the name splits it off even when the cloud adds no comma there; a name alone with 有 is not a mode.
    CHECK(runtime.addWheel("夜宵有烧烤，麻辣烫。")&&w.count==5&&!strcmp(w.modes[4].name,"夜宵")&&w.modes[4].count==2&&!strcmp(w.modes[4].options[0],"烧烤"));
    CHECK(runtime.longKey(false,602)&&runtime.longKey(false,602)&&w.count==4);
    CHECK(runtime.addWheel("夜宵有烧烤。")&&w.count==5&&w.modes[4].count==1&&!strcmp(w.modes[4].name,"夜宵"));
    CHECK(runtime.longKey(false,602)&&runtime.longKey(false,602)&&w.count==4);
    CHECK(runtime.addWheel("吃饭 火锅 烤肉 火锅")&&w.count==4&&w.current==0&&w.modes[0].count==2&&!strcmp(w.modes[0].options[1],"烤肉"));
    const WheelBoard before=w;
    CHECK(!runtime.addWheel("吃饭，这个选项名字实在太长了")&&strstr(m.lastNotice,"8 个字")&&!memcmp(&before,&w,sizeof(w)));
    CHECK(!runtime.addWheel("，。")&&strstr(m.lastNotice,"没有听清"));
    CHECK(!runtime.addWheel("数字，1，2，3，4，5，6，7，8，9")&&strstr(m.lastNotice,"8 个选项")&&!memcmp(&before,&w,sizeof(w)));
    CHECK(runtime.addWheel("数字，1，2，3，4，5，6，7，8")&&w.count==5&&w.modes[4].count==8);
    CHECK(!runtime.addWheel("9")&&strstr(m.lastNotice,"已满"));
    CHECK(!runtime.addWheel("颜色，红，绿，蓝")&&strstr(m.lastNotice,"最多 5 个模式"));
    {WheelBoard stored;CHECK(SettingsStore::loadWheel(stored)&&stored.count==5&&!strcmp(stored.modes[4].options[7],"8"));}
    uiRender(runtime);uiCapture("wheel_eight");
    // A long Down asks, a second deletes the mode on show; any other key cancels.
    runtime.key(Key::Up,603);runtime.key(Key::Up,603);runtime.key(Key::Up,603);CHECK(w.current==1);
    CHECK(runtime.longKey(false,603)&&m.wheelArmed&&w.count==5);uiRender(runtime);uiCapture("wheel_delete");
    runtime.key(Key::Down,603);CHECK(!m.wheelArmed&&w.current==2);runtime.key(Key::Up,603);
    CHECK(runtime.longKey(false,604)&&runtime.longKey(false,604)&&w.count==4&&w.current==1&&!strcmp(w.modes[1].name,"掷骰子")&&strstr(m.lastNotice,"已删除"));
    {WheelBoard stored;CHECK(SettingsStore::loadWheel(stored)&&stored.count==4&&stored.valid());}
    // A mode left with one option does not spin.
    w.modes[1].count=1;runtime.key(Key::Confirm,604);CHECK(!m.wheelSpinning()&&strstr(m.lastNotice,"2 个选项"));
    strcpy(m.liveValue,"rec");m.listenAt=604;uiRender(runtime);uiCapture("wheel_listening");strcpy(m.liveValue,"--");
    runtime.key(Key::Back,605);CHECK(runtime.active()==App::Menu);
    forget();
    puts("CASE_PASS wheel");
}
void statusTests(Runtime& runtime){
    auto& m=runtime.model;
    CHECK(runtime.switchTo(App::Status)==Result::Ok);
    // Labels typed on the computer come first; the presets follow.
    m.status={};strcpy(m.status.custom[1],"出差中周五回");
    CHECK(m.statusCount()==std::size(kStatusLabels)+1);
    CHECK(!strcmp(m.statusLabel(0).text,"出差中周五回")&&!strcmp(m.statusLabel(1).text,kStatusLabels[0].text));
    runtime.key(Key::Up,400);CHECK(m.status.index==m.statusCount()-1);
    runtime.key(Key::Down,401);runtime.key(Key::Down,402);CHECK(m.status.index==1);
    // Lit by default; a long Up/Down hands the screen back to the idle timer, and both survive a restart.
    // Confirm belongs to the spoken label (LiveServices) and changes nothing here.
    CHECK(runtime.setIdleMinutes(6));runtime.connection(false,402);
    CHECK(runtime.power(402+999999)==Power::Awake);
    runtime.key(Key::Confirm,403);CHECK(m.status.awake&&m.status.index==1);
    CHECK(runtime.longKey(true,403));CHECK(!m.status.awake&&runtime.power(403+999999)==Power::DeepSleepIntent);
    StatusBoard saved;CHECK(SettingsStore::loadStatus(saved)&&saved.index==1&&!saved.awake&&!strcmp(saved.custom[1],"出差中周五回"));
    CHECK(runtime.longKey(false,404));CHECK(m.status.awake);runtime.connection(true,404);
    // A spoken label: closing punctuation dropped, shown at once, newest first; five at most, the oldest goes;
    // saying one again moves it to the front; too long or nothing heard changes nothing.
    {
        m.status={};
        CHECK(runtime.addStatus(" 会议中，请勿打扰。")&&!strcmp(m.status.custom[0],"会议中，请勿打扰")&&m.status.index==0);
        CHECK(!strcmp(m.statusLabel(0).text,"会议中，请勿打扰"));
        for(const char* t:{"午休","外出","专注写代码!","接电话"})CHECK(runtime.addStatus(t));
        CHECK(!strcmp(m.status.custom[0],"接电话")&&!strcmp(m.status.custom[1],"专注写代码")&&!strcmp(m.status.custom[4],"会议中，请勿打扰"));
        CHECK(runtime.addStatus("出差"));CHECK(!strcmp(m.status.custom[0],"出差")&&!strcmp(m.status.custom[4],"午休"));
        CHECK(runtime.addStatus("外出。"));CHECK(!strcmp(m.status.custom[0],"外出")&&!strcmp(m.status.custom[1],"出差")&&!strcmp(m.status.custom[4],"午休"));
        CHECK(m.statusCount()==std::size(kStatusLabels)+5);
        const StatusBoard before=m.status;
        CHECK(!runtime.addStatus("这句话实在是太长了根本放不进状态牌里面")&&strstr(m.lastNotice,"太长")&&!memcmp(&before,&m.status,sizeof(before)));
        CHECK(!runtime.addStatus("。")&&strstr(m.lastNotice,"没有听清")&&!memcmp(&before,&m.status,sizeof(before)));
        runtime.key(Key::Down,404);CHECK(!*m.lastNotice&&m.status.index==1);
        StatusBoard stored;CHECK(SettingsStore::loadStatus(stored)&&!strcmp(stored.custom[0],"外出"));
        // The longest that fits (12 characters) on the sign, then the listening screen over it.
        CHECK(!runtime.addStatus("请勿打扰正在开会稍后回电话"));CHECK(runtime.addStatus("请勿打扰正在开会稍后回电")&&!*m.lastNotice);uiRender(runtime);uiCapture("status_spoken");
        strcpy(m.liveValue,"rec");m.listenAt=404;uiRender(runtime);uiCapture("status_listening");strcpy(m.liveValue,"--");
        // A board saved with four labels (before 0.4.8) keeps them, with the fifth empty.
        uint8_t old[kStatusV1Size]{StatusBoard::kVersion,2,1};strcpy(reinterpret_cast<char*>(old+3+40),"旧标签");
        nvs_handle_t h;CHECK(nvs_open("suite_system",NVS_READWRITE,&h)==ESP_OK&&nvs_set_blob(h,"status_v1",old,sizeof(old))==ESP_OK&&nvs_commit(h)==ESP_OK);nvs_close(h);
        CHECK(SettingsStore::loadStatus(stored)&&stored.index==2&&!*stored.custom[0]&&!strcmp(stored.custom[1],"旧标签")&&!*stored.custom[4]);
        m.status=saved;
    }
    // Removing a label keeps the shown index in range.
    m.status.index=std::size(kStatusLabels);SettingsStore::saveStatus(m.status);
    StatusBoard fewer=m.status;fewer.custom[1][0]=0;SettingsStore::saveStatus(fewer);
    runtime.reloadStatus();CHECK(m.status.index==0);
    m.status=saved;m.status.index=0;m.status.awake=1;uiRender(runtime);uiCapture("status_custom");
    m.status={};CHECK(SettingsStore::saveStatus(m.status));
    runtime.key(Key::Back,405);CHECK(runtime.active()==App::Menu);
    puts("CASE_PASS status_sign");
}
void toolPagesTests(Runtime& runtime){
    auto& m=runtime.model;
    // Calendar data (checked day by day against lunar_python by scripts/test_lunar_calendar.py).
    LunarDate l{};CHECK(toLunar({2026,10,24},l)&&l.month==9&&l.day==15&&!l.leap);
    char stem[7];ganzhi(l.year,stem);CHECK(!strcmp(stem,"丙午")&&!strcmp(zodiac(l.year),"马"));
    CHECK(toLunar({2026,2,17},l)&&!strcmp(festival({2026,2,17},l),"春节")&&holiday({2026,10,1})==1);
    Date d{};CHECK(fromLunar(2026,8,15,d)&&d.month==9&&d.day==25);
    // 倒数日 on a Wednesday at 17:00: 下班 leads with an hour left, 周末 is three days off, a past one-off event goes last.
    m.countdown={};strcpy(m.countdown.events[0].name,"纪念日");m.countdown.events[0].date=20200520;
    CountdownItem items[20];const unsigned n=m.countdownItems({2026,10,21},17*60,items,20);
    CHECK(n==13&&!strcmp(items[0].name,"下班")&&items[0].minutes==60&&!strcmp(items[1].name,"周末")&&items[1].days==3);
    CHECK(!strcmp(items[n-1].name,"纪念日")&&items[n-1].days<0);
    // QR: stored entries first, then WiFi built from the settings.
    QrEntry entry{};for(unsigned i=0;i<QrEntry::kCount;++i)CHECK(SettingsStore::saveQr(i,entry));
    CHECK(runtime.switchTo(App::Qr)==Result::Ok&&runtime.qrCount()==1&&!strncmp(m.qrText,"WIFI:T:WPA;S:",13));
    strcpy(entry.title,"我的博客");strcpy(entry.text,"https://example.com/passport");CHECK(SettingsStore::saveQr(1,entry));
    runtime.reloadQr();CHECK(runtime.qrCount()==2&&!strcmp(m.qrText,"https://example.com/passport"));
    uiRender(runtime);uiCapture("qr_custom");
    runtime.key(Key::Down,500);CHECK(m.qrIndex==1&&!strncmp(m.qrText,"WIFI:",5));
    runtime.key(Key::Confirm,501);CHECK(m.qrBright);
    entry={};CHECK(SettingsStore::saveQr(1,entry));
    // Torch: 20% steps, three colours, never sleeps.
    CHECK(runtime.switchTo(App::Torch)==Result::Ok);m.torchLevel=5;m.torchColor=0;
    runtime.key(Key::Up,502);CHECK(m.torchLevel==5);runtime.key(Key::Down,503);CHECK(m.torchLevel==4);
    runtime.key(Key::Confirm,504);CHECK(m.torchColor==1);
    runtime.connection(false,504);CHECK(runtime.power(504+999999)==Power::Awake);runtime.connection(true,505);
    // Calendar: Up/Down step a day across a year end, long presses a month; Confirm opens the day's 黄历.
    CHECK(runtime.switchTo(App::Calendar)==Result::Ok);m.calendarYear=2026;m.calendarMonth=1;m.calendarDay=1;
    runtime.key(Key::Up,506);CHECK(m.calendarYear==2025&&m.calendarMonth==12&&m.calendarDay==31);
    CHECK(runtime.longKey(false,507)&&m.calendarYear==2026&&m.calendarMonth==1&&m.calendarDay==31);
    CHECK(runtime.longKey(false,507)&&m.calendarMonth==2&&m.calendarDay==28);
    m.calendarMonth=10;m.calendarDay=24;uiRender(runtime);uiCapture("calendar_month");
    Almanac almanacDay{};char dayName[7];CHECK(almanac({2026,10,24},almanacDay));ganzhiName(almanacDay.day,dayName);
    CHECK(!strcmp(dayName,"辛未")&&!strcmp(almanacWord(almanacDay.yi[0]),"祭祀")&&!strcmp(almanacDay.sha,"西"));
    runtime.key(Key::Confirm,507);CHECK(m.calendarView==1);uiRender(runtime);uiCapture("calendar_day");
    // A second Confirm shows the day's 月相 and 节气 (2026-10-25: the night before a full moon, in 霜降), a third the month.
    runtime.key(Key::Down,507);CHECK(m.calendarDay==25&&m.calendarView==1);runtime.key(Key::Confirm,507);CHECK(m.calendarView==2);
    uiRender(runtime);uiCapture("calendar_moon");
    runtime.key(Key::Confirm,507);CHECK(m.calendarView==0);
    // 播客: Up/Down pick a program, Confirm opens its player (where Up/Down set the volume), long Confirm goes
    // back to the list and then to the menu.
    CHECK(runtime.switchTo(App::Podcast)==Result::Ok&&!m.podcastPlayer);
    m.podcastCount=3;m.podcastIndex=0;strcpy(m.page.podcasts[0].name,"声动早咖啡");strcpy(m.page.podcasts[1].name,"日谈公园");strcpy(m.page.podcasts[2].name,"BBC 随身英语");
    for(auto& row:m.page.podcasts)strcpy(row.note,"从最新一期开始");
    runtime.key(Key::Up,507);CHECK(m.podcastIndex==2);runtime.key(Key::Down,507);runtime.key(Key::Down,507);CHECK(m.podcastIndex==1);
    uiRender(runtime);uiCapture("podcast_list");
    {const auto volume=m.volume;runtime.key(Key::Confirm,507);CHECK(m.podcastPlayer&&m.podcastIndex==1);
     runtime.key(Key::Down,507);CHECK(m.volume==volume-5&&m.podcastIndex==1);runtime.key(Key::Up,507);CHECK(m.volume==volume);}
    m.podcastPos=312;m.podcastDur=668;uiRender(runtime);uiCapture("podcast_player");
    runtime.key(Key::Back,507);CHECK(runtime.active()==App::Podcast&&!m.podcastPlayer);
    runtime.key(Key::Back,507);CHECK(runtime.active()==App::Menu);
    // Menu: long Up/Down turn a page and land on its first entry.
    CHECK(runtime.switchTo(App::Menu)==Result::Ok);
    {const unsigned pages=std::size(kMenuPages),page=menuPageOf(runtime.selection());
     CHECK(runtime.longKey(false,507)&&runtime.selection()==kMenuStarts[(page+1)%pages]);
     CHECK(runtime.longKey(true,507)&&runtime.selection()==kMenuStarts[page]);}
    for(unsigned p=0;p<std::size(kMenuPages);++p)CHECK(kMenuStarts[p+1]-kMenuStarts[p]<=9&&menuPageOf(kMenuStarts[p])==p);
    // 秒表: Confirm runs and pauses, Up records a lap while running, Down resets once paused; it holds off deep sleep.
    CHECK(runtime.switchTo(App::Stopwatch)==Result::Ok);m.swRunning=false;m.swBase=0;m.swLapCount=0;
    runtime.key(Key::Confirm,507);CHECK(m.swRunning);vTaskDelay(pdMS_TO_TICKS(60));
    runtime.key(Key::Up,507);vTaskDelay(pdMS_TO_TICKS(30));runtime.key(Key::Up,507);CHECK(m.swLapCount==2&&m.swLaps[0]>m.swLaps[1]&&m.swLaps[1]>=50000);
    runtime.connection(false,507);CHECK(runtime.power(507+999999)==Power::ScreenOff);runtime.connection(true,507);
    runtime.key(Key::Down,507);CHECK(m.swLapCount==2);  // no reset while running
    runtime.key(Key::Confirm,507);CHECK(!m.swRunning&&m.stopwatchUs()>=80000);uiRender(runtime);uiCapture("stopwatch_laps");
    runtime.key(Key::Down,507);CHECK(m.stopwatchUs()==0&&m.swLapCount==0);
    // Sonar: named devices strongest first, focus follows an address, unnamed filter, reset.
    CHECK(runtime.switchTo(App::Sonar)==Result::Ok);
    m.sonarCount=3;m.sonar[0]={{1,2,3,4,5,6},-70,0x004c,0,""};m.sonar[1]={{9,8,7,6,5,4},-50,0,0,"Kevin 的耳机"};m.sonar[2]={{7,7,7,7,7,7},-80,0,0,"Mi Band"};
    uint8_t order[Model::kSonarSlots];CHECK(m.sonarVisible(order)==2&&order[0]==1);
    runtime.key(Key::Down,508);CHECK(!memcmp(m.sonarFocus,m.sonar[1].addr,6));
    runtime.key(Key::Down,509);CHECK(!memcmp(m.sonarFocus,m.sonar[2].addr,6));
    m.sonarPeerRssi=-45;uiRender(runtime);uiCapture("sonar_radar");
    runtime.key(Key::Confirm,510);CHECK(m.sonarList);uiRender(runtime);uiCapture("sonar_list");
    CHECK(runtime.longKey(true,511)&&!m.sonarNamed&&m.sonarVisible(order)==1);
    CHECK(runtime.longKey(false,512)&&m.sonarNamed&&m.sonarCount==3);
    m.sonarNamed=true;m.sonarList=false;m.sonarPeerRssi=0;
    // News: Up/Down through the fetched headlines.
    CHECK(runtime.switchTo(App::News)==Result::Ok);
    m.newsCount=2;strcpy(m.page.news.items[0].time,"22:16");strcpy(m.page.news.items[0].title,"央行开展逆回购操作 净投放资金");m.page.news.textFor=0;strcpy(m.page.news.text,"今日央行以利率招标方式开展了逆回购操作，市场流动性保持合理充裕。");
    strcpy(m.page.news.items[1].time,"22:05");strcpy(m.page.news.items[1].title,"周六重大事件汇总");strcpy(m.liveLines[0],"沪 3690 +0.89%  深 12346 +1.02%");
    // Past the last headline the next (older) page is asked for; Up at the first headline of page 1 stays.
    runtime.key(Key::Down,513);CHECK(m.newsIndex==1);runtime.key(Key::Down,514);CHECK(m.newsIndex==1&&m.newsWant==2);
    m.newsWant=1;m.newsIndex=0;runtime.key(Key::Up,514);CHECK(m.newsIndex==0&&m.newsWant==1);
    uiRender(runtime);uiCapture("news_items");
    // A long text: Down scrolls it a screen at a time (the headline gives way), then steps on; Up scrolls back.
    strcpy(m.page.news.items[0].title,"逸飞激光：拟4亿元收购光模块封装与自动化耦合设备公司兴启航100%股权");
    strcpy(m.page.news.text,"逸飞激光(688646.SH)公告称，公司拟以现金4亿元收购深圳市兴启航自动化设备有限公司100%股权。兴启航深耕光模块封装与自动化耦合设备领域十余载，产品覆盖TO-CAN、COB、BOX（Lens）、FA光纤阵列等全品类耦合设备，覆盖激光焊接、UV紫外胶固化、热固化等全工艺路径，并向CPO共封装设备、1.6T以上高速耦合设备等前沿领域延伸，为全球头部光模块客户提供专业解决方案。");
    {const unsigned lines=uiTextLines(m.page.news.text,200);CHECK(lines>Model::kNewsLines+Model::kNewsMoreLines);
    runtime.key(Key::Down,515);CHECK(m.newsIndex==0&&m.newsScroll==Model::kNewsLines-1);
    uiRender(runtime);uiCapture("news_scroll");
    for(unsigned i=0;i<10&&m.newsIndex==0;++i)runtime.key(Key::Down,516+i);
    CHECK(m.newsIndex==1&&m.newsScroll==0);runtime.key(Key::Up,530);CHECK(m.newsIndex==0&&m.newsScroll==0);
    m.newsScroll=Model::kNewsLines-1+2*(Model::kNewsMoreLines-1);runtime.key(Key::Up,531);CHECK(m.newsScroll==Model::kNewsLines-1+Model::kNewsMoreLines-1);
    runtime.key(Key::Up,532);CHECK(m.newsScroll==Model::kNewsLines-1);runtime.key(Key::Up,533);CHECK(m.newsScroll==0&&m.newsIndex==0);
    // Text not here yet: Down steps on rather than scroll.
    m.page.news.textFor=1;runtime.key(Key::Down,534);CHECK(m.newsIndex==1&&m.newsScroll==0);(void)lines;}
    m.newsCount=0;m.liveLines[0][0]=0;
    // 汇率: rows priced in the base currency; Up wraps to the last row.
    CHECK(runtime.switchTo(App::Rate)==Result::Ok);strcpy(m.rateBase,"CNY");m.rateCount=3;m.rateUpdated=1792800151;
    m.rates[0]={"USD",0.14992};m.rates[1]={"JPY",23.508};m.rates[2]={"HKD",1.1759};
    runtime.key(Key::Up,515);CHECK(m.rateIndex==2);uiRender(runtime);uiCapture("rate_items");m.rateCount=0;
    // 历史今日: Up wraps to the last event.
    CHECK(runtime.switchTo(App::History)==Result::Ok);m.eventCount=4;
    m.page.events[0]={1860,0,"清政府与英国在北京签订《中英北京条约》"};m.page.events[1]={1898,1,"中国军事家彭德怀出生"};m.page.events[2]={1601,2,"丹麦天文学家和占星学家第谷·布拉赫逝世"};m.page.events[3]={1901,0,"伊斯曼和柯达公司联合生产柯达相机"};
    runtime.key(Key::Down,516);CHECK(m.eventIndex==1);runtime.key(Key::Up,516);runtime.key(Key::Up,516);CHECK(m.eventIndex==3);
    m.eventIndex=0;uiRender(runtime);uiCapture("history_items");m.eventCount=0;
    // 每日一句: entry clears the sentence; Up asks for the day before, Down never goes past today.
    CHECK(runtime.switchTo(App::Daily)==Result::Ok&&!*m.page.daily.en);runtime.key(Key::Up,517);CHECK(m.dailyWant==0);
    strcpy(m.page.daily.en,"Grow through what you go through.");strcpy(m.page.daily.cn,"穿越困境，得以成长。");strcpy(m.page.daily.date,"2026-10-24");
    runtime.key(Key::Down,517);CHECK(m.dailyWant==0);runtime.key(Key::Up,517);CHECK(m.dailyWant==1);
    m.dailyWant=m.dailyBack=2;runtime.key(Key::Down,517);CHECK(m.dailyWant==1);m.dailyWant=2;
    uiRender(runtime);uiCapture("daily_items");m.page.daily.en[0]=0;m.dailyBack=m.dailyWant=0;
    const auto phase=m.livePhase;m.livePhase=LivePhase::Ready;
    // 地震速报: Up wraps to the oldest quake, Down back to the newest.
    CHECK(runtime.switchTo(App::Quake)==Result::Ok&&m.quakeIndex==0);m.quakeCount=5;
    m.page.quakes[0]={1792892265,"西藏那曲市巴青县",32,10};m.page.quakes[1]={1792891891,"青海玉树州杂多县",30,10};m.page.quakes[2]={1792880000,"瓦努阿图群岛",61,30};
    m.page.quakes[3]={1792860000,"新疆喀什地区伽师县",45,15};m.page.quakes[4]={1792800000,"台湾花莲县海域",52,20};
    runtime.key(Key::Up,518);CHECK(m.quakeIndex==4);runtime.key(Key::Down,518);CHECK(m.quakeIndex==0);
    uiRender(runtime);uiCapture("quake_items");m.quakeCount=0;
    // 知识问答: Up/Down pick, Confirm reveals and scores, picking stops once revealed, Confirm again asks for the next.
    CHECK(runtime.switchTo(App::Quiz)==Result::Ok&&!m.quizCount);
    strcpy(m.page.quiz.question,"Which continent is considered the largest and most populous continent in the world?");strcpy(m.page.quiz.category,"Geography");strcpy(m.page.quiz.level,"easy");
    {const char* answers[]{"Africa","Asia","Europe","North America"};for(unsigned i=0;i<4;++i)strcpy(m.page.quiz.answers[i],answers[i]);}
    m.quizCount=4;m.quizCorrect=1;m.quizSerial=1;
    runtime.key(Key::Up,519);CHECK(m.quizPick==3);runtime.key(Key::Down,519);runtime.key(Key::Down,519);CHECK(m.quizPick==1);
    uiRender(runtime);uiCapture("quiz_question");
    runtime.key(Key::Confirm,519);CHECK(m.quizShown&&m.quizRight==1&&m.quizAsked==1&&!m.quizWant);
    runtime.key(Key::Down,519);CHECK(m.quizPick==1);
    m.quizShown=false;m.quizPick=3;runtime.key(Key::Confirm,519);CHECK(m.quizRight==1&&m.quizAsked==2);uiRender(runtime);uiCapture("quiz_answered");
    runtime.key(Key::Confirm,519);CHECK(m.quizWant);m.quizCount=0;m.quizWant=false;
    // Hacker News: Up wraps to the last story.
    CHECK(runtime.switchTo(App::Hacker)==Result::Ok);m.storyCount=3;
    m.page.stories[0]={"Local Wikipedia: Wikipedia, but only for things a half hour walk from you","edjefferson.com",1551,243,1792710078};
    m.page.stories[1]={"No signs of AI payoff outside the tech sector","apollo.com",482,481,1792871239};
    m.page.stories[2]={"Show HN: A tiny ESP32 badge with twenty-six tools","github.com",120,35,1792873831};
    runtime.key(Key::Up,520);CHECK(m.storyIndex==2);runtime.key(Key::Down,520);CHECK(m.storyIndex==0);
    uiRender(runtime);uiCapture("hacker_items");m.storyCount=0;
    // 台风路径: Up/Down wrap through the season; a track shows only for the row it belongs to.
    CHECK(runtime.switchTo(App::Typhoon)==Result::Ok&&!m.page.typhoon.count&&m.typhoonIndex==0);
    {auto& t=m.page.typhoon;t.count=3;t.rows[0]={"2631","烟花",true};t.rows[1]={"2630","蔷琵",false};t.rows[2]={"","热带低压",false};
     runtime.key(Key::Up,522);CHECK(m.typhoonIndex==2);runtime.key(Key::Down,522);CHECK(m.typhoonIndex==0);
     // IN-FA on 2026-10-26 (中央气象台): east of Guam heading west, forecast to turn north-west.
     const int16_t track[][3]{{1667,104,1},{1664,104,1},{1660,106,1},{1656,108,2},{1652,111,2},{1648,115,2},{1645,120,3},{1641,126,3},{1638,132,3},{1636,135,4},{1634,140,4},{1632,140,4}};
     t.points=0;for(auto& p:track)t.track[t.points++]={p[0],p[1],uint8_t(p[2])};
     const int16_t ahead[][4]{{1628,141,5,12},{1623,142,5,24},{1613,143,5,36},{1596,145,5,48},{1571,149,5,60},{1540,153,5,72},{1466,170,5,96},{1416,203,5,120}};
     t.ahead=0;for(auto& a:ahead)t.forecast[t.ahead++]={a[0],a[1],uint8_t(a[2]),uint8_t(a[3])};
     t.at=1792983600;t.pressure=950;t.lowest=950;t.wind=45;t.speed=7;t.peakWind=45;t.peakGrade=4;strcpy(t.heading,"WNW");
     strcpy(t.city,"杭州");t.cityLon=1202;t.cityLat=303;t.shown=0;}
    uiRender(runtime);uiCapture("typhoon_items");
    // An ended one: its peak instead of now; the track from the South China Sea into Guangdong.
    {auto& t=m.page.typhoon;m.typhoonIndex=1;t.shown=1;
     const int16_t track[][3]{{1245,150,0},{1238,155,1},{1230,160,1},{1221,166,2},{1212,172,3},{1203,178,4},{1195,184,5},{1186,190,5},{1177,196,4},{1170,203,3},{1162,210,3},{1155,218,2},{1148,224,1},{1140,229,0}};
     t.points=0;for(auto& p:track)t.track[t.points++]={p[0],p[1],uint8_t(p[2])};
     t.ahead=0;t.at=1792400000;t.pressure=998;t.lowest=925;t.wind=15;t.speed=0;t.peakWind=58;t.peakGrade=5;t.heading[0]=0;}
    uiRender(runtime);uiCapture("typhoon_stopped");
    m.typhoonIndex=2;uiRender(runtime);CHECK(m.page.typhoon.shown!=m.typhoonIndex);m.page.typhoon.count=0;
    // 百度热榜: Up/Down move through the board; Confirm opens the picked item and closes it, Back leaves the detail first.
    CHECK(runtime.switchTo(App::Hot)==Result::Ok&&!m.hotCount&&!m.hotDetail&&m.page.hot.detailFor==255);
    {const char* titles[]{"二十届五中全会在北京开始举行","日本前首相福田康夫去世","去不去养老院 老人子女谁说了算","中国发展机遇持续与世界共享","蜜雪冰城2小时赚够香飘飘3个月利润",
                          "张家界景区播西游记硬控游客","美国客户一口气在广州买200套房","周星驰《功夫女足》烂番茄开分","全国大部气温将创新低","多地幼儿园改为养老院"};
     const uint8_t tags[]{kHotPinned,1,0,0,3,0,1,0,0,0};
     for(unsigned i=0;i<10;++i){auto& r=m.page.hot.rows[i];fitText(r.title,sizeof(r.title),titles[i],strlen(titles[i]));r.score=7904061-i*95321;r.tag=tags[i];}
     m.hotCount=10;m.hotUpdated=1792999800;}
    runtime.key(Key::Up,523);CHECK(m.hotIndex==9);runtime.key(Key::Down,523);runtime.key(Key::Down,523);runtime.key(Key::Down,523);CHECK(m.hotIndex==2);
    uiRender(runtime);uiCapture("hot_items");
    runtime.key(Key::Confirm,523);CHECK(m.hotDetail);uiRender(runtime);uiCapture("hot_loading");
    {auto& h=m.page.hot;h.detailFor=2;strcpy(h.title,"去不去养老院 老人子女谁说了算");
     strcpy(h.desc,"近日，重庆90多岁的苏大爷将6名子女告上法庭，要求每人每月支付700元赡养费送自己去养老院并定期探望。此前苏大爷随长子生活近十年，因年事渐高照料吃力。");}
    uiRender(runtime);uiCapture("hot_detail");
    runtime.key(Key::Down,523);CHECK(m.hotIndex==3&&m.hotDetail);runtime.key(Key::Confirm,523);CHECK(!m.hotDetail);
    runtime.key(Key::Confirm,523);runtime.key(Key::Back,523);CHECK(runtime.active()==App::Hot&&!m.hotDetail);
    m.hotCount=0;
    // 语音笔记: the cloud's text is saved newest first (addMemo); row 0 records (LiveServices), Confirm opens a note and
    // Up/Down scroll it, a long Down twice deletes it. While recording the list keys do nothing.
    CHECK(runtime.switchTo(App::Memo)==Result::Ok&&m.memoMounted);
    while(MemoStore::count())CHECK(MemoStore::remove(0));
    runtime.reloadMemo();CHECK(!m.memoCount&&!m.memoIndex);
    m.livePhase=LivePhase::Ready;strcpy(m.liveState,"云端就绪");m.liveValue[0]=0;
    uiRender(runtime);uiCapture("memo_empty");
    {// Too long for a note: cut to whole characters.
     std::string many="a";for(unsigned i=0;i<400;++i)many+="字";  // byte 1023 falls inside a character
     CHECK(runtime.addMemo(many.c_str(),531)&&m.memoCount==1);MemoNote note;CHECK(MemoStore::load(0,note)&&strlen(note.text)==1021&&m.page.memo.rows[0].chars==341);
     CHECK(MemoStore::remove(0));runtime.reloadMemo();CHECK(!m.memoCount);}
    const char* notes[]{
        "今天的会议纪要如下。第一，产品团队决定把新版本的发布时间推迟到下个月15号，因为测试还发现了几个严重的问题需要修复。第二，市场部希望在发布之前准备好宣传视频和三篇推广文章，由小王负责协调。第三，财务提醒大家本季度的差旅预算已经用了80%，请控制出差次数。第四，下次例会改到周四下午2点，地点在三楼的大会议室。最后，请每个人在周三之前把自己的周报发到群里。",
        "给妈妈买生日礼物，预算500块。",
        "第一件事，周五之前把季度报告发给财务。第二件事，下周一上午约牙医复查。",
        "灵感：胸牌的语音笔记可以按日期分组，周末汇总成一份周报。",
        "明天下午3点和设计团队开会，记得带上新版的原型图。"};
    for(auto* text:notes)CHECK(runtime.addMemo(text,531));
    CHECK(m.memoCount==5&&m.memoIndex==0&&!m.memoTop&&MemoStore::count()==5&&m.memoSavedAt==531);
    {MemoNote note;CHECK(MemoStore::load(0,note)&&!strcmp(note.text,notes[4])&&MemoStore::load(4,note)&&!strcmp(note.text,notes[0]));}
    runtime.key(Key::Up,531);CHECK(m.memoIndex==5&&m.memoTop==1);
    runtime.key(Key::Down,531);CHECK(m.memoIndex==0&&m.memoTop==0);runtime.key(Key::Down,531);runtime.key(Key::Down,531);CHECK(m.memoIndex==2);
    m.memoSavedAt=0;for(unsigned i=0;i<Model::kMemoRows;++i)m.page.memo.rows[i].at=1793000000-i*86400/3;
    uiRender(runtime);uiCapture("memo_items");
    m.memoSavedAt=531;m.memoIndex=0;uiRender(runtime);uiCapture("memo_saved");m.memoIndex=2;m.memoSavedAt=0;
    // Recording: the list keys wait; the screen shows the time listened, then that the cloud is recognising.
    strcpy(m.liveValue,"rec");{const uint64_t now=esp_timer_get_time()/1000000;m.listenAt=now>12?now-12:0;}
    runtime.key(Key::Down,531);runtime.key(Key::Confirm,531);CHECK(m.memoIndex==2&&!m.memoDetail&&m.voiceBusy());
    uiRender(runtime);uiCapture("memo_recording");
    strcpy(m.liveValue,"asr");uiRender(runtime);uiCapture("memo_recognizing");m.liveValue[0]=0;
    // The long note, oldest: Confirm opens it, Down scrolls by a screen less a line, Up back; Back closes it.
    runtime.key(Key::Down,531);runtime.key(Key::Down,531);runtime.key(Key::Down,531);CHECK(m.memoIndex==5&&m.memoTop==1);
    runtime.key(Key::Confirm,531);CHECK(m.memoDetail&&!m.memoScroll&&!strcmp(m.page.memo.note.text,notes[0]));
    m.page.memo.note.at=1792900000;
    {const unsigned lines=uiTextLines(notes[0]);CHECK(lines>Model::kMemoLines&&lines<2*Model::kMemoLines);
     uiRender(runtime);uiCapture("memo_note");
     runtime.key(Key::Down,531);CHECK(m.memoScroll==Model::kMemoLines-1);runtime.key(Key::Down,531);CHECK(m.memoScroll==Model::kMemoLines-1);
     uiRender(runtime);uiCapture("memo_note_scrolled");runtime.key(Key::Up,531);CHECK(!m.memoScroll);}
    runtime.key(Key::Back,531);CHECK(runtime.active()==App::Memo&&!m.memoDetail);
    // Delete: long Up or any key cancels the first long Down; a second long Down deletes and the list closes up.
    runtime.key(Key::Confirm,531);CHECK(m.memoDetail);
    runtime.longKey(false,531);CHECK(m.memoArmed);runtime.key(Key::Down,531);CHECK(!m.memoArmed&&m.memoDetail);
    runtime.longKey(false,531);CHECK(m.memoArmed);uiRender(runtime);uiCapture("memo_delete");
    runtime.longKey(false,531);CHECK(!m.memoArmed&&!m.memoDetail&&m.memoCount==4&&MemoStore::count()==4&&m.memoIndex==4);
    {MemoNote note;CHECK(MemoStore::load(3,note)&&!strcmp(note.text,notes[1]));}
    // From the list too: the picked note; on the 录音 row a long Down does nothing.
    m.memoIndex=0;runtime.longKey(false,531);CHECK(!m.memoArmed);
    runtime.key(Key::Down,531);runtime.longKey(false,531);CHECK(m.memoArmed&&!m.memoDetail);m.page.memo.rows[0].at=1793000000;uiRender(runtime);uiCapture("memo_list_delete");
    runtime.longKey(false,531);CHECK(!m.memoArmed&&m.memoCount==3&&m.memoIndex==1&&MemoStore::count()==3);
    {MemoNote note;CHECK(MemoStore::load(0,note)&&!strcmp(note.text,notes[3]));}
    runtime.key(Key::Back,531);CHECK(runtime.active()==App::Menu&&!m.memoMounted);
    // 天气预报: entry clears the forecast and opens the seven days; Down steps to the next 24 hours, the air and
    // sun, then the warnings (the highest kind colours its tab); Up steps back round.
    CHECK(runtime.switchTo(App::Forecast)==Result::Ok&&!m.page.forecast.days&&!m.forecastView);
    {auto& f=m.page.forecast;strcpy(f.city,"上海");f.days=7;f.hours=24;f.hour0=22;
     const uint8_t codes[]{61,2,3,3,95,1,45},rain[]{80,10,20,15,70,5,255};
     for(unsigned i=0;i<7;++i)f.day[i]={int8_t(19-i),int8_t(22-i/2),codes[i],rain[i]};
     for(unsigned i=0;i<24;++i)f.hour[i]={int8_t(i<9?18-int(i)/3:i<17?int(i)+6:39-int(i)),uint8_t(i<6?61:i<14?3:0),uint8_t(i<6?60-i*8:0)};
     f.airHours=24;for(unsigned i=0;i<24;++i)f.aqi[i]=int16_t(74+int(i)*9-int(i*i)/3);f.primary=0;f.pm25=54;f.pm10=58;f.sunrise=364;f.sunset=1031;f.uv=57;
     f.warnings=0;
     const char* titles[]{"上海市气象台发布大风蓝色预警信号","上海市浦东新区气象台发布大风黄色预警信号","上海市气象台发布大雾黄色预警信号","上海市气象台解除高温橙色预警信号","江苏省南京市气象台发布暴雨红色预警信号"};
     for(auto* t:titles)addWarning(f.warning,f.warnings,std::size(f.warning),t,"2026/10/26 11:02","","上海","");
     std::stable_sort(f.warning,f.warning+f.warnings,[](const WeatherWarning& a,const WeatherWarning& b){return a.level>b.level;});
     CHECK(f.warnings==2&&!strcmp(f.warning[0].type,"大风")&&f.warning[0].level==1&&f.warning[0].places==2);}
    uiRender(runtime);uiCapture("forecast_items");
    runtime.key(Key::Down,521);CHECK(m.forecastView==1);uiRender(runtime);uiCapture("forecast_hourly");
    runtime.key(Key::Down,521);CHECK(m.forecastView==2);uiRender(runtime);uiCapture("forecast_air");
    runtime.key(Key::Down,521);CHECK(m.forecastView==3);uiRender(runtime);uiCapture("forecast_warn");
    // Confirm opens each warning's text in turn (fetched by LiveServices), then the list; Up/Down leave it.
    runtime.key(Key::Confirm,521);CHECK(m.forecastWarn==1);
    {auto& f=m.page.forecast;f.detailFor=0;snprintf(f.detail,sizeof(f.detail),"%s","预计今天后半夜到明天上午，浦东新区、崇明区将有西北风6-7级、阵风8-9级，沿江沿海地区阵风可达9-10级，请有关单位和人员做好防范准备，注意高空坠物和水上作业安全，外出请远离广告牌和临时搭建物。");}
    uiRender(runtime);uiCapture("forecast_warn_text");
    runtime.key(Key::Confirm,521);CHECK(m.forecastWarn==2);runtime.key(Key::Confirm,521);CHECK(!m.forecastWarn);
    runtime.key(Key::Confirm,521);CHECK(m.forecastWarn==1);runtime.key(Key::Up,521);CHECK(!m.forecastWarn&&m.forecastView==2);runtime.key(Key::Down,521);
    runtime.key(Key::Down,521);CHECK(!m.forecastView);runtime.key(Key::Up,521);CHECK(m.forecastView==3);
    runtime.key(Key::Up,521);runtime.key(Key::Up,521);runtime.key(Key::Up,521);CHECK(!m.forecastView);
    m.page.forecast.days=m.page.forecast.hours=m.page.forecast.airHours=m.page.forecast.warnings=0;
    // 白噪音: Up/Down pick a row (the sounds, then the sleep timer); Confirm plays or stops the selected sound or steps
    // the timer. Playing turns only the screen off, and the timer ends it. Long Up/Down set the volume.
    const bool wasConnected=runtime.connected();runtime.connection(false,600);
    CHECK(runtime.switchTo(App::Noise)==Result::Ok&&!m.noisePlaying);m.noiseRow=0;m.noiseTimer=0;m.volume=50;
    runtime.key(Key::Down,600);CHECK(m.noiseRow==1);runtime.key(Key::Confirm,600);CHECK(m.noisePlaying&&m.noiseKind==1&&!m.noiseUntil);
    runtime.key(Key::Up,600);runtime.key(Key::Up,600);CHECK(m.noiseRow==5);
    runtime.key(Key::Confirm,600);CHECK(m.noiseTimer==15&&m.noiseUntil==600+900&&m.noisePlaying);
    CHECK(runtime.power(6000)==Power::ScreenOff);
    // Screenshots are taken plugged in: unplugged, the idle (by the real clock) would cover them.
    runtime.key(Key::Down,601);runtime.key(Key::Down,601);CHECK(m.noiseRow==1);runtime.connection(true,601);uiRender(runtime);uiCapture("noise_playing");
    runtime.tick(1501);CHECK(!m.noisePlaying&&!m.noiseUntil);runtime.connection(false,1501);CHECK(runtime.power(6000)==Power::DeepSleepIntent);runtime.connection(true,1501);
    runtime.key(Key::Up,1502);runtime.key(Key::Confirm,1502);CHECK(m.noisePlaying&&m.noiseKind==0&&m.noiseUntil==1502+900);
    runtime.key(Key::Confirm,1503);CHECK(!m.noisePlaying&&!m.noiseUntil);
    CHECK(runtime.longKey(true,1504)&&m.volume==60);CHECK(runtime.longKey(false,1504)&&m.volume==50);m.noiseTimer=0;
    // 闹钟: the list (the 录音 row, then the alarms); Confirm opens an alarm's clock, where Up/Down move it 5 minutes,
    // long Up/Down an hour and Confirm steps the repeat; long Confirm goes back to the list. At its minute it opens and
    // rings with the screen on; Up/Down snooze 5 minutes (awake meanwhile, so it can ring again), Confirm stops it.
    CHECK(runtime.switchTo(App::Alarm)==Result::Ok&&!m.alarmIndex&&!m.alarmEdit);
    m.alarms={};m.alarms.count=1;m.alarms.items[0]={AlarmPrefs::kVersion,7,30,0};m.alarmSnooze=0;m.alarmFired=0;
    runtime.key(Key::Down,1600);CHECK(m.alarmIndex==1);runtime.key(Key::Confirm,1600);CHECK(m.alarmEdit);
    {auto& a=m.alarms.items[0];
     runtime.key(Key::Up,1600);CHECK(a.hour==7&&a.minute==35);runtime.key(Key::Down,1600);runtime.key(Key::Down,1600);CHECK(a.minute==25);
     CHECK(runtime.longKey(true,1600)&&a.hour==8);CHECK(runtime.longKey(false,1600)&&a.hour==7);runtime.key(Key::Up,1600);
     runtime.key(Key::Confirm,1600);CHECK(a.mode==1);}
    {setenv("TZ","CST-8",1);tzset();timeval at{1792970999,0};settimeofday(&at,nullptr);}  // Monday 07:29:59
    m.clockValid=true;uiRender(runtime);uiCapture("alarm_set");
    runtime.key(Key::Back,1600);CHECK(!m.alarmEdit&&runtime.active()==App::Alarm);
    // Spoken alarms: repeating ones keep the hour said, a one-off rings within 24 hours.
    CHECK(runtime.addAlarm("工作日早上8点15分")&&m.alarms.count==2&&m.alarms.items[1].hour==8&&m.alarms.items[1].minute==15&&m.alarms.items[1].mode==2&&m.alarmIndex==2);
    CHECK(runtime.addAlarm("今晚10点")&&m.alarms.count==3&&m.alarms.items[2].hour==22&&m.alarms.items[2].mode==3&&strstr(m.lastNotice,"响一次"));
    CHECK(runtime.addAlarm("每天晚上10点")&&m.alarms.count==3&&m.alarms.items[2].mode==1);
    CHECK(!runtime.addAlarm("起床")&&strstr(m.lastNotice,"没听到时间")&&m.alarms.count==3);
    {AlarmList saved;CHECK(SettingsStore::loadAlarms(saved)&&saved.count==3&&saved.items[2].hour==22);}
    m.lastNotice[0]=0;m.alarmIndex=1;uiRender(runtime);uiCapture("alarm_list");
    m.alarmIndex=3;CHECK(runtime.longKey(false,1600)&&m.alarmArmed);uiRender(runtime);uiCapture("alarm_delete");
    CHECK(runtime.longKey(false,1600)&&!m.alarmArmed&&m.alarms.count==2&&m.alarmIndex==2);
    m.lastNotice[0]=0;m.alarmIndex=0;strcpy(m.liveValue,"rec");m.listenAt=1600;uiRender(runtime);uiCapture("alarm_listening");strcpy(m.liveValue,"--");
    // The clock ran on while the screens above were captured.
    {timeval at{1792970999,0};settimeofday(&at,nullptr);}
    CHECK(runtime.switchTo(App::Home)==Result::Ok);runtime.tick(1601);CHECK(runtime.active()==App::Home&&!m.alarmRinging);
    {timeval at{1792971001,0};settimeofday(&at,nullptr);}
    runtime.tick(1602);CHECK(runtime.active()==App::Alarm&&m.alarmRinging&&m.alarmFired==1792971000);
    runtime.connection(false,1602);CHECK(runtime.power(9000)==Power::Awake);runtime.connection(true,1602);uiRender(runtime);uiCapture("alarm_ringing");
    runtime.key(Key::Up,1603);CHECK(!m.alarmRinging&&m.alarmSnooze>=1792971301&&m.alarmSnooze<=1792971305&&m.confirmTaken);
    runtime.connection(false,1603);CHECK(runtime.power(9000)==Power::ScreenOff);runtime.connection(true,1603);
    uiRender(runtime);uiCapture("alarm_snoozed");
    runtime.tick(1604);CHECK(!m.alarmRinging);
    {timeval at{1792971310,0};settimeofday(&at,nullptr);}
    runtime.tick(1605);CHECK(m.alarmRinging&&!m.alarmSnooze);runtime.key(Key::Confirm,1606);CHECK(!m.alarmRinging&&m.confirmTaken);runtime.tick(1607);CHECK(!m.alarmRinging);
    // A ring stops by itself after 3 minutes; 仅一次 turns itself off when it rings.
    m.alarmRinging=true;m.alarmRingAt=1610;runtime.tick(1789);CHECK(m.alarmRinging);runtime.tick(1790);CHECK(!m.alarmRinging);
    m.alarms.items[0].mode=3;{timeval at{1793057400,0};settimeofday(&at,nullptr);}
    runtime.tick(1800);CHECK(m.alarmRinging&&m.alarms.items[0].mode==0&&m.alarms.items[1].mode==2);runtime.key(Key::Back,1801);CHECK(!m.alarmRinging&&runtime.active()==App::Alarm);
    m.alarms={};SettingsStore::saveAlarms(m.alarms);m.alarmSnooze=m.alarmFired=0;runtime.connection(wasConnected,1802);
    // 自选股: Up/Down pick a row and Confirm opens it; in the detail Up/Down step through the list and Confirm switches
    // minutes / 30 days; long Confirm goes back to the list before leaving.
    CHECK(runtime.switchTo(App::Stock)==Result::Ok&&!m.stockCount&&!m.stockDetail);
    {auto& rows=m.page.stocks.rows;
     rows[0]={"上证指数","000001","SH",2,3690.48f,32.45f,0.89f,3623.57f,3691.4f,3574.27f,3658.03f,8.98e11f};
     rows[1]={"沪深300ETF","510300","SH",3,4.178f,0.071f,1.73f,4.08f,4.186f,4.031f,4.107f,1.54e10f};
     rows[2]={"纳指ETF","513100","SH",3,1.952f,-0.018f,-0.91f,1.97f,1.975f,1.948f,1.97f,2.1e9f};
     rows[3]={"贵州茅台","600519","SH",2,1335.0f,16.51f,1.25f,1311.84f,1343.88f,1310.0f,1318.49f,7.68e9f};
     rows[4]={"腾讯控股","00700","HK",3,386.0f,3.6f,0.94f,381.8f,388.2f,380.6f,382.4f,1.2e10f};
     rows[5]={"苹果","AAPL","US",3,347.725f,-0.495f,-0.14f,349.6f,351.2f,346.1f,348.22f,1.6e10f};
     rows[6]={"纽约黄金","GC","CMX",2,4007.6f,-0.3f,-0.01f,4012.2f,4017.9f,4003.1f,4007.9f,NAN};
     m.stockCount=7;m.stockUpdated=1792743101;}
    runtime.key(Key::Up,700);CHECK(m.stockIndex==6);runtime.key(Key::Down,700);CHECK(m.stockIndex==0);
    m.stockIndex=3;uiRender(runtime);uiCapture("stock_list");
    runtime.key(Key::Confirm,701);CHECK(m.stockDetail&&!m.stockDays);
    m.chartFor=3;m.chartDays=false;m.chartTotal=121;m.chartCount=100;m.chartLow=1308;m.chartHigh=1344;
    strcpy(m.chartFrom,"09:30");strcpy(m.chartMid,"11:30/13:00");strcpy(m.chartTo,"15:00");
    for(unsigned i=0;i<m.chartCount;++i){const float v=1316+11*sinf(i/9.f)+i*0.16f;m.page.stocks.chart[i]=int16_t((v-1308)/36*999);}
    uiRender(runtime);uiCapture("stock_detail");
    runtime.key(Key::Confirm,702);CHECK(m.stockDays);
    m.chartDays=true;m.chartCount=120;m.chartTotal=30;strcpy(m.chartFrom,"09-04");m.chartMid[0]=0;strcpy(m.chartTo,"10-23");
    for(unsigned d=0;d<30;++d){const int base=300+int(200*sinf(d/5.f))+d*8,move=(d%3==1?-1:1)*(40+d%4*15);
        m.page.stocks.chart[4*d]=int16_t(base);m.page.stocks.chart[4*d+1]=int16_t(base+move);m.page.stocks.chart[4*d+2]=int16_t(std::max(base,base+move)+30);m.page.stocks.chart[4*d+3]=int16_t(std::min(base,base+move)-30);}
    uiRender(runtime);uiCapture("stock_days");
    // Gold round the clock (Beijing time from the session's start), the axis from Eastmoney's first minute.
    m.stockIndex=6;m.stockDays=m.chartDays=false;m.chartFor=6;m.chartTotal=180;m.chartCount=60;m.chartLow=3996;m.chartHigh=4018;
    strcpy(m.chartFrom,"06:00");strcpy(m.chartMid,"北京时间");strcpy(m.chartTo,"次日06:00");
    for(unsigned i=0;i<m.chartCount;++i){const float v=4011+5*sinf(i/6.f)-i*0.18f;m.page.stocks.chart[i]=int16_t((v-3996)/22*999);}
    uiRender(runtime);uiCapture("stock_gold");
    m.stockIndex=3;runtime.key(Key::Down,703);CHECK(m.stockIndex==4&&m.stockDetail);
    runtime.key(Key::Back,704);CHECK(runtime.active()==App::Stock&&!m.stockDetail);m.stockCount=0;m.chartFor=255;
    // WiFi 工具箱: long Up/Down change tab; Confirm reruns the tab, except on 设备 where it saves the wake target and opens 唤醒.
    CHECK(runtime.switchTo(App::Wifi)==Result::Ok&&m.wifiTab==0);
    m.wifiApCount=4;m.page.wifi.aps[0]={"Kevin-Home",-42,6,false};m.page.wifi.aps[1]={"ChinaNet-Xa3F",-58,1,false};m.page.wifi.aps[2]={"Starbucks",-71,11,true};m.page.wifi.aps[3]={"",-80,7,false};
    runtime.key(Key::Down,521);CHECK(m.wifiIndex==1);{const auto action=m.wifiAction;runtime.key(Key::Confirm,521);CHECK(m.wifiAction==static_cast<uint8_t>(action+1));}
    uiRender(runtime);uiCapture("wifi_channels");
    CHECK(runtime.longKey(false,521)&&m.wifiTab==1&&m.wifiIndex==0);
    m.pingCount=3;m.page.wifi.pings[0]={"路由器","192.168.1.1",3,2,6,5,0};m.page.wifi.pings[1]={"阿里 DNS","223.5.5.5",18,15,24,5,0};m.page.wifi.pings[2]={"百度","www.baidu.com",0,0,0,5,5};m.dnsMs=23;
    uiRender(runtime);uiCapture("wifi_ping");
    CHECK(runtime.longKey(false,521)&&m.wifiTab==2);
    m.lanCount=3;m.page.wifi.lan[0]={{192,168,1,1},{0x10,0x20,0x30,0x40,0x50,0x60}};m.page.wifi.lan[1]={{192,168,1,23},{0xa4,0x5e,0x60,0x12,0x34,0x56}};m.page.wifi.lan[2]={{192,168,1,57},{0x7a,0x11,0x22,0x33,0x44,0x55}};
    m.lanGateway=0;m.lanSelf=255;runtime.key(Key::Down,521);uiRender(runtime);uiCapture("wifi_devices");
    runtime.key(Key::Confirm,521);CHECK(m.wifiTab==3&&m.tools.wakeSet&&m.tools.wakeMac[0]==0xa4);
    {ToolPrefs saved{};CHECK(SettingsStore::loadTools(saved)&&saved.wakeSet&&saved.wakeMac[5]==0x56);}
    m.wakeSentAt=1792892265;uiRender(runtime);uiCapture("wifi_wake");
    CHECK(runtime.longKey(true,521)&&m.wifiTab==2);
    // 移动监控: Up/Down sensitivity (kept), Confirm asks for a new baseline; the page dims but never deep-sleeps.
    CHECK(runtime.switchTo(App::Motion)==Result::Ok&&m.motionPhase==0&&m.motionHistoryCount==0);
    {const unsigned sense=m.tools.sense();runtime.key(Key::Up,523);CHECK(m.tools.sense()==std::min(sense+1,9u));ToolPrefs saved{};CHECK(SettingsStore::loadTools(saved)&&saved.sense()==m.tools.sense());
     for(unsigned i=0;i<12;++i)runtime.key(Key::Down,523);
     CHECK(m.tools.sense()==1&&m.tools.motionLimit()==82);
     for(unsigned i=0;i<12;++i)runtime.key(Key::Up,523);
     CHECK(m.tools.sense()==9&&m.tools.motionLimit()==18);
     while(m.tools.sense()!=sense)runtime.key(Key::Down,523);}
    {const auto calibrate=m.motionCalibrate;runtime.key(Key::Confirm,523);CHECK(m.motionCalibrate==static_cast<uint8_t>(calibrate+1));}
    runtime.connection(false,523);CHECK(runtime.power(523+999999)==Power::ScreenOff);runtime.connection(true,523);
    // Long Up steps the alarm 关 → 一次 → 连续 → 关, long Down the other way; kept.
    {const unsigned alarm=m.tools.alarm();CHECK(runtime.longKey(true,523)&&m.tools.alarm()==(alarm+1)%3);ToolPrefs saved{};CHECK(SettingsStore::loadTools(saved)&&saved.alarm()==(alarm+1)%3);
     CHECK(runtime.longKey(false,523)&&runtime.longKey(false,523)&&m.tools.alarm()==(alarm+2)%3);CHECK(runtime.longKey(true,523)&&m.tools.alarm()==alarm);}
    m.tools.motionAlarm=2;
    m.motionPhase=1;m.motionCalLeft=3;uiRender(runtime);uiCapture("motion_calibrating");
    for(unsigned i=0;i<60;++i)m.motionHistory[i]=static_cast<uint8_t>(i<44?6+i*7%11:i<52?40+(i*29)%50:20-(i-52));
    m.motionHistoryCount=60;m.motionPhase=3;m.motionLevel=72;m.motionRate=28;m.motionRssi=-46;m.motionEvents=3;m.motionLastAt=std::max<time_t>(time(nullptr),1700000100)-12;
    uiRender(runtime);uiCapture("motion_moving");
    // 声音: Up/Down calibrate (kept), Confirm clears the maximum; the tuner; the metronome keeps the screen awake while it runs.
    CHECK(runtime.switchTo(App::Sound)==Result::Ok&&m.soundView==0);
    {const int cal=m.tools.soundCal;runtime.key(Key::Up,522);CHECK(m.tools.soundCal==cal+1);ToolPrefs saved{};CHECK(SettingsStore::loadTools(saved)&&saved.soundCal==cal+1);runtime.key(Key::Down,522);}
    for(unsigned i=0;i<24;++i){m.soundBars[i]=static_cast<uint8_t>(20+i*37%60);}
    m.soundDb=624;m.soundMax=751;
    uiRender(runtime);uiCapture("sound_spectrum");runtime.key(Key::Confirm,522);CHECK(m.soundMax==0);
    CHECK(runtime.longKey(false,522)&&m.soundView==1);m.soundHz=4415;uiRender(runtime);uiCapture("sound_tuner");
    CHECK(runtime.longKey(false,522)&&m.soundView==2);
    {const auto bpm=m.tools.bpm;runtime.key(Key::Up,522);CHECK(m.tools.bpm==bpm+5);runtime.key(Key::Down,522);CHECK(m.tools.bpm==bpm);}
    runtime.key(Key::Confirm,522);CHECK(m.soundRunning);runtime.connection(false,522);CHECK(runtime.power(522+999999)==Power::Awake);runtime.connection(true,522);
    m.soundBeat=0;uiRender(runtime);uiCapture("sound_metronome");
    CHECK(runtime.longKey(false,522)&&m.soundView==0&&!m.soundRunning);
    m.livePhase=phase;
    runtime.key(Key::Back,517);CHECK(runtime.active()==App::Menu);
    puts("CASE_PASS tool_pages_calendar_countdown_qr_torch_sonar_news_rate_history_daily_quake_quiz_hn_wifi_motion_sound");
}
void slidesTests(Runtime& runtime){
    CHECK(runtime.switchTo(App::Slides)==Result::Ok);auto& m=runtime.model;
    CHECK(!m.slidesPressed);
    runtime.key(Key::Down,10);CHECK(m.slidesPressed&&m.slidesLast==SlideKey::Next);
    runtime.key(Key::Up,11);CHECK(m.slidesLast==SlideKey::Previous);
    runtime.key(Key::Confirm,20);CHECK(m.slidesLast==SlideKey::Start);
    runtime.slideKey(SlideKey::End,101);
    runtime.key(Key::Back,102);CHECK(runtime.active()==App::Menu);
    runtime.slideKey(SlideKey::Next,103);CHECK(m.slidesLast==SlideKey::End);  // ignored off the page
    // Every visit starts a new talk at 00:00, whatever the keys did last time.
    runtime.tick(500);CHECK(runtime.switchTo(App::Slides)==Result::Ok&&m.slidesStarted==500&&!m.slidesPressed);
    CHECK(runtime.switchTo(App::Menu)==Result::Ok);
    // Screen fixture: linked, after a 12:34 talk.
    CHECK(runtime.switchTo(App::Slides)==Result::Ok);m.slidesReady=m.bleConnected=m.bleEncrypted=true;m.slidesStarted=esp_timer_get_time()/1000000-754;m.slidesLast=SlideKey::Next;m.slidesPressed=true;
    uiRender(runtime);uiCapture("slides_ready");
    m.slidesReady=m.bleConnected=m.bleEncrypted=m.slidesPressed=false;
    CHECK(runtime.switchTo(App::Home)==Result::Ok);
    puts("CASE_PASS slides_keys_talk_timer_and_menu_return");
}
void bleTests(Runtime& runtime) {
    CHECK(runtime.switchTo(App::Settings) == Result::Ok);
    const auto before = freeHeap();
    BleSessionModel ble;
    CHECK(ble.start(BleRole::Data));
    CHECK(ble.state() == BleState::Advertising);
    CHECK(ble.onConnect(1));
    CHECK(!ble.onConnect(2)); // One peer at a time.
    CHECK(!ble.accept(BleRole::Data, ble.generation(), 1, 16));
    CHECK(!ble.onSecurityReady(true, false));
    CHECK(ble.onSecurityReady(true, true));
    CHECK(ble.accept(BleRole::Data, ble.generation(), 1, 16));
    CHECK(!ble.accept(BleRole::Data, ble.generation(), 1, 16));
    CHECK(!ble.accept(BleRole::Data, ble.generation(), 2, 161));
    CHECK(!ble.accept(static_cast<BleRole>(1), ble.generation(), 2, 8));
    CHECK(!ble.start(BleRole::Data)); // Role change must disconnect first.
    runtime.model.bleConnected = true;
    uiRender(runtime); uiCapture("settings_ble_connected");
    sample("ble_budget_connected_model", runtime);
    // With the current conservative budgets, retain the BLE owner and reject
    // heavy audio until its controller has safely disconnected/released BLE.
    CHECK(runtime.switchTo(App::Chat) == Result::LowMemory);
    CHECK(runtime.active() == App::Home);
    const auto oldGeneration = ble.generation();
    ble.disconnect(); runtime.model.bleConnected = false;
    CHECK(!ble.accept(BleRole::Data, oldGeneration, 2, 16));
    CHECK(ble.start(BleRole::Data));
    CHECK(ble.onConnect(3)); CHECK(ble.onSecurityReady(true, true));
    CHECK(ble.accept(BleRole::Data, ble.generation(), 1, 8));
    ble.stop(); CHECK(ble.state() == BleState::Off);
    CHECK(freeHeap() + 128 >= before);
    CHECK(runtime.switchTo(App::Chat) == Result::Ok);
    CHECK(runtime.switchTo(App::Home) == Result::Ok);
    puts("CASE_PASS ble_model_pairing_security_single_peer_role_change_stale_rejection_heap_recovery");
}
// The phone page's upload path: header first, streamed body, CRC check, UI reload.
void identityUploadTests(Runtime& runtime) {
    CHECK(runtime.switchTo(App::Home)==Result::Ok);runtime.model.homeScreen=HomeScreen::Card;uiRender(runtime);
    CHECK(identity().valid&&!identityWrite(0,"x",1));
    static uint8_t chunk[1024];
    auto upload=[&](bool corrupt){
        CHECK(identityWriteBegin());CHECK(!identityWriteBegin()&&identityBusy());
        uiRender(runtime);CHECK(identity().busy&&!identity().valid);
        uint32_t crc=0;
        for(uint32_t offset=16;offset<kIdentityBytes;offset+=sizeof(chunk)){
            const auto size=std::min<uint32_t>(sizeof(chunk),kIdentityBytes-offset);
            for(uint32_t i=0;i<size;++i){const auto at=offset+i;chunk[i]=at<16+64?(at-16<6?"Tester"[at-16]:0):at<304?0:static_cast<uint8_t>(at*7);}
            crc=esp_rom_crc32_le(crc,chunk,size);CHECK(identityWrite(offset,chunk,size));
            if(offset==16+sizeof(chunk)*8){uiRender(runtime);runtime.model.homeScreen=HomeScreen::Photo;uiRender(runtime);runtime.model.homeScreen=HomeScreen::Card;}
        }
        const uint32_t header[4]={0x44495050,1,corrupt?crc^1:crc,3};
        CHECK(identityWrite(0,header,sizeof(header)));CHECK(!identityWrite(kIdentityBytes-1,chunk,2));
        CHECK(identityWriteFinish()!=corrupt);CHECK(!identityBusy());
    };
    upload(false);uiRender(runtime);
    CHECK(identity().valid&&!strcmp(identity().name,"Tester")&&!*identity().title&&identity().hasAvatar&&identity().hasPhoto);
    uint8_t header[16];CHECK(identityStored()&&identityRead(0,header,sizeof(header))&&!memcmp(header,"PPID",4));
    runtime.model.homeScreen=HomeScreen::Photo;uiRender(runtime);
    upload(true);uiRender(runtime);CHECK(!identity().valid&&!identity().busy&&!identityStored());
    runtime.model.homeScreen=HomeScreen::Weather;uiRender(runtime);
    puts("CASE_PASS identity_upload_stream_crc_reject_busy_placeholder_reload");
}
void failureTests(Runtime& runtime) {
    CHECK(runtime.switchTo(App::Chat) == Result::Ok);
    const auto owned = runtime.workspaceBytes();
    runtime.failNextStop();
    CHECK(runtime.switchTo(App::Radio) == Result::StopTimeout);
    CHECK(runtime.active() == App::Chat && runtime.workspaceBytes() == owned);
    runtime.connection(false, 0);
    CHECK(runtime.power(1000) == Power::Awake);
    CHECK(runtime.switchTo(App::Chat) == Result::Ok); // Same-app recovery must quiesce again.
    CHECK(runtime.switchTo(App::Radio) == Result::Ok);
    const auto oldGeneration = runtime.generation();
    CHECK(runtime.switchTo(App::Monitor) == Result::Ok);
    const auto rejected = runtime.rejectedEvents;
    CHECK(runtime.post({App::Radio, oldGeneration, 1}));
    runtime.poll(); CHECK(runtime.rejectedEvents > rejected);
    for (int i = 0; i < 100; ++i) runtime.post({App::Radio, oldGeneration, 1});
    CHECK(runtime.queueDrops > 0); runtime.poll();
    CHECK(runtime.switchTo(App::Chat, true) == Result::LowMemory);
    CHECK(runtime.active() == App::Home && runtime.workspaceBytes() == 0);
    CHECK(runtime.switchTo(App::Chat, false, true) == Result::StartFailed);
    CHECK(runtime.active() == App::Home && runtime.workspaceBytes() == 0);
    CHECK(runtime.switchTo(static_cast<App>(255)) == Result::Invalid);
    CHECK(runtime.switchTo(static_cast<App>(6)) == Result::Invalid); // Retired iPhone app ID.
    CHECK(runtime.switchTo(App::Home) == Result::Ok);

    // Actual allocator pressure and fragmentation, not a mocked free-memory number.
    std::array<void*, 40> pressure{};
    unsigned count = 0;
    while (count < pressure.size() && freeHeap() > 16384) {
        pressure[count] = heap_caps_malloc(8192, kCaps);
        if (!pressure[count]) break;
        memset(pressure[count], 0xa5, 8192); ++count;
    }
    CHECK(count > 4);
    CHECK(runtime.switchTo(App::Chat) == Result::LowMemory);
    sample("oom_rejected", runtime);
    for (unsigned i = 0; i < count; ++i) if (i % 4) { heap_caps_free(pressure[i]); pressure[i] = nullptr; }
    const bool fragmentationOnly = freeHeap() >= profile(App::Radio).workspace + kReserve;
    CHECK(heap_caps_get_largest_free_block(kCaps) < profile(App::Radio).workspace);
    CHECK(runtime.switchTo(App::Radio) == Result::LowMemory);
    printf("FRAGMENTATION_ONLY free_sufficient=%d\n", fragmentationOnly);
    CHECK(fragmentationOnly);
    sample("fragmented_rejected", runtime);
    for (auto* p : pressure) if (p) heap_caps_free(p);
    CHECK(runtime.switchTo(App::Chat) == Result::Ok);
    CHECK(runtime.switchTo(App::Home) == Result::Ok);
    runtime.connection(true, 0);
    puts("CASE_PASS stop_timeout_late_events_queue_overflow_start_failure_real_oom_fragmentation_recovery");
}
}

extern "C" void app_main() {
    using namespace passport;
    puts("SIM_BOOT_OK suite cpp=20 chip=esp32c3 flash=8MB psram=0 physical_device=0");
    static Runtime runtime;
    static SystemSettings settings;
    SettingsStore store;
    ESP_ERROR_CHECK(nvs_flash_init());
    bool created = false;
    CHECK(store.loadOrCreate(settings, created));
    SystemSettings reloaded;
    CHECK(store.load(reloaded));
    CHECK(memcmp(&settings, &reloaded, sizeof(settings)) == 0);
    printf("SETTINGS_OK source=%s writes=%lu reads=%lu idle=%lu credentials_logged=0\n",
           created ? "created" : "persisted", static_cast<unsigned long>(store.writes),
           static_cast<unsigned long>(store.reads), static_cast<unsigned long>(settings.idleSeconds));
    const auto configWrites = store.writes, configReads = store.reads;
    sample("boot", runtime);
    networkStart(settings);
    sample("wifi_connected", runtime);
    uiInit();
    CHECK(runtime.init(settings));
    // Screenshot fixture from the supplied reference; production uses native IP weather.
    runtime.model.weatherReading.update("杭州","小毛毛雨",24.7,21,31,85,51,161);
    runtime.model.wifiConnected=true;runtime.model.wifiSignalKnown=true;runtime.model.wifiRssi=-50;
    runtime.model.batteryKnown=true;runtime.model.batteryPercent=85;
    setenv("TZ","CST-8",1);tzset();
    timeval homeTime{1792490743,0};settimeofday(&homeTime,nullptr);
    uiRender(runtime);
    sample("core_ready", runtime);
    uiResourcesReady(false);CHECK(runtime.switchTo(App::Chat)==Result::Ok);uiRender(runtime);
    CHECK(!uiPreparedFor(App::Chat));
    uiResourcesReady(true);uiRender(runtime);CHECK(uiPreparedFor(App::Chat));
    CHECK(runtime.switchTo(App::Home)==Result::Ok);uiRender(runtime);
    CHECK(runtime.switchTo(App::Chat)==Result::Ok);uiRender(runtime);uiCapture("chat_reentry");
    CHECK(runtime.switchTo(App::Home)==Result::Ok);uiRender(runtime);
    printf("STATIC model=%u runtime=%u lvgl_pool_home=22528 lvgl_pool_chat=12288 lcd_dma=9600 shared_worker_stack=4096\n",
           static_cast<unsigned>(sizeof(Model)), static_cast<unsigned>(sizeof(Runtime)));

    // C++ renders the actual LVGL RGB565 pixels. Host only assembles image rows.
    CHECK(runtime.active() == App::Home);
    uiRender(runtime); uiCapture("home");
    runtime.model.batteryPercent=100;uiRender(runtime);uiCapture("home_battery_full");
    runtime.model.batteryPercent=15;uiRender(runtime);uiCapture("home_battery_low");
    runtime.model.batteryPercent=0;uiRender(runtime);uiCapture("home_battery_empty");
    runtime.model.batteryPercent=85;
    // A yellow 大风 warning beside the city.
    snprintf(runtime.model.weatherReading.warning,sizeof(runtime.model.weatherReading.warning),"大风");runtime.model.weatherReading.warningLevel=1;uiRender(runtime);uiCapture("home_warning");
    runtime.model.weatherReading.warning[0]=0;runtime.model.weatherReading.warningLevel=-1;
    const auto fixtureWeather=runtime.model.weatherReading;
    runtime.model.weatherReading={};runtime.model.batteryKnown=false;runtime.model.wifiConnected=false;runtime.model.pppConnected=true;
    uiRender(runtime);uiCapture("home_unavailable");
    runtime.model.weatherReading=fixtureWeather;runtime.model.batteryKnown=true;runtime.model.wifiConnected=true;runtime.model.pppConnected=false;
    // Up/Down cycle weather -> identity card -> full-screen photo, persisted in NVS.
    CHECK(identity().valid&&identity().hasAvatar&&identity().hasPhoto&&!strcmp(identity().name,"Kevin"));
    runtime.key(Key::Down, 0); CHECK(runtime.active() == App::Home&&runtime.model.homeScreen==HomeScreen::Card);
    uiRender(runtime);uiCapture("home_card");CHECK(SettingsStore::loadByte("home_screen")==1);
    runtime.key(Key::Down, 0); CHECK(runtime.model.homeScreen==HomeScreen::Photo);uiRender(runtime);uiCapture("home_photo");
    runtime.key(Key::Down, 0); CHECK(runtime.model.homeScreen==HomeScreen::Weather);
    runtime.key(Key::Up, 0); CHECK(runtime.model.homeScreen==HomeScreen::Photo);uiRender(runtime);
    runtime.key(Key::Confirm, 0); CHECK(runtime.active() == App::Menu);uiRender(runtime);
    runtime.key(Key::Back, 0); CHECK(runtime.active() == App::Home&&runtime.model.homeScreen==HomeScreen::Photo);uiRender(runtime);
    runtime.key(Key::Down, 0); CHECK(runtime.model.homeScreen==HomeScreen::Weather&&SettingsStore::loadByte("home_screen")==0);uiRender(runtime);
    puts("CASE_PASS home_screens_weather_card_photo_cycle_persist");
    runtime.key(Key::Confirm, 0); CHECK(runtime.active() == App::Menu);
    runtime.key(Key::Down, 0); runtime.key(Key::Up, 0);
    CHECK(runtime.selection() == 0);
    uiRender(runtime); uiCapture("menu_1");
    CHECK(uiAnimating(runtime));uiCapture("footer_scroll_1");
    for(unsigned i=0;i<40;++i){vTaskDelay(pdMS_TO_TICKS(50));uiRender(runtime);}
    uiCapture("footer_scroll_2");
    for (unsigned i = 0; i < kFunctions.size()-1; ++i) runtime.key(Key::Down, 0);
    CHECK(runtime.selection() == kFunctions.size()-1);
    runtime.key(Key::Down, 0); CHECK(runtime.selection() == 0);
    runtime.key(Key::Up, 0); CHECK(runtime.selection() == kFunctions.size()-1);
    uiRender(runtime); uiCapture("menu_2");
    // The middle pages under their themes, then 系统设置 at the end of the last.
    CHECK(runtime.longKey(true,0)&&runtime.selection()==27);uiRender(runtime);uiCapture("menu_page4");
    CHECK(runtime.longKey(true,0)&&runtime.selection()==18);uiRender(runtime);uiCapture("menu_page3");
    CHECK(runtime.longKey(true,0)&&runtime.selection()==9);uiRender(runtime);uiCapture("menu_page2");
    while(runtime.selection()!=kFunctions.size()-1)runtime.key(Key::Down,0);
    runtime.key(Key::Confirm, 0); CHECK(runtime.active() == App::Settings);
    runtime.key(Key::Back, 0); CHECK(runtime.active() == App::Menu);
    runtime.key(Key::Back, 0); CHECK(runtime.active() == App::Home);
    runtime.key(Key::OpenSettings, 0); CHECK(runtime.active() == App::Settings);
    uiRender(runtime); uiCapture("settings");
    runtime.key(Key::Back, 0); CHECK(runtime.active() == App::Menu);runtime.key(Key::Back,0);CHECK(runtime.active()==App::Home);
    for (const auto app : kFunctions) {
        ESP_ERROR_CHECK(heap_caps_monitor_local_minimum_free_size_start());
        CHECK(runtime.switchTo(app) == Result::Ok);
        uiRender(runtime); uiCapture(profile(app).id);
        sample("app_active", runtime);
        printf("PEAK {\"app\":\"%s\",\"minimum_free\":%u}\n", profile(app).id,
               static_cast<unsigned>(heap_caps_get_minimum_free_size(kCaps)));
        ESP_ERROR_CHECK(heap_caps_monitor_local_minimum_free_size_stop());
        CHECK(freeHeap() >= kReserve);
        CHECK(runtime.switchTo(App::Home) == Result::Ok);
        uiRender(runtime);
    }
    runtime.switchTo(App::Dictation); runtime.model.bridgeAlive = false;
    uiRender(runtime); uiCapture("dictation_disconnected");
    runtime.model.bridgeAlive = true;
    {   // 语音输入: what the computer reports at each step (liveValue, liveState, the text typed and its size, the link).
        auto& m=runtime.model;const uint64_t now=esp_timer_get_time()/1000000;
        const auto report=[&](const char* value,const char* state,const char* text,const char* size){
            strcpy(m.liveValue,value);strcpy(m.liveState,state);strcpy(m.liveLines[0],text);strcpy(m.liveLines[1],size);strcpy(m.liveLines[2],"USB");};
        report("ready","USB 就绪","","");uiRender(runtime);uiCapture("dictation_ready");
        report("rec","正在录音","","");m.listenAt=now>7?now-7:0;uiRender(runtime);uiCapture("dictation_recording");
        report("asr","正在识别","","");uiRender(runtime);uiCapture("dictation_recognizing");
        const char* typed="明天下午三点和设计团队开会，记得带上新版的原型图，会后把纪要发到群里。";
        report("done","已输入",typed,"34 字 · 14:05");m.dictationAt=now;uiRender(runtime);uiCapture("dictation_done");
        report("empty","没有听清，请再说一次",typed,"34 字 · 14:05");uiRender(runtime);uiCapture("dictation_empty");
    }
    runtime.switchTo(App::Home);
    // Explicit UI fixtures; none are reported as live account or sensor data.
    runtime.model.quotas[0]={true,24,300,time(nullptr)+2*3600+10*60};runtime.model.quotas[1]={true,11,10080,time(nullptr)+6*86400+3*3600};
    runtime.model.quotas[2]={true,0,300,time(nullptr)+3600+30*60};runtime.model.quotas[3]={true,74,10080,time(nullptr)+86400+15*3600};
    runtime.switchTo(App::Codex);uiRender(runtime);uiCapture("codex_quota");
    {   // Pomodoro fixture: third focus round, 18:23 left; then the same round paused.
        runtime.switchTo(App::Focus);auto& p=runtime.model;const auto saved=std::make_tuple(p.focusRunning,p.focusRemaining,p.focusRounds,p.focusDeadline);
        p.focusRounds=2;p.focusRemaining=1103;p.focusRunning=true;p.focusDeadline=~0ULL;
        uiRender(runtime);uiCapture("focus_running");CHECK(!uiAnimating(runtime));  // the footer fits: no 8 Hz redraws
        p.focusRunning=false;uiRender(runtime);uiCapture("focus_paused");CHECK(!uiAnimating(runtime));
        std::tie(p.focusRunning,p.focusRemaining,p.focusRounds,p.focusDeadline)=saved;
    }
    runtime.switchTo(App::Home);runtime.model.live=true;
    runtime.switchTo(App::Monitor);runtime.model.metricsKnown=true;runtime.model.cpu=37;runtime.model.ram=68;
    runtime.model.memoryUsed=44ULL<<30;runtime.model.memoryTotal=64ULL<<30;
    runtime.model.disk=90;runtime.model.diskFree=43ULL<<30;runtime.model.battery=76;runtime.model.charging=true;runtime.model.load=3.4f;runtime.model.cores=10;runtime.model.uptime=8*86400+23*3600;
    runtime.model.netUp=12*1024;runtime.model.netDown=340*1024;
    runtime.model.livePhase=LivePhase::Ready;runtime.model.historyCount=24;
    for(unsigned i=0;i<24;++i){runtime.model.cpuHistory[i]=i%5*12+12;}
    uiRender(runtime);uiCapture("monitor_dashboard");
    // Radio fixture in the shape native_services publishes; station names are samples.
    const auto media=[&](uint8_t index,uint8_t count,const char* state,const char* name,const char* const* rows){
        auto& m=runtime.model;m.mediaIndex=index;m.mediaCount=count;m.livePhase=LivePhase::Ready;
        strcpy(m.liveState,state);strcpy(m.liveValue,name);for(unsigned i=0;i<4;++i)strcpy(m.liveLines[i],rows[i]);
    };
    runtime.switchTo(App::Radio);
    {const char* rows[]{"本地 · 64k","所在城市：杭州","下一个：西湖之声",""};media(2,24,"正在播放","杭州交通91.8电台",rows);}
    uiRender(runtime);uiCapture("media_radio");
    runtime.model.mediaCount=0;
    // Chat record: a turn is logged once, reply sentences join one entry, re-entry starts empty.
    {
        CHECK(runtime.switchTo(App::Chat)==Result::Ok);auto& m=runtime.model;m.live=true;m.livePhase=LivePhase::Ready;
        m.chatRecord(0,"","你好呀，有什么想聊的吗？");
        m.chatRecord(1,"今天杭州天气怎么样","");m.chatRecord(1,"今天杭州天气怎么样","");CHECK(m.chatCount==2&&m.chat[1].user);
        m.chatRecord(1,"今天杭州天气怎么样","杭州今天多云，");m.chatRecord(1,"今天杭州天气怎么样","杭州今天多云，");
        m.chatRecord(1,"今天杭州天气怎么样","气温 18 到 24 度，适合出门散步。");
        CHECK(m.chatCount==3&&!m.chat[2].user&&!strcmp(m.chat[2].text,"杭州今天多云，气温 18 到 24 度，适合出门散步。"));
        m.chatRecord(2,"那明天呢","明天有小雨，记得带伞。");CHECK(m.chatCount==5);
        for(unsigned t=3;t<6;++t)m.chatRecord(t,"再说一个","好的。");
        CHECK(m.chatCount==6&&!strcmp(m.chat[5].text,"好的。"));  // the oldest entries drop off
        CHECK(runtime.switchTo(App::Home)==Result::Ok&&runtime.switchTo(App::Chat)==Result::Ok&&!m.chatCount);
        m.chatRecord(0,"","你好呀，有什么想聊的吗？");m.chatRecord(1,"今天杭州天气怎么样","杭州今天多云，气温 18 到 24 度，适合出门散步。");
        m.chatRecord(2,"那明天呢","明天有小雨，记得带伞。");strcpy(m.liveLines[2],"自动收音 · ");strcpy(m.liveState,"云端就绪");
        uiRender(runtime);uiCapture("chat_record");CHECK(!uiAnimating(runtime));  // a still footer: no 8 Hz redraws during audio
        // Six full entries must still fit the 12 KiB chat pool (text is not copied into it).
        for(unsigned t=3;t<9;++t)m.chatRecord(t,"请再详细地讲一讲这个问题，我想知道更多的背景和细节，谢谢你的耐心解答","这是一段很长的回答，用来确认文字记录在最长的情况下仍然能放进聊天页面的内存池里。");
        uiRender(runtime);CHECK(uiMemoryFree()>=1536);m.liveLines[2][0]=0;
        CHECK(runtime.switchTo(App::Home)==Result::Ok);
    }
    runtime.switchTo(App::Chat);runtime.model.livePhase=LivePhase::Loading;strcpy(runtime.model.connectionState,"云端连接中");
    CHECK(uiAnimating(runtime));uiRender(runtime);uiCapture("connecting_1");vTaskDelay(pdMS_TO_TICKS(180));uiRender(runtime);uiCapture("connecting_2");
    runtime.model.livePhase=LivePhase::Error;strcpy(runtime.model.liveState,"连接超时");uiRender(runtime);uiCapture("connection_error");
    runtime.switchTo(App::Home);runtime.model.live=false;
    uiRender(runtime); uiCheck();
    const auto baseline = freeHeap();
    sample("warm_baseline", runtime);
    size_t activeMin = baseline, activeLargestMin = heap_caps_get_largest_free_block(kCaps);
    // All 6 x 6 function pairs. Returning Home must recover the baseline.
    for (const auto from : kFunctions) for (const auto to : kFunctions) {
        CHECK(runtime.switchTo(from) == Result::Ok);
        CHECK(runtime.switchTo(to) == Result::Ok);
        vTaskDelay(pdMS_TO_TICKS(2)); runtime.poll(); uiRender(runtime);
        activeMin = std::min(activeMin, freeHeap());
        activeLargestMin = std::min(activeLargestMin, heap_caps_get_largest_free_block(kCaps));
        CHECK(runtime.switchTo(App::Home) == Result::Ok);
        uiRender(runtime);
        CHECK(freeHeap() + 1024 >= baseline);
    }
    for (unsigned cycle = 0; cycle < 30; ++cycle) {
        for (const auto app : kFunctions) {
            CHECK(runtime.switchTo(app) == Result::Ok);
            runtime.poll(); uiRender(runtime);
            activeMin = std::min(activeMin, freeHeap());
            activeLargestMin = std::min(activeLargestMin, heap_caps_get_largest_free_block(kCaps));
        }
        CHECK(runtime.switchTo(App::Home) == Result::Ok);
        uiRender(runtime);
        CHECK(freeHeap() + 1024 >= baseline);
        CHECK(heap_caps_check_integrity_all(true));
    }
    sample("switch_stress_done", runtime);
    printf("STRESS {\"directed_pairs\":49,\"cycles\":30,\"baseline_free\":%u,\"recovered_free\":%u,\"active_min_free\":%u,\"active_min_largest\":%u,\"switches\":%lu}\n",
        static_cast<unsigned>(baseline), static_cast<unsigned>(freeHeap()), static_cast<unsigned>(activeMin),
        static_cast<unsigned>(activeLargestMin), static_cast<unsigned long>(runtime.switches));
    puts("CASE_PASS all_49_pairs_30_cycles_ui_and_heap_recovery");
    CHECK(&runtime.settings() == &settings);
    CHECK(store.writes == configWrites && store.reads == configReads);
    wifi_ap_record_t ap{};
    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&ap));
    CHECK(strcmp(reinterpret_cast<const char*>(ap.ssid), settings.wifiSsid) == 0);
    puts("CASE_PASS one_shared_config_no_reconfigure_during_switches_wifi_still_associated");
    domainTests(runtime);
    settingsTests(runtime);
    powerTests(runtime);
    slidesTests(runtime);alertTests(runtime);statusTests(runtime);wheelTests(runtime);translateTests(runtime);guardTests(runtime);todoTests(runtime);ledgerTests(runtime);calcTests(runtime);walkieTests(runtime);voiceCommandTests(runtime);voiceAddTests(runtime);toolPagesTests(runtime);
    bleTests(runtime);
    identityUploadTests(runtime);
    failureTests(runtime);
    uiRender(runtime); uiCheck();
    sample("final_home", runtime);
    CHECK(freeHeap() + 1024 >= baseline);
    printf("SUITE_PASS checks=%u switches=%lu rejected_events=%lu queue_drops=%lu\n", checks,
           static_cast<unsigned long>(runtime.switches), static_cast<unsigned long>(runtime.rejectedEvents),
           static_cast<unsigned long>(runtime.queueDrops));
    CHECK(runtime.shutdown()); uiShutdown();
    puts("SUITE_DONE");
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
