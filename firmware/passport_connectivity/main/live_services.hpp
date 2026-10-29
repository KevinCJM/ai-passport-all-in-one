#pragma once
#include "suite.hpp"
#include "audio_pipe.hpp"
#include "esp_http_server.h"
#include "esp_timer.h"

namespace passport {
#ifdef PASSPORT_DEVICE
class NativeServices;
#endif
// Main task owns all business state. HTTP callbacks only enqueue bounded input.
class LiveServices {
    struct Message { char json[2048];bool native{}; };
    struct View{char state[48]{},value[64]{},connection[64]{},lines[4][160]{};bool computer{},bridge{};} view_;
    StaticSemaphore_t viewStorage_{};SemaphoreHandle_t viewMutex_{};
    Runtime& runtime_;
    AudioPipe audio_;
    QueueHandle_t messages_{};
    uint64_t requestedAt_{}, renderedAt_{};
    uint32_t requestId_{}, pending_{}, pendingGeneration_{}, accepted_{}, received_{}, stale_{};
    uint32_t macRequest_{};  // 打开 Mac 程序: the request the computer has yet to answer
    void openOnMac(const char* name,uint64_t now);
    uint64_t voiceAt_{};  // 语音指令: the last time it was busy; home 10 s later
    App command(const char* heard,uint64_t now);
    uint32_t observedGeneration_{};
    unsigned lastSpeechTurn_{};
    App observed_{App::Count};
    std::atomic<unsigned> controlId_{}, controlResult_{};
    std::atomic<unsigned> receivedCount_{}, acceptedId_{}, staleCount_{}, noticeCount_{};
    std::atomic<uint64_t> focusRemaining_{};
    std::atomic<unsigned> focusPhase_{},focusRounds_{},focusDuration_{1500};
    std::atomic<bool> clockValid_{}, focusRunning_{}, focusFinished_{};
    std::atomic<uint64_t> hostContact_{};
    std::atomic<unsigned> phase_{};
    bool networkReady_{},usb_{},bleReady_{},previousNetwork_{};
    char deferred_[12]{};
    bool hostWasMissing_{};
    uint8_t newsAsked_{};  // the older 财经新闻 page requested and not yet answered
    int newsItemAsked_{-1}; // the 财经新闻 headline whose text was last asked for
    uint64_t timerShown_{};  // the second the 定时关闭 countdown last redrew
    int dailyAsked_{-1};   // the 每日一句 day requested and not yet answered
    int wordAsked_{-1};    // the 背单词 card whose IPA and meaning were last requested
    bool quizAsked_{};     // the next 知识问答 question requested and not yet answered
    int podcastAsked_{-1}; // the 播客 program the device was asked to play, -1 = its list
    char stockAsked_[16]{}; // what 自选股 last asked for: "list" or "chart:<row>:<days>"
    uint8_t warnAsked_{};   // the 预警 detail (1-based) last asked for, 0 none
    int typhoonAsked_{-1};  // the 台风 row whose track was last asked for
    int hotAsked_{-1};      // the 热榜 item last asked for in full
    uint32_t voiceJob_{};   // a phrase being spoken on 汇率, 自选股 or 所在城市 (its request): timed refreshes wait
#ifdef PASSPORT_DEVICE
    NativeServices* native_{};bool deferredAudio_{};
    QueueHandle_t nativeMessages_{};
#endif
    uint64_t loadingAt_{};
    void request(const char* operation);
    void renderLocal(uint64_t now);
    void persistFocus();
    int control(void* json);
public:
    explicit LiveServices(Runtime& runtime) : runtime_(runtime) {}
    void init(httpd_handle_t server);
    bool tick(uint64_t now);
    void action(Key key);
    void refresh() { request("enter"); }
    // A long Up starts speaking: 随机转盘's mode or options, a 倒数日 event, a currency (汇率), a security (自选股);
    // Confirm then ends it, as on 状态牌.
    void speak();
    // 自选股: a long Down asks to delete the picked security, a second one has the device delete it.
    void dropStock();
    void say();
    // 离座锁屏: a long Up takes the current signal as the at-desk level, a long Down locks the computer now.
    void guardCommand(bool calibrate) { if(runtime_.active()==App::Guard) request(calibrate?"cal":"lock"); }
    // Long press on Up/Down tunes the radio or steps a podcast's episodes; short presses change the volume.
    void station(bool next) { if(runtime_.active()==App::Radio||(runtime_.active()==App::Podcast&&runtime_.model.podcastPlayer)) request(next?"next":"prev"); }
    void hostSeen() { hostContact_=esp_timer_get_time()/1000000; }
    void transport(bool network,bool usb,bool ble) { networkReady_=network;usb_=usb;bleReady_=ble; }
    esp_err_t receive(httpd_req_t* request);
    esp_err_t snapshot(httpd_req_t* request);
    bool snapshotJson(char*,size_t);
    bool enqueueComputer(const char*);
    void reconnectComputer(){observed_=App::Count;pending_=0;hostContact_=0;}
    uint32_t received() const { return received_; }
    uint32_t stale() const { return stale_; }
    bool stopAudio();
    bool nativeIdle()const;
    bool nativeBusy()const;  // a native task running or queued
    // 打开 Mac 程序 waits for the computer: Bluetooth is wanted (and the wake word set aside) until it answers.
    bool macWaiting()const{return macRequest_;}
    bool enqueueNative(const char*);
    bool allowLocation(bool allowed);
    const AudioPipe& audio() const { return audio_; }
};
}
