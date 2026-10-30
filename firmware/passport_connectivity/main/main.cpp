// SIMULATOR ONLY. Real IDF WiFi/NimBLE stacks; virtual peers and test credentials.
#include "startup_object.hpp"
#include "suite.hpp"
#include "identity.hpp"
#ifdef PASSPORT_DEVICE
#include "board.hpp"
#include "tool_tasks.hpp"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "wifi_portal.hpp"
#include "portal_protocol.hpp"
#include "ble_companion.hpp"
#include "ble_slides.hpp"
#include "esp_app_desc.h"
#include "esp_sleep.h"
#endif
#include "live_services.hpp"
#include "ble_dictation.hpp"
#include "ppp_link.hpp"
#include "mbedtls/base64.h"
#ifdef PASSPORT_DEVICE
#define PASSPORT_SIMULATOR_JSON "false"
#else
#define PASSPORT_SIMULATOR_JSON "true"
#endif
#include <atomic>
#include <memory>
#include <new>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_rom_serial_output.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
extern "C" {
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
void ble_store_config_init(void);
}

namespace {
using namespace passport;
Runtime& runtime=startupObject<Runtime>();
LiveServices live(runtime);
SystemSettings settings;
std::atomic<bool> station{}, ap{}, bt{}, linked{}, encrypted{}, hostStopped{}, notify{}, bleReady{};
ble_npl_event bleReadyEvent;
std::atomic<int> advResult{-100}; // Last ble_gap_adv_start result; -100 = never advertised.
// Pairing diagnostics; -1 = not seen since boot.
std::atomic<int> bonds{-1},encStatus{-1},disconnectReason{-1};std::atomic<unsigned> repeatPairings{};
// The link as negotiated: connection interval (1.25 ms units), ATT MTU and LL payload octets the badge may send.
std::atomic<unsigned> linkInterval{},linkMtu{},linkOctets{};
std::atomic<int> apClients{}, wifiReason{}, appId{}, actionResult{}, wifiRetryBudget{};
std::atomic<int64_t> wifiRetryAt{};
// 已保存的 WiFi: a join is under way (cleared when it connects or gives up); a scan for saved networks runs or is done.
// joinTarget is the saved network being joined; its password is cleared once it is remembered or failed.
std::atomic<bool> wifiConnecting{},autoScanDone{};std::atomic<unsigned> wifiScans{},wifiJoins{};
bool autoScanning=false,joinActive=false;int64_t nextAutoScan=0;WifiNetwork joinTarget{};
std::atomic<unsigned> wifiRetries{};
std::atomic<unsigned> pagePrepareUs{},pageRenderUs{},pageSpiUs{},renderPixelsLast{},renderSpiUsLast{},renderLastUs{},renderMaxUs{},renderCount{},loopMaxUs{};std::atomic<uint64_t> renderTotalUs{};
std::atomic<unsigned> selection{}, appGeneration{1};
std::atomic<uint64_t> wordProgress{};  // cursor, queue size, words added, mastered
std::atomic<unsigned> connects{}, disconnects{}, writes{}, securityEvents{};
std::atomic<unsigned> mainStack{}, consoleStack{}, completedCommand{};
esp_netif_t *staNetif, *apNetif;
uint8_t addressType;
uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE, valueHandle;
char gattValue[65] = "AI-Passport-C3-Sim";
portMUX_TYPE gattMux = portMUX_INITIALIZER_UNLOCKED;
struct Command { unsigned id; char name[40];
#ifdef PASSPORT_DEVICE
 char ssid[33],password[65];int locationConsent{-1},bluetooth{-1};uint32_t inputContext{},inputGeneration{};bool physical{};
#endif
};
QueueHandle_t commands;
bool dirty = true;
bool restoreBleAfterAudio = false;
bool wifiInitialized = false;
// WiFi and BLE share one radio. Awake WiFi takes half of every 100 ms period, too little for 语音输入 audio, so
// while BLE runs WiFi stays connected but sleeps between beacons, leaving the radio to BLE.
int wifiPowerSave(){return esp_wifi_set_ps(bt?WIFI_PS_MIN_MODEM:WIFI_PS_NONE);}
#ifdef PASSPORT_DEVICE
WifiPortal& wifiPortal=startupObject<WifiPortal>();
SystemSettings pendingWifi;
bool wifiPending=false,restoreBleAfterProvision=false;int pendingLocationConsent=-1;
int64_t wifiJoinDeadline=0,closePortalAt=0;
int bleStart();int bleStop();
#else
bool wifiPausedForPpp=false;
#endif
const ble_uuid128_t serviceUuid = BLE_UUID128_INIT(1,0xb0,0xe3,0xdc,0xbd,0x4f,0x6c,0x9f,0x30,0x4e,0xed,0x70,0x6a,0xd6,0x67,0x5d);
const ble_uuid128_t echoUuid = BLE_UUID128_INIT(2,0xb0,0xe3,0xdc,0xbd,0x4f,0x6c,0x9f,0x30,0x4e,0xed,0x70,0x6a,0xd6,0x67,0x5d);

void state(char* data, size_t size) {
    snprintf(data, size, "{\"service\":\"passport-c3-live\",\"simulator\":" PASSPORT_SIMULATOR_JSON ",\"wifi_station\":%s,\"wifi_ap\":%s,\"ap_clients\":%d,\"wifi_reason\":%d,\"ble_stack\":%s,\"ble_synced\":%s,\"ble_adv\":%s,\"ble_adv_rc\":%d,\"ble_bonds\":%d,\"ble_enc_status\":%d,\"ble_repeat_pairings\":%u,\"ble_disconnect_reason\":%d,\"ble_link\":[%u,%u,%u],\"ble_connected\":%s,\"ble_encrypted\":%s,\"ble_connects\":%u,\"ble_disconnects\":%u,\"ble_writes\":%u,\"security_events\":%u,\"app\":%d,\"last_result\":%d,\"free_heap\":%u,\"largest\":%u,\"main_stack_min_bytes\":%u,\"console_stack_min_bytes\":%u,\"selection\":%u,\"command_id\":%u,\"wifi_retries\":%u,\"audio_encoded\":%u,\"audio_decoded\":%u,\"audio_errors\":%u,\"audio_max_us\":%u,\"audio_stack_free\":%u,\"audio_active\":%s,\"generation\":%u,\"dictation_phase\":%u,\"ble_audio_bytes\":%u,\"ble_audio_dropped\":%u,\"ui_free\":%u,\"ui_peak\":%u,\"vad_mic\":%u,\"vad_ref\":%u,\"vad_residual\":%u,\"sonar_devices\":%u,\"sonar_peer_rssi\":%d}",
        station ? "true" : "false", ap ? "true" : "false", apClients.load(), wifiReason.load(),
        bt ? "true" : "false", bt&&ble_hs_synced() ? "true" : "false", bt&&ble_gap_adv_active() ? "true" : "false", advResult.load(),bonds.load(),encStatus.load(),repeatPairings.load(),disconnectReason.load(),linkInterval.load(),linkMtu.load(),linkOctets.load(),
        linked ? "true" : "false", encrypted ? "true" : "false",
        connects.load(), disconnects.load(), writes.load(), securityEvents.load(), appId.load(), actionResult.load(),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),mainStack.load(),consoleStack.load(),selection.load(),completedCommand.load(),wifiRetries.load(),static_cast<unsigned>(live.audio().encoded.load()),static_cast<unsigned>(live.audio().decoded.load()),static_cast<unsigned>(live.audio().errors.load()),static_cast<unsigned>(live.audio().maximumUs.load()),static_cast<unsigned>(live.audio().stackFree.load()),live.audio().active() ? "true" : "false",appGeneration.load(),dictationBle.phase(),static_cast<unsigned>(dictationBle.pcmBytes.load()),static_cast<unsigned>(dictationBle.dropped.load()),uiMemoryFree(),uiMemoryPeak(),live.audio().vadMicRms.load(),live.audio().vadRefRms.load(),live.audio().vadResidualRms.load(),static_cast<unsigned>(runtime.model.sonarCount),runtime.model.sonarPeerRssi);
    const auto used=strlen(data);
    if(used&&used+450<size) {
        char ppp[360];pppLink.status(ppp,sizeof(ppp));
        snprintf(data+used-1,size-used+1,",\"ppp\":%s,\"usb_connected\":%s,\"idle_seconds\":%u,\"power\":%u,\"voice_interruptions\":%u,\"voice_ends\":%u,\"voice_overflows\":%u,\"minimum_heap\":%u,\"uptime_seconds\":%llu}",ppp,runtime.connected()?"true":"false",static_cast<unsigned>(settings.idleSeconds),static_cast<unsigned>(runtime.power(esp_timer_get_time()/1000000)),unsigned(live.audio().interruptions.load()),unsigned(live.audio().voiceEnds.load()),unsigned(live.audio().voiceOverflows.load()),unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),static_cast<unsigned long long>(esp_timer_get_time()/1000000));
    }
#ifdef PASSPORT_DEVICE
    const size_t count=strlen(data);if(count+512<size){char boardState[512];board::diagnostics(boardState,sizeof(boardState));snprintf(data+count-1,size-count+1,",\"hardware\":%s}",boardState);}
    const size_t end=strlen(data);if(end+240<size)snprintf(data+end-1,size-end+1,",\"render_last_us\":%u,\"render_max_us\":%u,\"render_count\":%u,\"render_total_us\":%llu,\"loop_max_us\":%u,\"render_pixels_last\":%u,\"render_spi_us_last\":%u,\"page_prepare_us\":%u,\"page_render_us\":%u,\"page_spi_us\":%u}",renderLastUs.load(),renderMaxUs.load(),renderCount.load(),static_cast<unsigned long long>(renderTotalUs.load()),loopMaxUs.load(),renderPixelsLast.load(),renderSpiUsLast.load(),pagePrepareUs.load(),pageRenderUs.load(),pageSpiUs.load());
#ifdef PASSPORT_DEVICE
    {const size_t tail=strlen(data);if(tail+48<size)snprintf(data+tail-1,size-tail+1,",\"slides\":[%d,%u,%u]}",slidesBle.ready(),slidesBle.sent.load(),slidesBle.failed.load());}
    {char wake[96];tools::wakeStatus(wake,sizeof(wake));const size_t tail=strlen(data);if(tail+strlen(wake)+12<size)snprintf(data+tail-1,size-tail+1,",\"wake\":%s}",wake);}
    {char walkie[96];tools::walkieStatus(walkie,sizeof(walkie));const size_t tail=strlen(data);if(tail+strlen(walkie)+14<size)snprintf(data+tail-1,size-tail+1,",\"walkie\":%s}",walkie);}
#endif
    {const size_t tail=strlen(data);if(tail+80<size)snprintf(data+tail-1,size-tail+1,",\"wifi_saved\":%u,\"wifi_scans\":%u,\"wifi_joins\":%u,\"wifi_connecting\":%s}",runtime.model.wifiSavedCount,wifiScans.load(),wifiJoins.load(),wifiConnecting?"true":"false");}
    {const auto l=ledgerStatus();const size_t tail=strlen(data);if(tail+32<size)snprintf(data+tail-1,size-tail+1,",\"ledger\":[%u,%u]}",l.pending,unsigned(l.monthKnown));}
    {const uint64_t w=wordProgress.load();const size_t tail=strlen(data);if(tail+48<size)snprintf(data+tail-1,size-tail+1,",\"words\":[%u,%u,%u,%u]}",unsigned(w&255),unsigned(w>>8&255),unsigned(w>>16&65535),unsigned(w>>32&65535));}
    char battery[256];board::batteryDiagnostics(battery,sizeof(battery));
    const size_t batteryAt=strlen(data);if(batteryAt+strlen(battery)+12<size)snprintf(data+batteryAt-1,size-batteryAt+1,",\"battery\":%s}",battery);
#endif
}
void printState() { char data[2048]; state(data, sizeof(data)); printf("STATE %s\n", data); }
void wifiEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) { station = true; wifiConnecting = false; wifiReason = 0; wifiRetryBudget=0; wifiRetryAt=0; }
    if (base == WIFI_EVENT) {
#ifdef PASSPORT_DEVICE
        if(id==WIFI_EVENT_SCAN_DONE){wifiPortal.scanDone();autoScanDone=true;}
#endif
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            station = false;
            wifiReason = static_cast<wifi_event_sta_disconnected_t*>(data)->reason;
            if (wifiReason != WIFI_REASON_ASSOC_LEAVE) wifiConnecting = false;  // not the disconnect before a new join
            if (wifiRetryBudget>0) wifiRetryAt=esp_timer_get_time()+2000000;
        }
        if (id == WIFI_EVENT_AP_START) ap = true;
        if (id == WIFI_EVENT_AP_STOP) { ap = false; apClients = 0; }
        if (id == WIFI_EVENT_AP_STACONNECTED) ++apClients;
        if (id == WIFI_EVENT_AP_STADISCONNECTED && apClients > 0) --apClients;
    }
}
void wifiInit() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    staNetif = esp_netif_create_default_wifi_sta();
    apNetif = esp_netif_create_default_wifi_ap();
    esp_netif_dhcps_stop(apNetif); // New AP netif may already be stopped.
    esp_netif_ip_info_t ip{};
    IP4_ADDR(&ip.ip, 192,168,8,1); IP4_ADDR(&ip.gw,192,168,8,1); IP4_ADDR(&ip.netmask,255,255,255,0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(apNetif, &ip));
    // Default AP event glue starts DHCP when this interface comes up.
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEvent, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifiEvent, nullptr));
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    wifiInitialized=true;
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start()); ESP_ERROR_CHECK(wifiPowerSave());
}
int stationConnect(bool correct, bool automatic=false) {
    if (!automatic) wifiRetryBudget=correct ? 2 : 0;
    wifiRetryAt=0;
    if(wifiInitialized)esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
#ifndef PASSPORT_DEVICE
    // This provisioning flow ends SoftAP before joining WiFi. The emulator's
    // APSTA WPA2 re-authentication fails while its virtual AP client is active.
    // Do not claim concurrent re-authentication is supported by this profile.
    if(wifiInitialized) { ESP_ERROR_CHECK(esp_wifi_stop());ESP_ERROR_CHECK(esp_wifi_deinit());wifiInitialized=false; }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    wifiInitialized=true;
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
#else
    const int modeResult=esp_wifi_set_mode(board::provisioning()?WIFI_MODE_APSTA:WIFI_MODE_STA);
    if(modeResult)return modeResult;
#endif
    station = false; wifiReason = 0;
    wifi_config_t config{};
    // The network set up on the phone, else a saved one being joined, else the last joined.
    const char* ssid=joinActive?joinTarget.ssid:settings.wifiSsid;const char* stored=joinActive?joinTarget.password:settings.wifiPassword;
#ifdef PASSPORT_DEVICE
    if(wifiPending){ssid=pendingWifi.wifiSsid;stored=pendingWifi.wifiPassword;}
#endif
    memcpy(config.sta.ssid, ssid, strnlen(ssid, 32));
    const char* password = correct ? stored : "incorrect-test-password";
    memcpy(config.sta.password, password, strlen(password));
    config.sta.threshold.authmode = *password ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    int rc = esp_wifi_set_config(WIFI_IF_STA, &config);
#ifndef PASSPORT_DEVICE
    if (!rc) rc = esp_wifi_start();
#endif
    if (!rc) rc = wifiPowerSave();
    wifiRetryAt=0;
    if (!rc) { wifiConnecting = true; rc = esp_wifi_connect(); if (rc) wifiConnecting = false; }
    return rc;
}
#ifdef PASSPORT_DEVICE
// Joins saved network `index` (last joined first); once connected it becomes the first and the one tried at boot.
int joinSaved(unsigned index){
    auto list=std::unique_ptr<WifiList>(new(std::nothrow) WifiList);
    if(!list||!SettingsStore::loadWifi(*list)||index>=list->count)return -3;
    joinTarget=list->items[index];joinActive=true;++wifiJoins;*list={};
    return stationConnect(true);
}
// After a scan: the strongest saved network in range, if any, is joined.
void joinStrongest(){
    uint16_t count=20;auto records=std::unique_ptr<wifi_ap_record_t[]>(new(std::nothrow) wifi_ap_record_t[count]);
    if(!records){esp_wifi_clear_ap_list();return;}
    if(esp_wifi_scan_get_ap_records(&count,records.get())!=ESP_OK)return;
    auto list=std::unique_ptr<WifiList>(new(std::nothrow) WifiList);
    if(!list||!SettingsStore::loadWifi(*list))return;
    int best=-1,rssi=-128;
    for(unsigned i=0;i<count;++i){const int known=list->find(reinterpret_cast<const char*>(records[i].ssid));if(known>=0&&records[i].rssi>rssi){best=known;rssi=records[i].rssi;}}
    *list={};
    if(best>=0)joinSaved(best);
}
#endif
int setAp(bool enabled, bool correct = true) {
    #ifdef PASSPORT_DEVICE
    if(enabled&&board::provisioning())return 0;
    if(enabled){joinActive=false;memset(&joinTarget,0,sizeof(joinTarget));if(autoScanning){esp_wifi_scan_stop();autoScanning=false;}}
    if(enabled&&!station){wifiRetryBudget=0;wifiRetryAt=0;esp_wifi_disconnect();vTaskDelay(pdMS_TO_TICKS(100));}
    if(enabled&&bt){const int result=bleStop();if(result)return result;restoreBleAfterProvision=true;}
    if(!enabled){wifiPortal.end();esp_wifi_scan_stop();wifiPending=false;wifiRetryBudget=0;closePortalAt=0;memset(pendingWifi.wifiPassword,0,sizeof(pendingWifi.wifiPassword));}
    board::provisioning(enabled);
    if(enabled&&!wifiInitialized){wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();int rc=esp_wifi_init(&init);if(rc)return rc;wifiInitialized=true;esp_wifi_set_storage(WIFI_STORAGE_RAM);esp_wifi_set_mode(WIFI_MODE_APSTA);esp_wifi_start();}
    #endif
    if (!enabled) {
        const int result=esp_wifi_set_mode(WIFI_MODE_STA);
#ifdef PASSPORT_DEVICE
        if(restoreBleAfterProvision){restoreBleAfterProvision=false;bleStart();}
#endif
        return result;
    }
    if (ap) { esp_wifi_set_mode(WIFI_MODE_STA); vTaskDelay(pdMS_TO_TICKS(100)); }
    int rc = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (rc) return rc;
    wifi_config_t config{};
    #ifdef PASSPORT_DEVICE
    (void)correct;strcpy(reinterpret_cast<char*>(config.ap.ssid),board::apName());config.ap.ssid_len=strlen(board::apName());config.ap.authmode=WIFI_AUTH_OPEN;
    #else
    strcpy(reinterpret_cast<char*>(config.ap.ssid), "AI-Passport-Sim");
    strcpy(reinterpret_cast<char*>(config.ap.password), correct ? "passport123" : "wrong-hotspot-pass");
    config.ap.ssid_len = strlen("AI-Passport-Sim");
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    #endif
    config.ap.max_connection = 1; config.ap.channel = 6;
    const int result=esp_wifi_set_config(WIFI_IF_AP, &config);
#ifdef PASSPORT_DEVICE
    if(!result){wifiPortal.begin();runtime.model.wifiSetup=true;strcpy(runtime.model.wifiSetupStatus,"手机连接热点后选择 WiFi");}
#endif
    return result;
}
int gattAccess(uint16_t connection, uint16_t attribute, ble_gatt_access_ctxt* context, void*) {
    ble_gap_conn_desc detail{};ble_gap_conn_find(connection,&detail);
    printf("BLE_ACCESS conn=%u attr=%u op=%u flags=%u encrypted=%u authenticated=%u key_size=%u sc_only=%u\n",connection,attribute,context->op,unsigned(context->chr->flags),detail.sec_state.encrypted,detail.sec_state.authenticated,detail.sec_state.key_size,ble_hs_cfg.sm_sc_only);
    if (context->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        char copy[65]; portENTER_CRITICAL(&gattMux); memcpy(copy,gattValue,sizeof(copy)); portEXIT_CRITICAL(&gattMux);
        return os_mbuf_append(context->om, copy, strlen(copy)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    if (context->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        const auto length = OS_MBUF_PKTLEN(context->om);
        if (!length || length > 64) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        char copy[65]{}; uint16_t copied = 0;
        if (ble_hs_mbuf_to_flat(context->om, copy, 64, &copied)) return BLE_ATT_ERR_UNLIKELY;
        portENTER_CRITICAL(&gattMux); memcpy(gattValue,copy,sizeof(copy)); portEXIT_CRITICAL(&gattMux);
        ++writes; notify = true;
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}
#ifdef PASSPORT_DEVICE
// 蓝牙声纳: the NimBLE host task records advertisers here; the UI loop copies them once a second.
portMUX_TYPE sonarMux=portMUX_INITIALIZER_UNLOCKED;
Model::SonarDevice sonarSeen[Model::kSonarSlots];unsigned sonarSeenCount;bool scanning;
void sonarReport(const ble_gap_disc_desc& found){
    Model::SonarDevice update{};memcpy(update.addr,found.addr.val,6);update.rssi=found.rssi;update.seen=esp_timer_get_time()/1000000;
    ble_hs_adv_fields fields{};
    if(!ble_hs_adv_parse_fields(&fields,found.data,found.length_data)){
        size_t n=std::min<size_t>(fields.name_len,sizeof(update.name)-1);while(n&&(fields.name[n]&0xc0)==0x80&&n<fields.name_len)--n;
        memcpy(update.name,fields.name,n);update.name[n]=0;
        if(fields.mfg_data_len>=2)update.company=fields.mfg_data[0]|fields.mfg_data[1]<<8;
    }
    portENTER_CRITICAL(&sonarMux);
    // Scan responses arrive separately for the same address: merge, and smooth the RSSI.
    unsigned at=0;while(at<sonarSeenCount&&memcmp(sonarSeen[at].addr,update.addr,6))++at;
    if(at==sonarSeenCount){
        if(sonarSeenCount<std::size(sonarSeen))++sonarSeenCount;
        else for(unsigned i=at=0;i<sonarSeenCount;++i)if(sonarSeen[i].rssi<sonarSeen[at].rssi)at=i;  // full: drop the weakest
        sonarSeen[at]=update;
    }else{
        auto& d=sonarSeen[at];d.rssi=(d.rssi*3+update.rssi)/4;d.seen=update.seen;
        if(*update.name)strcpy(d.name,update.name);
        if(update.company)d.company=update.company;
    }
    portEXIT_CRITICAL(&sonarMux);
}
#endif
int gapEvent(ble_gap_event*, void*);
void advertise() {
    ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const char* name = ble_svc_gap_device_name();
#ifdef PASSPORT_DEVICE
    static const ble_uuid128_t voiceUuid=BLE_UUID128_INIT(0x10,0xb0,0xe3,0xdc,0xbd,0x4f,0x6c,0x9f,0x30,0x4e,0xed,0x70,0x6a,0xd6,0x67,0x5d);
    fields.uuids128=&voiceUuid;fields.num_uuids128=1;fields.uuids128_is_complete=1;
    // The PPT page is a standard HID keyboard; computers list it with a keyboard icon.
    static const ble_uuid16_t hidUuid=BLE_UUID16_INIT(0x1812);
    fields.uuids16=&hidUuid;fields.num_uuids16=1;fields.uuids16_is_complete=1;fields.appearance=0x03c1;fields.appearance_is_present=1;
    ble_hs_adv_fields scan{};scan.name=reinterpret_cast<const uint8_t*>(name);scan.name_len=strlen(name);scan.name_is_complete=1;ble_gap_adv_rsp_set_fields(&scan);
#else
    fields.name = reinterpret_cast<const uint8_t*>(name); fields.name_len = strlen(name); fields.name_is_complete = 1;
#endif
    const int configured = ble_gap_adv_set_fields(&fields);
    ble_gap_adv_params params{}; params.conn_mode = BLE_GAP_CONN_MODE_UND; params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    const int started = configured ? configured : ble_gap_adv_start(addressType, nullptr, BLE_HS_FOREVER, &params, gapEvent, nullptr);
    printf("BLE_ADV result=%d\n", started);advResult=started;
}
int gapEvent(ble_gap_event* event, void*) {
    if(event->type!=BLE_GAP_EVENT_DISC)printf("BLE_EVENT type=%d\n", event->type);
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        if (!event->connect.status) { linked = true; ++connects; connHandle = event->connect.conn_handle; dictationBle.connected(connHandle);
            // A 语音输入 audio report is 179 bytes: in 27-byte LL packets it takes seven, which the link cannot
            // carry 47 times a second. Longer LL packets send it in one.
            ble_gap_set_data_len(connHandle,251,2120);
            {ble_gap_conn_desc desc{};if(!ble_gap_conn_find(connHandle,&desc))linkInterval=desc.conn_itvl;linkMtu=0;linkOctets=27;}
            {int count=-1;ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC,&count);bonds=count;}
#ifdef PASSPORT_DEVICE
            bleCompanion.connected(connHandle);slidesBle.connected(connHandle);
            ble_gap_security_initiate(connHandle);
#endif
        }
        else advertise();
    }
    if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        linked = encrypted = false; connHandle = BLE_HS_CONN_HANDLE_NONE; ++disconnects; disconnectReason = event->disconnect.reason;
        dictationBle.disconnected();
#ifdef PASSPORT_DEVICE
        bleCompanion.disconnected();slidesBle.disconnected();
#endif
        if (bt) advertise();
    }
    if (event->type == BLE_GAP_EVENT_ENC_CHANGE) {
        ble_gap_conn_desc desc{};
        if (!ble_gap_conn_find(event->enc_change.conn_handle,&desc)) encrypted = desc.sec_state.encrypted;
        dictationBle.secured(encrypted);
#ifdef PASSPORT_DEVICE
        bleCompanion.secured(encrypted);slidesBle.secured(encrypted);
#endif
        ++securityEvents; encStatus = event->enc_change.status;
        printf("BLE_SECURITY status=%d encrypted=%d bonded=%d authenticated=%d key_size=%u sc_only=%u\n", event->enc_change.status,
               static_cast<int>(encrypted.load()), desc.sec_state.bonded, desc.sec_state.authenticated,desc.sec_state.key_size,ble_hs_cfg.sm_sc_only);
    }
    if(event->type==BLE_GAP_EVENT_CONN_UPDATE){ble_gap_conn_desc desc{};if(!ble_gap_conn_find(event->conn_update.conn_handle,&desc))linkInterval=desc.conn_itvl;}
    if(event->type==BLE_GAP_EVENT_MTU)linkMtu=event->mtu.value;
    if(event->type==BLE_GAP_EVENT_DATA_LEN_CHG)linkOctets=event->data_len_chg.max_tx_octets;
    if(event->type==BLE_GAP_EVENT_REPEAT_PAIRING) {
        ++repeatPairings;
        // A computer that removed the badge pairs again with fresh keys. Pairing is Just Works,
        // so refusing it protects nothing (an unknown computer may pair anyway) and only left the
        // Mac stuck on "等待电脑同意配对" off the PPT page.
        ble_gap_conn_desc desc{};
        if(!ble_gap_conn_find(event->repeat_pairing.conn_handle,&desc))ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
#ifdef PASSPORT_DEVICE
    if(event->type==BLE_GAP_EVENT_DISC){sonarReport(event->disc);return 0;}
    if(event->type==BLE_GAP_EVENT_DISC_COMPLETE)scanning=false;
#endif
    if(event->type==BLE_GAP_EVENT_SUBSCRIBE) {
        dictationBle.subscribed(event->subscribe.attr_handle,event->subscribe.cur_notify);
#ifdef PASSPORT_DEVICE
        slidesBle.subscribed(event->subscribe.attr_handle,event->subscribe.cur_notify);
#endif
    }
    return 0;
}
void onSync() {
    const int rc = ble_hs_util_ensure_addr(0);
    if (rc || ble_hs_id_infer_auto(0,&addressType)) { puts("BLE_SYNC_FAILED"); advResult=-101; return; }
    advertise();
    // Run the ACK after the entire host sync callback has returned.
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(),&bleReadyEvent);
}
void hostTask(void*) { nimble_port_run(); hostStopped = true; nimble_port_freertos_deinit(); }
int bleStart() {
    if (bt) return 0;
#ifdef PASSPORT_DEVICE
    // USB replaces BLE for the computer functions, but only BLE can type on the PPT computer.
    if(board::usbConnected()&&runtime.active()!=App::Slides&&runtime.active()!=App::Sonar)return -4;
    if(!bleCompanion.start())return -5;
#endif
    esp_log_level_set("NimBLE", ESP_LOG_INFO);
    const int rc = nimble_port_init(); if (rc) {
#ifdef PASSPORT_DEVICE
        bleCompanion.stop();
#endif
        return rc;
    }
    ble_hs_cfg.sync_cb = onSync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1; ble_hs_cfg.sm_sc = 1; ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init(); ble_svc_gatt_init();
#ifdef PASSPORT_DEVICE
    ble_svc_gap_device_name_set(board::bluetoothName());ble_svc_gap_device_appearance_set(0x03c1);
#else
    ble_svc_gap_device_name_set("AI-Passport-Sim");
#endif
    static ble_gatt_chr_def chars[2]{}; static ble_gatt_svc_def services[2]{};
    chars[0].uuid=&echoUuid.u; chars[0].access_cb=gattAccess;
    chars[0].flags=BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_WRITE_ENC;
    chars[0].min_key_size=16; chars[0].val_handle=&valueHandle;
    services[0].type=BLE_GATT_SVC_TYPE_PRIMARY; services[0].uuid=&serviceUuid.u; services[0].characteristics=chars;
    int result=ble_gatts_count_cfg(services); if (!result) result=ble_gatts_add_svcs(services);
    if(!result) result=ble_gatts_count_cfg(dictationBle.services());
    if(!result) result=ble_gatts_add_svcs(dictationBle.services());
#ifdef PASSPORT_DEVICE
    if(!result)result=ble_gatts_count_cfg(bleCompanion.services());
    if(!result)result=ble_gatts_add_svcs(bleCompanion.services());
    if(!result)result=ble_gatts_count_cfg(slidesBle.services());
    if(!result)result=ble_gatts_add_svcs(slidesBle.services());
#endif
    if (result) { nimble_port_deinit();
#ifdef PASSPORT_DEVICE
        bleCompanion.stop();
#endif
        return result;
    }
    ble_store_config_init(); bt=true; hostStopped=false; bleReady=false;
    ble_npl_event_init(&bleReadyEvent,[](ble_npl_event*){bleReady=true;},nullptr);
    nimble_port_freertos_init(hostTask);
    if(wifiInitialized)wifiPowerSave();
    for(unsigned i=0;i<500 && !bleReady;++i) vTaskDelay(pdMS_TO_TICKS(10));
    return bleReady ? 0 : -5;
}
int bleStop() {
    if (!bt) return 0;
    if (!bleReady) return -5; // Preserve an owner whose startup is unfinished.
    dictationBle.cancel(); bt=false; ble_gap_adv_stop();
    const int rc=nimble_port_stop(); if (rc) { bt=true; return rc; }
    if(wifiInitialized)wifiPowerSave();
    for (int i=0; i<100 && !hostStopped; ++i) vTaskDelay(pdMS_TO_TICKS(10));
    if (!hostStopped) return -1;
    // This NPL event belongs to the pool that deinit is about to destroy.
    // Release it first so a restart cannot alias an event allocated by NimBLE.
    ble_npl_event_deinit(&bleReadyEvent);
    const int result=nimble_port_deinit();
    linked=encrypted=bleReady=false; connHandle=BLE_HS_CONN_HANDLE_NONE;
#ifdef PASSPORT_DEVICE
    scanning=false;
#endif
    dictationBle.disconnected();
#ifdef PASSPORT_DEVICE
    bleCompanion.stop();slidesBle.disconnected();
#endif
    return result;
}
#ifdef PASSPORT_DEVICE
extern "C" int passport_memory_report(char*,size_t);
#endif
esp_err_t statusGet(httpd_req_t* request) {
    #ifdef PASSPORT_DEVICE
    if(!board::authorized(request))return httpd_resp_send_err(request,HTTPD_401_UNAUTHORIZED,"unauthorized");
    #endif
    // Static: the httpd task has under 1 KiB of stack to spare. Handlers run one at a time on that task.
    static char data[2304]; // BleExchange::capacity, the BLE path's limit for the same JSON.
#ifdef PASSPORT_DEVICE
    // ponytail: temporary memory survey; remove with passport_memory_report.
    if(strstr(request->uri,"?mem"))passport_memory_report(data,sizeof(data));else
#endif
    state(data,sizeof(data));
    httpd_resp_set_type(request,"application/json");
    return httpd_resp_sendstr(request,data);
}
#ifdef PASSPORT_DEVICE
// Status sign labels typed on the computer: at most four, each under 40 bytes, "" for none.
// Saved here; the queued "status_labels" command reloads them on the UI loop.
bool saveStatusLabels(const cJSON* object);bool saveCountdown(const cJSON*);bool saveQr(const cJSON*);
// Commands that carry content for a page save it before they are queued; the rest pass.
bool saveContent(const char* command,const cJSON* object){
    return !strcmp(command,"status_labels")?saveStatusLabels(object):!strcmp(command,"countdowns")?saveCountdown(object):!strcmp(command,"qr_code")?saveQr(object):true;
}
bool saveStatusLabels(const cJSON* object){
    const auto* labels=cJSON_GetObjectItemCaseSensitive(object,"labels");
    if(!cJSON_IsArray(labels)||cJSON_GetArraySize(labels)>StatusBoard::kCustom)return false;
    StatusBoard board;SettingsStore::loadStatus(board);memset(board.custom,0,sizeof(board.custom));
    unsigned i=0;const cJSON* item;
    cJSON_ArrayForEach(item,labels){
        if(!cJSON_IsString(item)||strlen(item->valuestring)>=sizeof(board.custom[0]))return false;
        strcpy(board.custom[i++],item->valuestring);
    }
    return SettingsStore::saveStatus(board);
}
// 倒数日 from the computer: at most six {name, date "YYYY-MM-DD", yearly, lunar} and the 下班 time "HH:MM" ("" = none).
bool saveCountdown(const cJSON* object){
    const auto* events=cJSON_GetObjectItemCaseSensitive(object,"events");const auto* offwork=cJSON_GetObjectItemCaseSensitive(object,"offwork");
    if(!cJSON_IsArray(events)||cJSON_GetArraySize(events)>CountdownBoard::kEvents||!cJSON_IsString(offwork))return false;
    CountdownBoard board{};int hour=0,minute=0;
    if(!*offwork->valuestring)board.offwork=0;
    else if(sscanf(offwork->valuestring,"%d:%d",&hour,&minute)!=2||hour<0||hour>23||minute<0||minute>59)return false;
    else board.offwork=hour*60+minute;
    unsigned i=0;const cJSON* event;
    cJSON_ArrayForEach(event,events){
        const auto* name=cJSON_GetObjectItemCaseSensitive(event,"name");const auto* date=cJSON_GetObjectItemCaseSensitive(event,"date");
        const bool lunar=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(event,"lunar"));int y=0,m=0,d=0;
        if(!cJSON_IsString(name)||!*name->valuestring||strlen(name->valuestring)>=sizeof(board.events[0].name)||!cJSON_IsString(date)
           ||sscanf(date->valuestring,"%d-%d-%d",&y,&m,&d)!=3||y<1901||y>2099||m<1||m>12||d<1||d>(lunar?30:monthDays(y,m)))return false;
        auto& out=board.events[i++];strcpy(out.name,name->valuestring);out.date=y*10000+m*100+d;
        out.flags=(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(event,"yearly"))?CountdownBoard::kYearly:0)|(lunar?CountdownBoard::kLunar:0);
    }
    return SettingsStore::saveCountdown(board);
}
// One QR entry from the computer: index 0-3, a title and its text ("" text removes it).
bool saveQr(const cJSON* object){
    const auto* index=cJSON_GetObjectItemCaseSensitive(object,"index");const auto* title=cJSON_GetObjectItemCaseSensitive(object,"title");const auto* text=cJSON_GetObjectItemCaseSensitive(object,"text");
    QrEntry entry{};
    if(!cJSON_IsNumber(index)||index->valueint<0||index->valueint>=QrEntry::kCount||!cJSON_IsString(title)||strlen(title->valuestring)>=sizeof(entry.title)
       ||!cJSON_IsString(text)||strlen(text->valuestring)>=sizeof(entry.text))return false;
    strcpy(entry.title,title->valuestring);strcpy(entry.text,text->valuestring);
    return SettingsStore::saveQr(index->valueint,entry);
}
void sonarTick(){
    const bool wanted=runtime.active()==App::Sonar&&bt&&bleReady;
    if(wanted&&!scanning){
        ble_gap_disc_params params{};params.itvl=0x60;params.window=0x30;params.filter_duplicates=0;  // active scan: names come in scan responses
        scanning=!ble_gap_disc(addressType,BLE_HS_FOREVER,&params,gapEvent,nullptr);
    }
    if(!wanted&&scanning){if(bt)ble_gap_disc_cancel();scanning=false;}
    static uint64_t copied;const uint64_t now=esp_timer_get_time()/1000000;auto& m=runtime.model;
    if(!wanted||now==copied)return;
    copied=now;
    portENTER_CRITICAL(&sonarMux);
    for(unsigned i=0;i<sonarSeenCount;)if(now-sonarSeen[i].seen>30)sonarSeen[i]=sonarSeen[--sonarSeenCount];else ++i;  // gone for 30 s
    memcpy(m.sonar.data(),sonarSeen,sizeof(sonarSeen));m.sonarCount=sonarSeenCount;
    portEXIT_CRITICAL(&sonarMux);
    int8_t rssi=0;m.sonarPeerRssi=linked&&!ble_gap_conn_rssi(connHandle,&rssi)?rssi:0;
}
// 语音记账: the computer's assistant hands back what it saved ({"ack":[[id,rev],...],"month":[out,in,yyyymm]}) and
// gets the entries it has not got yet.
int ledgerRequest(const cJSON* data,char* output,size_t size){
    LedgerAck acks[8];unsigned count=0;
    const auto* list=cJSON_GetObjectItemCaseSensitive(data,"ack");const cJSON* item=nullptr;
    cJSON_ArrayForEach(item,list){
        if(count==std::size(acks)||cJSON_GetArraySize(item)!=2)return 400;
        const auto* id=cJSON_GetArrayItem(item,0);const auto* rev=cJSON_GetArrayItem(item,1);
        if(!cJSON_IsNumber(id)||!cJSON_IsNumber(rev)||id->valuedouble<0||id->valuedouble>4294967295.0||rev->valuedouble<0||rev->valuedouble>255)return 400;
        acks[count++]={static_cast<uint32_t>(id->valuedouble),static_cast<uint8_t>(rev->valuedouble)};
    }
    int32_t out=0,in=0;uint32_t month=0;
    const auto* totals=cJSON_GetObjectItemCaseSensitive(data,"month");
    if(cJSON_GetArraySize(totals)==3&&cJSON_IsNumber(cJSON_GetArrayItem(totals,2))){
        out=static_cast<int32_t>(cJSON_GetArrayItem(totals,0)->valuedouble);in=static_cast<int32_t>(cJSON_GetArrayItem(totals,1)->valuedouble);
        month=static_cast<uint32_t>(cJSON_GetArrayItem(totals,2)->valuedouble);
    }
    ledgerExchange(acks,count,out,in,month,output,size);
    return 200;
}
int companionRequest(const char* raw,char* output,size_t size){
    snprintf(output,size,"{}");
    if(!BleExchange::boundedJson(raw))return 400;
    auto* object=cJSON_Parse(raw);if(!object)return 400;
    struct Cleanup{cJSON* value;~Cleanup(){cJSON_Delete(value);}}cleanup{object};
    const auto* token=cJSON_GetObjectItemCaseSensitive(object,"token");
    if(!cJSON_IsString(token)||strlen(token->valuestring)!=64)return 401;
    unsigned difference=0;for(unsigned i=0;i<64;++i)difference|=token->valuestring[i]^board::apiToken()[i];
    if(difference)return 401;
    const auto* path=cJSON_GetObjectItemCaseSensitive(object,"path");if(!cJSON_IsString(path))return 400;
    live.hostSeen();
    if(!strcmp(path->valuestring,"/status")){state(output,size);return 200;}
    if(!strcmp(path->valuestring,"/events")){board::nextEvent(output,size);return 200;}
    if(!strcmp(path->valuestring,"/app_state"))return live.snapshotJson(output,size)?200:500;
    if(!strcmp(path->valuestring,"/wifi/status")){
        uint8_t mac[6];esp_read_mac(mac,ESP_MAC_WIFI_STA);
        snprintf(output,size,"{\"firmware\":\"%s\",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}",esp_app_get_description()->version,mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);return 200;
    }
    auto* data=cJSON_GetObjectItemCaseSensitive(object,"data");if(!cJSON_IsObject(data))return 400;
    if(!strcmp(path->valuestring,"/ledger"))return ledgerRequest(data,output,size);
    if(!strcmp(path->valuestring,"/service")){
        char* body=cJSON_PrintUnformatted(data);if(!body)return 500;
        const bool accepted=live.enqueueComputer(body);free(body);
        if(accepted)snprintf(output,size,"{\"queued\":true}");
        return accepted?200:500;
    }
    if(!strcmp(path->valuestring,"/command")){
        const auto* name=cJSON_GetObjectItemCaseSensitive(data,"command");const auto* id=cJSON_GetObjectItemCaseSensitive(data,"id");
        if(!cJSON_IsString(name)||!cJSON_IsNumber(id)||id->valuedouble<1||id->valuedouble>4294967295.0||id->valuedouble!=static_cast<unsigned>(id->valuedouble))return 400;
        const char* allowed[]={"up","down","ok","back","settings","wake","refresh_service","status_labels","countdowns","qr_code"};bool valid=false;
        for(auto* value:allowed)valid|=!strcmp(name->valuestring,value);
        if(!valid||!saveContent(name->valuestring,data))return 400;
        Command command{};command.id=static_cast<unsigned>(id->valuedouble);strcpy(command.name,name->valuestring);
        if(xQueueSend(commands,&command,0)!=pdTRUE)return 500;
        snprintf(output,size,"{\"queued\":true}");return 200;
    }
    return 404;
}
#endif
#ifndef PASSPORT_DEVICE
void consoleTask(void*) {
    char line[200]{}; unsigned used=0;uint8_t ppp[128];
    for (;;) {
        uint8_t c;
        while (esp_rom_output_rx_one_char(&c)==0) {
            if (c=='\n') {
                line[used]=0; Command command{};
                if(!strncmp(line,"PPP_RX ",7)) {
                    size_t count=0;
                    if(!mbedtls_base64_decode(ppp,sizeof(ppp),&count,reinterpret_cast<const uint8_t*>(line+7),used-7))pppLink.input(ppp,count);
                } else if (sscanf(line,"%u %39s",&command.id,command.name)==2) xQueueSend(commands,&command,0);
                used=0;
            } else if (c!='\r' && used<sizeof(line)-1) line[used++]=c;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        consoleStack=uxTaskGetStackHighWaterMark(nullptr);
    }
}
#endif
#ifdef PASSPORT_DEVICE
// Start show tries PowerPoint / WPS on Windows (F5), PowerPoint on Mac (Cmd+Shift+Return), then Keynote (Option+Cmd+P).
bool sendSlideKey(SlideKey key){
    if(!slidesBle.ready())return false;
    switch(key){
    case SlideKey::Previous:return slidesBle.tap(0,BleSlides::Left);
    case SlideKey::Next:return slidesBle.tap(0,BleSlides::Right);
    case SlideKey::Black:return slidesBle.tap(0,BleSlides::B);
    case SlideKey::End:return slidesBle.tap(0,BleSlides::Escape);
    case SlideKey::Start:{
        bool ok=slidesBle.tap(0,BleSlides::F5);vTaskDelay(pdMS_TO_TICKS(200));
        ok&=slidesBle.tap(BleSlides::Gui|BleSlides::Shift,BleSlides::Return);vTaskDelay(pdMS_TO_TICKS(200));
        return slidesBle.tap(BleSlides::Gui|BleSlides::Alt,BleSlides::P)&&ok;
    }
    }
    return false;
}
#endif
int execute(const char* name) {
    const uint64_t now=esp_timer_get_time()/1000000;
    const auto previous=runtime.active();

#ifdef PASSPORT_DEVICE
    const auto settingsPageBefore=runtime.model.settingsPage;
    const auto volumeBefore=runtime.model.volume;
    // Settings > WiFi: Confirm on 添加 WiFi starts the setup hotspot (and again stops it), on a saved network joins it;
    // a long Down forgets that network (asked twice).
    if(previous==App::Settings&&settingsPageBefore==SettingsPage::Wifi&&!strcmp(name,"ok")){
        runtime.wake(now);runtime.model.wifiArmed=false;runtime.model.lastNotice[0]=0;
        if(board::provisioning()){const int result=setAp(false);if(!station&&*settings.wifiSsid)stationConnect(true);return result;}
        if(!runtime.model.wifiRow)return setAp(true);
        snprintf(runtime.model.lastNotice,sizeof(runtime.model.lastNotice),"正在连接 %s",runtime.model.wifiSaved[runtime.model.wifiRow-1]);
        return joinSaved(runtime.model.wifiRow-1);
    }
    if(previous==App::Settings&&settingsPageBefore==SettingsPage::Wifi&&!strcmp(name,"settings")&&!board::provisioning()){runtime.wake(now);runtime.forgetWifi();return 0;}
    if(previous==App::Settings&&settingsPageBefore==SettingsPage::Wifi&&!strcmp(name,"back")&&board::provisioning()){closePortalAt=0;setAp(false);if(!station&&*settings.wifiSsid)stationConnect(true);}
#endif
    // Long Confirm in the podcast player only goes back to its list; the page and its task stay.
    const bool leavingAudio=!strcmp(name,"back") && profile(previous).audio && !(previous==App::Podcast&&runtime.model.podcastPlayer);
    const bool enteringAudio=!strcmp(name,"ok") && previous==App::Menu && profile(kFunctions[runtime.selection()]).audio && kFunctions[runtime.selection()]!=App::Dictation;
    // 语音输入: Confirm starts and ends speaking; on the error screen it reconnects instead (LiveServices).
    if(previous==App::Dictation && !strcmp(name,"ok") && runtime.model.livePhase!=LivePhase::Error) return dictationBle.toggle(runtime.generation());
    if(previous==App::Dictation && !strcmp(name,"back")) dictationBle.cancel();
    // Its shortcuts on the computer's input line: Down presses Return, Up deletes the whole line (between phrases).
    if(previous==App::Dictation && (!strcmp(name,"up")||!strcmp(name,"down"))){
        runtime.wake(now);
        const int result=runtime.model.voiceBusy()?-9:dictationBle.key(runtime.generation(),!strcmp(name,"down")?BleDictation::Return:BleDictation::ClearLine);
        if(!result)runtime.model.dictationAt=now;
        return result;
    }
    if(leavingAudio && !live.stopAudio()) return static_cast<int>(Result::StopTimeout);
    if(enteringAudio && bt) {
        if(bleStop()) return -4;
        vTaskDelay(pdMS_TO_TICKS(10)); // Allow the idle task to reclaim the stopped BLE task.
        restoreBleAfterAudio=true; puts("BLE_PAUSED_FOR_AUDIO");
    }
    if (!strcmp(name,"wake")) {runtime.wake(now);return 0;}
    // The radio reuses the long Up/Down presses (otherwise hotspot / settings shortcuts): long Up tunes to the next station
    // (a podcast: the next episode), long Down steps the sleep timer.
    if ((previous==App::Radio||previous==App::Podcast)&&!strcmp(name,"ap_on")) {live.station(true);return 0;}
    if ((previous==App::Radio||previous==App::Podcast)&&!strcmp(name,"settings")) {runtime.stepMediaTimer(now);return 0;}
    // The word page grades with them: long Up = known, long Down = not known.
    if (previous==App::Words&&(!strcmp(name,"ap_on")||!strcmp(name,"settings"))) {runtime.gradeWord(!strcmp(name,"ap_on"),now);return 0;}
    // The PPT page: long Up = black screen, long Down = end show.
    if (previous==App::Slides&&(!strcmp(name,"ap_on")||!strcmp(name,"settings"))) {
        runtime.slideKey(!strcmp(name,"ap_on")?SlideKey::Black:SlideKey::End,now);
#ifdef PASSPORT_DEVICE
        runtime.model.slidesFailed=!sendSlideKey(runtime.model.slidesLast);
#endif
        return runtime.model.slidesFailed?-7:0;
    }
    // 随机转盘, 倒数日, 汇率 and 自选股: a long Up speaks a mode or options, an event, a currency, a security; a long Down
    // (twice) deletes the one on show, below (自选股's through the device's task).
    if ((previous==App::Wheel||previous==App::Countdown||previous==App::Rate||previous==App::Stock)&&!strcmp(name,"ap_on")) {runtime.wake(now);live.speak();return 0;}
    if(previous==App::Macro&&(!strcmp(name,"ap_on")||!strcmp(name,"settings"))){
        runtime.wake(now);
        if(runtime.model.macroDetail){if(!strcmp(name,"ap_on"))live.refresh();}
        else runtime.longKey(!strcmp(name,"ap_on"),now);
        return 0;
    }
    if (previous==App::Stock&&!strcmp(name,"settings")) {runtime.wake(now);live.dropStock();return 0;}
    // 离座锁屏: a long Up calibrates the at-desk signal, a long Down locks the computer now.
    if (previous==App::Guard&&(!strcmp(name,"ap_on")||!strcmp(name,"settings"))) {runtime.wake(now);live.guardCommand(!strcmp(name,"ap_on"));return 0;}
    // 中英翻译: a long Up reads the translation aloud.
    if (previous==App::Translate&&!strcmp(name,"ap_on")) {runtime.wake(now);live.say();return 0;}
    // Long presses: the menu turns a page, the calendar steps a month, the sonar switches its filter,
    // the WiFi toolbox and the sound page change tab, the motion monitor turns its alarm on or off.
    // 白噪音 sets the volume with them, 闹钟 the hour; 语音笔记 deletes the open note with a long Down; 状态牌 switches 常亮.
    if ((previous==App::Menu||previous==App::Calendar||previous==App::Sonar||previous==App::Wifi||previous==App::Sound||previous==App::Motion||previous==App::Noise||previous==App::Alarm||previous==App::Memo||previous==App::Status||previous==App::Wheel||previous==App::Todo||previous==App::Ledger||previous==App::Calc||previous==App::Rate||previous==App::Countdown||previous==App::Walkie)&&(!strcmp(name,"ap_on")||!strcmp(name,"settings"))) {
        runtime.longKey(!strcmp(name,"ap_on"),now);
#ifdef PASSPORT_DEVICE
        if(previous==App::Noise&&!board::setVolume(runtime.model.volume)){runtime.model.volume=board::volume();return -10;}
#endif
        return 0;
    }
    if (!strcmp(name,"up")) runtime.key(Key::Up,now);
    else if (!strcmp(name,"down")) runtime.key(Key::Down,now);
    else if (!strcmp(name,"ok")) runtime.key(Key::Confirm,now);
    else if (!strcmp(name,"back")) runtime.key(Key::Back,now);
    else if (!strcmp(name,"settings")) runtime.key(Key::OpenSettings,now);
    else if(!strcmp(name,"refresh_service")){live.refresh();return 0;}
    else if(!strcmp(name,"status_labels")){runtime.reloadStatus();return 0;}
    else if(!strcmp(name,"countdowns")){if(runtime.active()==App::Countdown)runtime.reloadCountdown();return 0;}
    else if(!strcmp(name,"qr_code")){if(runtime.active()==App::Qr)runtime.reloadQr();return 0;}
    else if (!strcmp(name,"wifi_connect")) return stationConnect(true);
    else if (!strcmp(name,"wifi_bad_password")) return stationConnect(false);
    else if (!strcmp(name,"wifi_disconnect")) { wifiRetryBudget=0; wifiRetryAt=0; return wifiInitialized?esp_wifi_disconnect():0; }
    #ifdef PASSPORT_DEVICE
    else if (!strcmp(name,"ppp_on")) return pppLink.startUsb();
    #else
    else if (!strcmp(name,"ppp_on")) return pppLink.start([](void*,const uint8_t* bytes,size_t size)->esp_err_t {
        // Simulator HAL only: the same PPP bytes use USB Serial/JTAG on hardware.
        for(size_t offset=0;offset<size;offset+=96) {
            uint8_t encoded[132];size_t length=0;
            if(mbedtls_base64_encode(encoded,sizeof(encoded),&length,bytes+offset,std::min<size_t>(96,size-offset)))return ESP_FAIL;
            encoded[length]=0;printf("PPP_TX %s\n",encoded);
        }
        return ESP_OK;
    },nullptr);
    #endif
    else if (!strcmp(name,"ppp_off")) return pppLink.stop();
    else if (!strcmp(name,"ppp_probe")) return pppLink.probe();
    else if (!strcmp(name,"usb_connect")) { runtime.connection(true,now);return 0; }
    else if (!strcmp(name,"usb_disconnect")) { runtime.connection(false,now);pppLink.linkLost();return 0; }
    else if (!strcmp(name,"ap_on")) {
#ifdef PASSPORT_DEVICE
        if(previous!=App::Home&&previous!=App::Menu&&previous!=App::Settings)return -4;
        runtime.key(Key::OpenSettings,now);runtime.model.settingsPage=SettingsPage::Wifi;board::setBrightness(settings.brightness);
#endif
        return setAp(true);
    }
    else if (!strcmp(name,"ap_wrong_password")) return setAp(true,false);
    else if (!strcmp(name,"ap_off")) return setAp(false);
    else if (!strcmp(name,"ble_on")) return profile(runtime.active()).audio && runtime.active()!=App::Dictation ? -4 : bleStart();
    else if (!strcmp(name,"ble_off")) return bleStop();
    else if (!strcmp(name,"save_exit")) { puts("SIM_RESTART_SAFE"); return 0; }
    else if (!strcmp(name,"scan")) {
        wifi_scan_config_t config{}; int rc=esp_wifi_scan_start(&config,true);
        if (rc) return rc;
        wifi_ap_record_t records[8]{}; uint16_t count=8;
        rc=esp_wifi_scan_get_ap_records(&count,records);
        bool found=false; for (int i=0;i<count;++i) found |= !strcmp(reinterpret_cast<const char*>(records[i].ssid),settings.wifiSsid);
        printf("WIFI_SCAN count=%u configured_ssid_found=%d\n",count,found);
        return found ? rc : -2;
    } else return -3;
    if(leavingAudio && restoreBleAfterAudio) {
#ifdef PASSPORT_DEVICE
        if(live.nativeIdle()){
#endif
        restoreBleAfterAudio=false;
        const int rc=bleStart(); if(rc) return rc;
        puts("BLE_RESTORED_AFTER_AUDIO");
#ifdef PASSPORT_DEVICE
        }
#endif
    }
    if(previous==runtime.active()) {
#ifdef PASSPORT_DEVICE
        if(previous==App::Settings){
            const auto page=runtime.model.settingsPage;
            if(!board::setBrightness(page==SettingsPage::Brightness?runtime.model.settingsValue:settings.brightness))return -10;
            if(runtime.model.volume!=volumeBefore&&!board::setVolume(runtime.model.volume)){runtime.model.volume=volumeBefore;strcpy(runtime.model.lastNotice,"保存失败，请重试");return -10;}
        }
        if((previous==App::Chat||previous==App::Radio||previous==App::Podcast||previous==App::Walkie)&&(!strcmp(name,"up")||!strcmp(name,"down"))&&!board::setVolume(runtime.model.volume)){runtime.model.volume=board::volume();return -10;}
        if(previous==App::Slides&&(!strcmp(name,"up")||!strcmp(name,"down")||!strcmp(name,"ok"))){runtime.model.slidesFailed=!sendSlideKey(runtime.model.slidesLast);if(runtime.model.slidesFailed)return -7;}
#endif
        if(!strcmp(name,"ok")) live.action(Key::Confirm);
        if(!strcmp(name,"up")) live.action(Key::Up);
        if(!strcmp(name,"down")) live.action(Key::Down);
    }
    return static_cast<int>(runtime.lastResult);
}
}
extern "C" void app_main() {
    #ifdef PASSPORT_DEVICE
    puts("DEVICE_BOOT_OK C++ C3 8MB PSRAM=0");
    #else
    puts("SIM_BOOT_OK suite-connectivity C++ C3 8MB PSRAM=0 physical_device=0");
    #endif
    ESP_ERROR_CHECK(nvs_flash_init()); SettingsStore store; bool created=false;
    configASSERT(store.loadOrCreate(settings,created));
    #ifndef PASSPORT_DEVICE
    wifiInit(); stationConnect(true);
    #endif
    #ifdef PASSPORT_DEVICE
    board::init(settings);
    #endif
    identity(); // Registers the identity flash region before HTTP handlers can write it.
    uiInit(); configASSERT(runtime.init(settings));
    // 已保存的 WiFi: a board set up before 0.4.9 starts its list with the network it already has.
    if(*settings.wifiSsid)runtime.rememberWifi(settings.wifiSsid,settings.wifiPassword);else runtime.reloadWifi();
#ifdef PASSPORT_DEVICE
    // A timer wake is the alarm's early call to set the clock: stay dark (the alarm, or a key, lights it).
    if(esp_sleep_get_wakeup_cause()==ESP_SLEEP_WAKEUP_TIMER)runtime.darken();
#endif
#ifdef PASSPORT_DEVICE
    runtime.model.volume=board::volume();
#endif
    commands=xQueueCreate(16,sizeof(Command)); configASSERT(commands);
    #ifdef PASSPORT_DEVICE
    wifiInit(); if(*settings.wifiSsid)stationConnect(true);
    #else
    xTaskCreate(consoleTask,"sim_console",3072,nullptr,3,nullptr);
    #endif
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 4096;
    config.max_uri_handlers = 22;
    config.uri_match_fn=httpd_uri_match_wildcard;
#ifdef PASSPORT_DEVICE
    config.stack_size=6144;config.lru_purge_enable=true;
#endif
    httpd_handle_t server = nullptr;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    httpd_uri_t status{};
    status.uri = "/status";
    status.method = HTTP_GET;
    status.handler = statusGet;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));
    live.init(server);
#ifdef PASSPORT_DEVICE
    httpd_uri_t pair{};pair.uri="/pair";pair.method=HTTP_POST;pair.handler=board::pair;ESP_ERROR_CHECK(httpd_register_uri_handler(server,&pair));pair.method=HTTP_GET;ESP_ERROR_CHECK(httpd_register_uri_handler(server,&pair));
    httpd_uri_t ledger{};ledger.uri="/ledger";ledger.method=HTTP_POST;
    ledger.handler=[](httpd_req_t* request)->esp_err_t{
        if(!board::authorized(request))return httpd_resp_send_err(request,HTTPD_401_UNAUTHORIZED,"unauthorized");
        if(request->content_len<=0||request->content_len>1024)return httpd_resp_send_err(request,HTTPD_400_BAD_REQUEST,"size");
        // Static, as /status: handlers run one at a time on the httpd task.
        static char raw[1025],out[1600];memset(raw,0,sizeof(raw));
        for(int used=0;used<request->content_len;){const int n=httpd_req_recv(request,raw+used,request->content_len-used);if(n<=0)return ESP_FAIL;used+=n;}
        live.hostSeen();
        auto* data=cJSON_Parse(raw);
        const int status=cJSON_IsObject(data)?ledgerRequest(data,out,sizeof(out)):400;cJSON_Delete(data);
        if(status!=200)return httpd_resp_send_err(request,HTTPD_400_BAD_REQUEST,"ledger");
        httpd_resp_set_type(request,"application/json");return httpd_resp_sendstr(request,out);
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&ledger));
    httpd_uri_t events{};
    events.uri = "/events";
    events.method = HTTP_GET;
    events.handler = [](httpd_req_t* request)->esp_err_t {if(board::authorized(request))live.hostSeen();return board::events(request);};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &events));
    httpd_uri_t action{};
    action.uri = "/command";
    action.method = HTTP_POST;
    action.handler = [](httpd_req_t *request) -> esp_err_t {
        const bool bluetoothSetup = !strcmp(request->uri, "/bluetooth");
        const bool setup = !strcmp(request->uri, "/wifi") || bluetoothSetup;
        bool permitted = board::authorized(request);
        if (setup) {
            permitted = wifiPortal.acceptsSetup(request);
            char origin[80]{};
            if (httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) == ESP_OK &&
                strcmp(origin, "http://192.168.8.1"))
                permitted = false;
        }
        if (!permitted)
            return httpd_resp_send_err(request, HTTPD_401_UNAUTHORIZED, "unauthorized");
        // Static: a QR text plus JSON outgrows what the httpd task's stack can spare. Handlers run one at a time.
        if (request->content_len <= 0 || request->content_len > 1024)
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "size");
        static char raw[1025];
        memset(raw, 0, sizeof(raw));
        int used = 0;
        while (used < request->content_len) {
            int n = httpd_req_recv(request, raw + used, request->content_len - used);
            if (n <= 0)
                return ESP_FAIL;
            used += n;
        }
        auto *object = cJSON_Parse(raw);
        if (!object)
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "json");
        auto *name = cJSON_GetObjectItemCaseSensitive(object, "command");
        auto *id = cJSON_GetObjectItemCaseSensitive(object, "id");
        Command cmd{};
        if (!setup &&
            (!cJSON_IsString(name) || strlen(name->valuestring) >= sizeof(cmd.name) ||
             !cJSON_IsNumber(id) || id->valuedouble < 1 || id->valuedouble > 4294967295.0 ||
             id->valuedouble != static_cast<unsigned>(id->valuedouble))) {
            cJSON_Delete(object);
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "command");
        }
        cmd.id = setup ? 0 : static_cast<unsigned>(id->valuedouble);
        snprintf(cmd.name, sizeof(cmd.name), "%s", bluetoothSetup ? "set_bluetooth" : setup ? "configure_wifi" : name->valuestring);
        const char *allowed[] = {"up",
                                 "down",
                                 "ok",
                                 "back",
                                 "settings",
                                 "wake",
                                 "wifi_connect",
                                 "wifi_disconnect",
                                 "ap_on",
                                 "ap_off",
                                 "ble_on",
                                 "ble_off",
                                 "scan",
                                 "ppp_probe",
                                 "configure_wifi","refresh_service","set_bluetooth","status_labels","countdowns","qr_code"};
        bool known = false;
        for (auto *value : allowed)
            known |= !strcmp(value, cmd.name);
        if (!known) {
            cJSON_Delete(object);
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "unsupported");
        }
        if (!saveContent(cmd.name, object)) {
            cJSON_Delete(object);
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "labels");
        }
        if (!strcmp(cmd.name, "set_bluetooth")) {
            const auto* enabled=cJSON_GetObjectItemCaseSensitive(object,"enabled");
            if(!cJSON_IsBool(enabled)){cJSON_Delete(object);return httpd_resp_send_err(request,HTTPD_400_BAD_REQUEST,"bluetooth");}
            cmd.bluetooth=cJSON_IsTrue(enabled)?1:0;
        }
        if (!strcmp(cmd.name, "configure_wifi")) {
            auto *ssid = cJSON_GetObjectItemCaseSensitive(object, "ssid");
            auto *password = cJSON_GetObjectItemCaseSensitive(object, "password");
            auto *hex=cJSON_GetObjectItemCaseSensitive(object,"ssid_hex");
            const bool ssidValid=cJSON_IsString(hex)?portal::decodeSsid(hex->valuestring,cmd.ssid):cJSON_IsString(ssid)&&*ssid->valuestring&&strlen(ssid->valuestring)<=32;
            if (!ssidValid||!cJSON_IsString(password)||!portal::passwordValid(password->valuestring)) {
                cJSON_Delete(object);
                return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "wifi");
            }
            if(!cJSON_IsString(hex))strcpy(cmd.ssid, ssid->valuestring);
            strcpy(cmd.password, password->valuestring);
            const auto* location=cJSON_GetObjectItemCaseSensitive(object,"ip_location_allowed");
            if(location&&!cJSON_IsBool(location)){cJSON_Delete(object);return httpd_resp_send_err(request,HTTPD_400_BAD_REQUEST,"location consent");}
            if(location)cmd.locationConsent=cJSON_IsTrue(location)?1:0;
        }
        cJSON_Delete(object);
        if (xQueueSend(commands, &cmd, 0) != pdTRUE)
            return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "queue");
        return httpd_resp_sendstr(request, "{\"queued\":true}");
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &action));
    action.uri = "/wifi";
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &action));
    action.uri = "/bluetooth";
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &action));
    wifiPortal.init(server,apNetif);
#endif
    uint64_t last = 0;
    int64_t lastEmotion = 0;
    bool previousLink = false;
    auto previousPower = runtime.power(0);
    int64_t loopAt=0;
    for (;;) {
        // Longest gap between loop passes: key handling and redraws wait on it.
        {const auto at=esp_timer_get_time();if(loopAt)loopMaxUs=std::max<unsigned>(loopMaxUs.load(),static_cast<unsigned>(at-loopAt));loopAt=at;}
#ifdef PASSPORT_DEVICE
        const auto nowBoard = esp_timer_get_time() / 1000000;
        const bool usb = board::usbConnected();
        if(runtime.connected()!=usb&&needsComputer(runtime.active())){dictationBle.cancel();live.reconnectComputer();}
        dictationBle.usbMode(usb);
        if(usb&&runtime.active()!=App::Slides&&runtime.active()!=App::Sonar){if(bt)bleStop();restoreBleAfterAudio=false;}
        if (runtime.connected() && !usb)
            pppLink.linkLost();
        runtime.connection(usb, nowBoard);
        if(pppLink.active()&&pppLink.dead())pppLink.stop();
        static int64_t nextPppStart = 5000000;
        if (usb && !pppLink.active() && esp_timer_get_time() >= nextPppStart) {
            pppLink.startUsb();
            nextPppStart = esp_timer_get_time() + 3000000;
        }
        char inputPairCode[7]{};uint32_t inputPairEpoch{};const bool inputPairing=board::pairCode(inputPairCode,&inputPairEpoch);
        board::keyMode(runtime.active()==App::Chat,runtime.generation()*8+static_cast<unsigned>(runtime.model.settingsPage),runtime.power(nowBoard)==Power::Awake||runtime.active()==App::Slides,inputPairEpoch,runtime.active()==App::Home&&!inputPairing);
        if (const char *name = board::key()) {
            char currentPairCode[7]{};uint32_t currentPairEpoch{};const bool currentPairing=board::pairCode(currentPairCode,&currentPairEpoch);
            if(inputPairing||currentPairing) {
                if(inputPairing&&currentPairing&&inputPairEpoch==currentPairEpoch&&(!strcmp(name,"ok")||!strcmp(name,"back")))board::pairKey(!strcmp(name,"ok"),inputPairEpoch);
            } else {
            Command cmd{};
            cmd.physical=true;cmd.inputContext=board::keyContext();cmd.inputGeneration=runtime.generation();
            snprintf(cmd.name, sizeof(cmd.name), "%s", name);
            xQueueSend(commands, &cmd, 0);
            }
        }
        char pairCode[7]{};board::pairCode(pairCode);if(strcmp(pairCode,runtime.model.pairingCode)){strcpy(runtime.model.pairingCode,pairCode);dirty=true;}
        bleCompanion.poll(companionRequest);
#endif
        Command command{};
        if (xQueueReceive(commands, &command, pdMS_TO_TICKS(20)) == pdTRUE) {
            int rc = 0;
#ifdef PASSPORT_DEVICE
            char pendingPairCode[7]{};
            if(command.physical&&(command.inputGeneration!=runtime.generation()||command.inputContext!=board::keyContext()||board::pairCode(pendingPairCode)))continue;
            if (!strcmp(command.name, "set_bluetooth")) {
                // Same record and effect as Settings > Bluetooth; the loop starts/stops BLE.
                runtime.wake(esp_timer_get_time()/1000000);auto candidate=settings;candidate.bluetoothEnabled=command.bluetooth==1;SettingsStore store;
                rc=command.bluetooth<0?-3:store.save(candidate)?0:-10;if(!rc)settings=candidate;
            } else if (!strcmp(command.name, "configure_wifi")) {
                runtime.wake(esp_timer_get_time()/1000000);
                pendingWifi=settings;strcpy(pendingWifi.wifiSsid,command.ssid);strcpy(pendingWifi.wifiPassword,command.password);
                wifiPending=true;pendingLocationConsent=command.locationConsent;closePortalAt=0;wifiJoinDeadline=esp_timer_get_time()+45000000;
                esp_wifi_scan_stop();wifiPortal.phase("connecting","正在连接 WiFi",pendingWifi.wifiSsid);strcpy(runtime.model.wifiSetupStatus,"正在连接 WiFi…");
                rc=stationConnect(true);
                if(rc){wifiPending=false;wifiPortal.phase("failed","无法开始连接，请重新扫描后重试",command.ssid);}
            } else
#endif
                rc = execute(command.name);
            actionResult = rc;
            dirty = true;
            appId = static_cast<int>(runtime.active());
            selection = runtime.selection();
            {const auto& w=runtime.model;wordProgress=w.wordSession.cursor|w.wordSession.size<<8|uint64_t(w.words.next)<<16|uint64_t(w.words.mastered)<<32;}
            appGeneration = runtime.generation();
            if (command.id)
                completedCommand = command.id;
            printf("ACTION_DONE {\"id\":%u,\"command\":\"%s\",\"result\":%d}\n", command.id,
                   command.name, rc);
        }
        const auto retryAt = wifiRetryAt.load();
#ifndef PASSPORT_DEVICE
        if (pppLink.up() && wifiInitialized && !ap) {
            wifiRetryBudget = 0;
            wifiRetryAt = 0;
            esp_wifi_disconnect();
            esp_wifi_stop();
            esp_wifi_deinit();
            wifiInitialized = false;
            wifiPausedForPpp = true;
            station = false;
            dirty = true;
        } else if (!pppLink.up() && wifiPausedForPpp) {
            if (!pppLink.active() || pppLink.stop() == ESP_OK) {
                wifiPausedForPpp = false;
                stationConnect(true);
                dirty = true;
            }
        }
#else
        // 状态牌 and 随机转盘 are local, but while what is spoken goes to the cloud it needs the memory BLE holds.
        if((needsInternet(runtime.active())||((speaksOnce(runtime.active())||speaksToo(runtime.active()))&&live.nativeBusy()))&&!live.macWaiting()&&bt&&!board::provisioning()&&bleCompanion.drained()){if(bleStop()==0)restoreBleAfterAudio=false;}
        if(bt&&!settings.bluetoothEnabled)bleStop();
        static int64_t nextBleStart=0;
        // 打开 Mac 程序 brings Bluetooth up on any page until the computer has answered.
        const bool computerBle=(needsComputer(runtime.active())||(runtime.active()==App::Settings&&runtime.model.settingsPage==SettingsPage::Bluetooth)||live.macWaiting())&&!runtime.connected();
        if((computerBle||runtime.active()==App::Slides||runtime.active()==App::Sonar)&&runtime.power(esp_timer_get_time()/1000000)==Power::Awake&&!bt&&live.nativeIdle()&&uiPreparedFor(runtime.active())&&settings.bluetoothEnabled&&esp_timer_get_time()>=nextBleStart){bleStart();nextBleStart=esp_timer_get_time()+5000000;restoreBleAfterAudio=false;}
        wifiPortal.tick(ap,wifiPending);
        if(wifiPending){
            wifi_ap_record_t connected{};
            if(station&&esp_wifi_sta_get_ap_info(&connected)==ESP_OK&&!strcmp(reinterpret_cast<const char*>(connected.ssid),pendingWifi.wifiSsid)){
                SettingsStore store;
                if(store.save(pendingWifi)){
                    settings=pendingWifi;runtime.rememberWifi(pendingWifi.wifiSsid,pendingWifi.wifiPassword);wifiPending=false;if(pendingLocationConsent>=0)live.allowLocation(pendingLocationConsent==1);pendingLocationConsent=-1;esp_netif_ip_info_t info{};esp_netif_get_ip_info(staNetif,&info);char ip[16];snprintf(ip,sizeof(ip),IPSTR,IP2STR(&info.ip));
                    wifiPortal.phase("connected","WiFi 已连接，设置已保存",settings.wifiSsid,ip);strcpy(runtime.model.wifiSetupStatus,"WiFi 已连接 · 设置已保存");closePortalAt=esp_timer_get_time()+30000000;
                }else {wifiPending=false;wifiPortal.phase("failed","连接成功，但保存失败，请重试",pendingWifi.wifiSsid);}
                memset(pendingWifi.wifiPassword,0,sizeof(pendingWifi.wifiPassword));dirty=true;
            }else if(esp_timer_get_time()>wifiJoinDeadline||(wifiReason&&wifiReason!=8&&!wifiRetryBudget&&!wifiRetryAt)){
                wifiPending=false;wifiRetryBudget=0;wifiRetryAt=0;
                const char* reason=wifiReason==201?"未找到该网络，请确认是 2.4 GHz WiFi":wifiReason==2||wifiReason==15||wifiReason==202||wifiReason==204?"密码错误或认证失败，请检查后重试":"连接超时，请检查信号或密码后重试";
                wifiPortal.phase("failed",reason,pendingWifi.wifiSsid);strcpy(runtime.model.wifiSetupStatus,"连接失败 · 请在手机重试");memset(pendingWifi.wifiPassword,0,sizeof(pendingWifi.wifiPassword));dirty=true;
            }
        }
        // 已保存的 WiFi: when the retries for the last network have given up, look for any saved one every minute while
        // the screen is on, and join the strongest. Not while setting up WiFi, recording, or on pages that use the
        // radio themselves (WiFi 工具箱, 移动监控) or need it quick (PPT 翻页, 语音输入 over Bluetooth).
        {
            const int64_t clock=esp_timer_get_time();const auto shown=runtime.active();
            if(joinActive&&station){joinActive=false;runtime.rememberWifi(joinTarget.ssid,joinTarget.password);memset(&joinTarget,0,sizeof(joinTarget));dirty=true;}
            const bool idle=!station&&!wifiConnecting&&!wifiRetryBudget&&!wifiRetryAt;
            if(joinActive&&idle){joinActive=false;memset(&joinTarget,0,sizeof(joinTarget));}
            const bool quiet=runtime.model.voiceBusy()||shown==App::Wifi||shown==App::Motion||shown==App::Slides||shown==App::Dictation||shown==App::Walkie;
            if(idle&&wifiInitialized&&!board::provisioning()&&!wifiPending&&!autoScanning&&*settings.wifiSsid&&!quiet&&clock>=nextAutoScan
               &&runtime.power(clock/1000000)==Power::Awake){
                nextAutoScan=clock+60000000;wifi_scan_config_t config{};autoScanDone=false;
                autoScanning=esp_wifi_scan_start(&config,false)==ESP_OK;if(autoScanning)++wifiScans;
            }
            if(autoScanning&&clock>nextAutoScan-45000000){autoScanning=false;esp_wifi_scan_stop();}  // no answer in 15 s
            if(autoScanning&&autoScanDone.exchange(false)){autoScanning=false;if(!station&&!wifiPending&&!board::provisioning())joinStrongest();else esp_wifi_clear_ap_list();}
        }
        if(closePortalAt&&esp_timer_get_time()>closePortalAt){setAp(false);if(runtime.active()==App::Settings&&runtime.model.settingsPage==SettingsPage::Wifi)runtime.key(Key::Back,esp_timer_get_time()/1000000);dirty=true;}
        runtime.model.wifiConnected=station;runtime.model.wifiSetup=board::provisioning();runtime.model.pppConnected=pppLink.up();
        board::pollBattery();const auto battery=board::battery();
        if(runtime.model.batteryKnown!=battery.known||runtime.model.batteryPercent!=battery.percent){runtime.model.batteryKnown=battery.known;runtime.model.batteryPercent=battery.percent;dirty=true;}
        static int64_t signalAt=0;
        if(esp_timer_get_time()>=signalAt){
            wifi_ap_record_t signal{};runtime.model.wifiSignalKnown=station&&esp_wifi_sta_get_ap_info(&signal)==ESP_OK;
            runtime.model.wifiRssi=runtime.model.wifiSignalKnown?signal.rssi:-127;signalAt=esp_timer_get_time()+1000000;
        }
        static bool autoOffered=false;
        if(!autoOffered&&!bt&&!runtime.connected()&&!station&&!pppLink.up()&&esp_timer_get_time()>25000000&&(runtime.active()==App::Home||runtime.active()==App::Menu)){autoOffered=true;setAp(true);dirty=true;}
#endif
        if (retryAt && esp_timer_get_time() >= retryAt && wifiRetryBudget > 0) {
            wifiRetryAt = 0;
            --wifiRetryBudget;
            ++wifiRetries;
            printf("WIFI_RETRY attempt=%u\n", wifiRetries.load());
            stationConnect(true, true);
        }
        dictationBle.tick();
        runtime.poll();
        runtime.tick(esp_timer_get_time() / 1000000);
#ifdef PASSPORT_DEVICE
        char nextPairCode[7]{};uint32_t nextPairEpoch{};board::pairCode(nextPairCode,&nextPairEpoch);
        board::keyMode(runtime.active()==App::Chat,runtime.generation()*8+static_cast<unsigned>(runtime.model.settingsPage),runtime.power(esp_timer_get_time()/1000000)==Power::Awake||runtime.active()==App::Slides,nextPairEpoch,runtime.active()==App::Home&&!*nextPairCode);
        uiResourcesReady(live.nativeIdle());
        // Uploads arrive on the HTTP task; redraw the card and stay awake while flash is written.
        static uint32_t identityShown=identityRevision();
        if(identityShown!=identityRevision()){identityShown=identityRevision();dirty=true;}
        runtime.flashBusy(identityBusy());
        const bool wasPrepared=uiPreparedFor(runtime.active());
        static bool freshPage=false;
        const auto prepareStarted=esp_timer_get_time();
        if(uiPrepare(runtime.active())&&!wasPrepared){dirty=freshPage=true;pagePrepareUs=esp_timer_get_time()-prepareStarted;}
#endif
#ifdef PASSPORT_DEVICE
        // Before LiveServices: leaving the sound page must stop its task before another page opens the codec.
        dirty|=tools::wifiTick(runtime,staNetif,station);
        dirty|=tools::motionTick(runtime,staNetif,station);
        dirty|=tools::soundTick(runtime,uiPreparedFor(runtime.active())&&runtime.power(esp_timer_get_time()/1000000+2)==Power::Awake);
        dirty|=tools::noiseTick(runtime,live.nativeIdle());
        // On the home page and the menu. The home page's own fetches (weather over TLS) come first: a queued one stops
        // the listening, and the native task starts only once the model's memory is back (tools::wakeHolding).
        dirty|=tools::wakeTick(runtime,runtime.model.wakeWord&&(runtime.active()==App::Home||runtime.active()==App::Menu)&&uiPreparedFor(runtime.active())&&!live.nativeBusy()&&!live.macWaiting());
        // 对讲机: a call heard on any page (后台接收对讲) opens it; LiveServices then ends the page it interrupted.
        dirty|=tools::walkieTick(runtime,staNetif,station,live.nativeIdle()&&uiPreparedFor(runtime.active()));
#endif
        live.transport(pppLink.up()||station,runtime.connected(),encrypted);
        dirty |= live.tick(esp_timer_get_time() / 1000000);
        const auto currentPower = runtime.power(esp_timer_get_time() / 1000000);
        if (currentPower != previousPower) {
            previousPower = currentPower;
            dirty = true;
        }
#ifdef PASSPORT_DEVICE
        static bool sleepWifiStopped=false;
        if(currentPower==Power::DeepSleepIntent){
            board::power(1);
            const bool stopped=live.stopAudio();
            if(stopped&&live.nativeIdle()&&(!bt||bleStop()==0)&&(!pppLink.active()||pppLink.stop()==ESP_OK)){
                // The alarm (or a 待办 reminder) wakes it: 5 minutes early to set the drifting clock when it is far off, then on time.
                const time_t clock=time(nullptr);time_t next=runtime.model.clockValid?nextAlarm(runtime.model.alarms,clock):0;
                const time_t todo=runtime.model.todoNextAt;if(runtime.model.clockValid&&todo>clock&&(!next||todo<next))next=todo;
                board::wakeAfter(next?static_cast<uint32_t>(std::max<time_t>(1,next-clock>600?next-clock-300:next-clock)):0);
                if(wifiInitialized&&!sleepWifiStopped){wifiRetryBudget=0;wifiRetryAt=0;esp_wifi_stop();sleepWifiStopped=true;}
                board::power(2);
            }
        }else{
            if(sleepWifiStopped){sleepWifiStopped=false;if(*settings.wifiSsid)stationConnect(true);}
            board::power(static_cast<unsigned>(currentPower));
        }
#endif
        runtime.model.bleConnected = linked;runtime.model.bleEncrypted=encrypted;
#ifdef PASSPORT_DEVICE
        // The warning light runs at full brightness, the torch at its own level, a brightened QR code at full;
        // leaving hands the saved level back.
        static int pageLight=-1;
        const auto& page=runtime.model;const auto shown=runtime.active();
        const int light=shown==App::Alert||(shown==App::Qr&&page.qrBright)?100:shown==App::Torch?page.torchLevel*20:-1;
        if(light!=pageLight){pageLight=light;board::setBrightness(light<0?settings.brightness:light);}
        sonarTick();
        if(runtime.model.slidesReady!=slidesBle.ready()){runtime.model.slidesReady=!runtime.model.slidesReady;dirty=true;}
#endif
        if (previousLink != linked) {
            previousLink = linked;
            dirty = true;
        }
        if (notify.exchange(false) && linked) {
            char copy[65]; portENTER_CRITICAL(&gattMux); memcpy(copy,gattValue,sizeof(copy)); portEXIT_CRITICAL(&gattMux);
            auto* packet=ble_hs_mbuf_from_flat(copy,strlen(copy));
            if (packet) ble_gatts_notify_custom(connHandle,valueHandle,packet);
        }
        const auto wallMicros=esp_timer_get_time();
        // Frames land on 125 ms boundaries so each warning-light step lasts the same time.
        const bool animate=uiAnimating(runtime)&&wallMicros/125000!=lastEmotion/125000;
        if(animate)lastEmotion=wallMicros;
        if (dirty||animate) {
            const auto started=esp_timer_get_time();const auto pixels=uiFlushPixels(),spi=uiFlushMicros();
            if(dirty)uiRender(runtime);else uiAnimate(runtime);
            renderPixelsLast=uiFlushPixels()-pixels;renderSpiUsLast=uiFlushMicros()-spi; uiCapture("live"); dirty=false; printState();
            const auto spent=static_cast<unsigned>(esp_timer_get_time()-started);
            #ifdef PASSPORT_DEVICE
            if(freshPage){freshPage=false;pageRenderUs=spent;pageSpiUs=renderSpiUsLast.load();}
#endif
            renderLastUs=spent;renderMaxUs=std::max(renderMaxUs.load(),spent);++renderCount;renderTotalUs+=spent;
            printf("RENDER_PERF app=%u microseconds=%u\n",static_cast<unsigned>(runtime.active()),spent);
        }
        mainStack=uxTaskGetStackHighWaterMark(nullptr);
        const uint64_t now=esp_timer_get_time()/1000000;
        if (now!=last) { printState(); last=now; }
    }
}
