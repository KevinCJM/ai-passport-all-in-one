#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "settings.hpp"
#include "apps.hpp"
#include "weather.hpp"
#include "typhoon.hpp"
#include "alarm.hpp"
#include "calc.hpp"
#include "word_study.hpp"
#include "lunar.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"

namespace passport {
enum class Result : uint8_t { Ok, StopTimeout, LowMemory, Invalid, StartFailed };
enum class Key : uint8_t { Up, Down, Confirm, Back, OpenSettings };
enum class Power : uint8_t { Awake, ScreenOff, DeepSleepIntent };
enum class LivePhase : uint8_t { Loading, Ready, Error };
enum class HomeScreen : uint8_t { Weather, Card, Photo, Count };
enum class SlideKey : uint8_t { Previous, Next, Start, Black, End };
struct Profile { const char* id; const char* title; uint32_t workspace; bool audio; bool ble; const char* label{}; };
const Profile& profile(App app);
// Name under the menu icon: at most four characters.
inline const char* menuLabel(App app) { return profile(app).label ? profile(app).label : profile(app).title; }
// One 倒数日 entry: days until (or, once past, since) the date; minutes >= 0 only for 下班.
struct CountdownItem { const char* name; Date date; int days; int minutes; };
constexpr size_t kReserve = 48 * 1024;
struct Event { App app; uint32_t generation; uint32_t value; };

// All records are bounded. No account secrets or live credentials in fixtures.
struct Model {
    std::array<std::array<char, 160>, 8> notices{};
    uint8_t noticeCount{}, noticeHead{}, focusPhase{}, volume{55};
    uint32_t focusRounds{};
    std::array<uint32_t,3> focusPeriods{1500,300,900};
    uint32_t notificationsReceived{}, notificationsDropped{};
    uint64_t focusDeadline{}, focusRemaining{25 * 60}, focusDuration{25 * 60};
    bool focusRunning{}, focusFinished{}, clockValid{true}, quotaKnown{false};
    uint32_t framesAccepted{};
    bool bridgeAlive{true};
    bool bleConnected{},bleEncrypted{};
    bool live{};
    char liveState[48]{}, liveValue[64]{}, liveLines[4][160]{};
    // Chat page text record: user utterances and replies, oldest first; reply sentences join one entry.
    struct ChatEntry { bool user{}; char text[160]{}; };
    std::array<ChatEntry,6> chat{}; uint8_t chatCount{}; unsigned chatTurn{}; char chatSentence[160]{}; uint32_t chatRevision{};
    void chatRecord(unsigned turn, const char* heard, const char* sentence);
    LivePhase livePhase{LivePhase::Loading};
    char connectionState[64]{"正在连接"};
    char pairingCode[7]{};
    bool wifiConnected{},wifiSetup{};
    bool pppConnected{},wifiSignalKnown{},batteryKnown{};
    int wifiRssi{-127};uint8_t batteryPercent{};
    WeatherReading weatherReading;
    SettingsPage settingsPage{SettingsPage::List};
    HomeScreen homeScreen{HomeScreen::Weather};
    bool wakeWord{};  // 语音唤醒: listen for 你好小智 on the home page and the menu
    // 说功能名跳转: a 语音指令 phrase that is a page's name opens that page (on unless switched off). wakeRow: the
    // 语音唤醒 page's row picked (0 the wake word, 1 this, 2 打开 Mac 程序).
    bool voiceJump{true};uint8_t wakeRow{};
    // 打开 Mac 程序: "打开 Mac 的微信" in 语音指令 has the computer open it (on unless switched off). macNote, shown in
    // the footer from macAt (seconds): the request on its way, then what the computer did.
    bool macOpen{true};char macNote[64]{};uint64_t macAt{};
    // 连电脑时常亮 (on unless switched off; off, the idle time darkens the screen on USB too, but never sleeps it).
    // idleRow: the 自动息屏 page's row picked (0 the time, 1 this), 2 while the time is being changed.
    bool usbAwake{true};uint8_t idleRow{};
    uint8_t settingsSelection{};uint32_t settingsValue{};
    // Settings > 所在城市: the city set (on the phone, by voice or by IP) as "杭州 · 浙江", and the row picked (0 speak, 1 IP).
    char cityName[48]{};uint8_t cityRow{};
    char wifiSetupStatus[80]{"手机连接热点后选择 WiFi"};
    // Settings > WiFi: the saved networks' names (last joined first). wifiRow 0 is 添加 WiFi, n the network n-1;
    // wifiArmed: a long Down asked to forget it.
    char wifiSaved[WifiList::kMax][33]{};uint8_t wifiSavedCount{},wifiRow{};bool wifiArmed{};
    struct Quota { bool known{};uint8_t used{};uint32_t minutes{};int64_t resets{}; } quotas[4];  // Codex short/long window, then Claude Code 5 hours/7 days.
    bool metricsKnown{};
    uint64_t memoryUsed{},memoryTotal{},netUp{},netDown{},diskFree{},uptime{};
    int disk{},battery{-1};bool charging{};float load{};uint8_t cores{};  // battery -1: a Mac without one
    std::array<uint8_t,24> cpuHistory{};
    uint8_t historyCount{};
    int cpu{37}, ram{62}, quotaUsed{};
    uint8_t mediaIndex{}, mediaCount{};  // position in the station list
    bool mediaPlaying{};  // the radio or a podcast is playing: the screen goes off, but no deep sleep
    // 播客: the program list (page.podcasts, sent by the device), or once one is opened its player.
    // podcastPos and podcastDur are the episode's place and length in seconds.
    uint8_t podcastCount{},podcastIndex{};bool podcastPlayer{};uint32_t podcastPos{},podcastDur{};
    WordDeck words; WordSession wordSession;
    // PPT page: the talk timer runs while the page is open; the device reports whether the key reached the computer.
    bool slidesPressed{},slidesFailed{},slidesReady{};uint64_t slidesStarted{};SlideKey slidesLast{};
    // Warning light: the pattern and when it was picked (its name shows for two seconds).
    uint8_t alertMode{};uint64_t alertChanged{};
    // Status sign: labels typed on the computer come first, then the presets.
    StatusBoard status;
    unsigned statusCount() const;
    StatusLabel statusLabel(unsigned index) const;
    // Torch: brightness in 20% steps (1-5) and colour; the mode shows for two seconds after a key.
    uint8_t torchLevel{5},torchColor{};uint64_t torchChanged{};
    // QR page: stored entries present (a bit per slot), then the WiFi entry; the shown one is loaded into qrTitle/qrText.
    uint8_t qrIndex{},qrMask{};bool qrBright{};char qrTitle[48]{},qrText[301]{};
    // 万年历: the selected day (its month is on show); calendarView 0 shows the month, 1 that day's 黄历, 2 its 月相 and 节气.
    int calendarYear{2026},calendarMonth{1},calendarDay{1};uint8_t calendarView{};
    // 倒数日: built-in festivals plus the computer's (or spoken) events, nearest first; past one-off events last.
    // countdownArmed: a long Down asked to delete the event on show.
    CountdownBoard countdown;uint8_t countdownIndex{};bool countdownList{},countdownArmed{};
    unsigned countdownItems(Date today,int minutes,CountdownItem* out,unsigned capacity) const;
    struct NewsItem{char time[6];char title[96];};
    struct EventItem{int16_t year;uint8_t type;char title[64];};
    struct QuakeItem{uint32_t at;char place[48];uint8_t mag;uint16_t depth;};
    struct StoryItem{char title[96];char site[32];uint16_t points,comments;uint32_t at;};
    struct WifiAp{char ssid[33];int8_t rssi;uint8_t channel;bool open;};
    struct PingRow{char name[12];char host[24];uint16_t average,low,high;uint8_t sent,lost;};  // milliseconds
    struct LanDevice{uint8_t ip[4];uint8_t mac[6];};
    struct PodcastRow{char name[40];char note[48];};  // program, and where it was left
    // 自选股: one quote; tag is the market (SH, SZ, BJ, HK, US); values are NAN when not traded.
    struct StockRow{char name[28];char code[10];char tag[4];uint8_t decimals;float price,change,pct,open,high,low,prev,amount;};
    // 百度热搜: a title cut to the list's width, its heat and Baidu's tag (kHotPinned: pinned on top).
    struct HotRow{char title[40];uint32_t score;uint8_t tag;};
    // 语音笔记: a note's time and the start of its text, for the list.
    struct MemoRow{uint32_t at;uint16_t chars;char head[62];};
    static constexpr unsigned kMemoRows=4,kMemoLines=10;  // notes in the list, lines of an open note
    // What only its own page reads shares one block; each of these pages clears its count (or text) on entry,
    // so another page's leftovers are never read.
    union PageData{
        // 财经新闻: the six headlines, and item textFor's text in full (255: none yet).
        struct{std::array<NewsItem,6> items;char text[720];uint8_t textFor;} news;
        std::array<EventItem,16> events;
        struct{char en[224],cn[192],date[12];} daily;
        std::array<QuakeItem,16> quakes;
        std::array<StoryItem,10> stories;
        struct{char question[256],answers[4][72],category[40],level[8];} quiz;
        struct{std::array<WifiAp,16> aps;std::array<PingRow,3> pings;std::array<LanDevice,32> lan;} wifi;
        std::array<PodcastRow,12> podcasts;
        WeatherForecast forecast;
        // chart: 0-999 between chartLow and chartHigh; minute prices, or four (open, close, high, low) per day.
        struct{std::array<StockRow,12> rows;std::array<int16_t,200> chart;} stocks;
        TyphoonView typhoon;
        // title and desc: item detailFor in full (255: none yet).
        struct{std::array<HotRow,kHotRows> rows;char title[72],desc[240];uint8_t detailFor;} hot;
        // rows: the notes from memoTop on show (a line of each); note: the one memoDetail opens.
        struct{std::array<MemoRow,kMemoRows> rows;MemoNote note;} memo;
        WheelBoard wheel;
        TodoList todo;
        LedgerBook ledger;
        // 语音计算器: the sums worked out on this visit, newest first (calcCount of them).
        std::array<calc::Row,8> calc;
        // 中英翻译: what was said and its translation (toEnglish: Chinese was said), by engine 1 腾讯翻译君 / 2 MyMemory
        // (0: it failed); at is when it arrived (monotonic seconds).
        struct{char source[512],result[768];bool toEnglish;uint8_t engine;uint64_t at;} translation;
    } page{};
    // 财经新闻: page.news from the native fetch (liveLines[0] holds the index quotes).
    uint8_t newsCount{},newsIndex{};
    // Pages of six, newest first: moving past the last headline asks for the next (older) page.
    // newsScroll: the first line of the text on show; Up/Down scroll it, then step to the next headline.
    uint8_t newsPage{1},newsWant{1},newsScroll{};bool newsFromEnd{};
    static constexpr unsigned kNewsLines=5,kNewsMoreLines=8;  // lines under the headline, and once scrolled past it
    // 白噪音: noiseRow is the selected row (the sounds, then the sleep timer); noiseKind plays while noisePlaying.
    // noiseTimer is the sleep timer in minutes (0 = none) and noiseUntil when it ends (monotonic seconds).
    uint8_t noiseRow{},noiseKind{},noiseTimer{};bool noisePlaying{};uint64_t noiseUntil{};
    // 闹钟: the saved alarms. alarmFired is the ring last started (epoch), so each one rings once; alarmSnooze brings
    // a snoozed one back (epoch, 0 = none). It rings from alarmRingAt (monotonic seconds) for at most 3 minutes.
    // The list: alarmIndex 0 is the 录音 row, n the alarm n-1; alarmEdit opens that alarm's clock; alarmArmed: a long
    // Down asked to delete it.
    AlarmList alarms;uint32_t alarmFired{},alarmSnooze{};bool alarmRinging{};uint64_t alarmRingAt{};
    uint8_t alarmIndex{};bool alarmEdit{},alarmArmed{};
    // 自选股: page.stocks; Confirm opens stockIndex in detail, where Confirm switches minutes / 30 days. The chart
    // in page.stocks is for row chartFor (255 = none) and chartDays: chartCount points spread over chartTotal
    // (minutes of the session, or days); the axis reads chartFrom, chartMid (a lunch break or time zone) and chartTo.
    // stockArmed: a long Down asked to delete the picked one (LiveServices has the device do it).
    uint8_t stockCount{},stockIndex{},chartFor{255},chartCount{};bool stockDetail{},stockDays{},chartDays{},stockArmed{};uint16_t chartTotal{};
    float chartLow{},chartHigh{};char chartFrom[8]{},chartMid[16]{},chartTo[12]{};uint32_t stockUpdated{};
    // 天气预报: page.forecast from the native fetch; Up/Down step through the seven days, the next 24 hours and air.
    uint8_t forecastView{},forecastWarn{};  // forecastWarn: the 预警 tab shows warning n-1's text, 0 the list
    // 汇率: units of each currency per one unit of the base, as fetched; rateUpdated is the source's epoch.
    struct RateItem{char code[4];double value;};
    // rateArmed: a long Down asked to delete the picked currency.
    std::array<RateItem,RateBoard::kTargets> rates{};uint8_t rateCount{},rateIndex{};char rateBase[4]{};uint32_t rateUpdated{};bool rateArmed{};
    // 历史今日: page.events, today's events from the native fetch; type 0 = event, 1 = birth, 2 = death.
    uint8_t eventCount{},eventIndex{};
    // 每日一句 (金山词霸): page.daily, the sentence dailyBack days ago; dailyWant is the day asked for.
    uint16_t dailyBack{},dailyWant{};
    // 地震速报 (中国地震台网 via wolfx): page.quakes, newest first; at is the epoch, magnitude in tenths, depth in km.
    uint8_t quakeCount{},quakeIndex{};
    // 知识问答 (Open Trivia DB): page.quiz, one question at a time, its answers already shuffled; quizSerial tells a new
    // one apart. Up/Down pick an answer, Confirm reveals it, then Confirm again sets quizWant for the next question.
    uint8_t quizCount{},quizCorrect{},quizPick{};bool quizShown{},quizWant{};uint16_t quizRight{},quizAsked{};uint32_t quizSerial{};
    // Hacker News front page (Algolia): page.stories with title, site, points, comments and posting time.
    uint8_t storyCount{},storyIndex{};
    // 台风路径: page.typhoon, this season's typhoons; Up/Down pick typhoonIndex, whose track LiveServices fetches.
    uint8_t typhoonIndex{};
    // 百度热搜: page.hot; Up/Down move hotIndex, Confirm opens or closes its detail. hotUpdated is the fetch's epoch.
    uint8_t hotCount{},hotIndex{};bool hotDetail{};uint32_t hotUpdated{};
    // 语音笔记: memoCount notes in MemoStore, newest first. memoIndex 0 is the 录音 row, n the note n-1; Confirm
    // records (LiveServices) or opens the note, scrolled memoScroll lines. memoArmed: a long Down asked to delete it.
    // memoSavedAt: when the last note was saved.
    uint8_t memoCount{},memoIndex{},memoTop{};bool memoDetail{},memoArmed{},memoMounted{};uint16_t memoScroll{};
    uint64_t memoSavedAt{};
    // 语音笔记 and 语音输入 report liveValue "rec" while listening and "asr" while recognising; listenAt is when
    // listening started (monotonic seconds). 语音输入's computer reports the outcome as "done", "empty" or "fail"
    // (dictationAt: when), with the text typed in liveLines[0], "N 字 · HH:MM" in [1] and the link in [2].
    uint64_t listenAt{},dictationAt{};
    // The last Confirm stopped a ring or a snooze, so it does not also start listening (LiveServices reads it).
    bool confirmTaken{};
    bool listening() const { return !strcmp(liveValue,"rec"); }
    bool voiceBusy() const { return listening()||!strcmp(liveValue,"asr"); }
    // 随机转盘: page.wheel. A spin started at wheelSpunAt (microseconds, 0 = never) runs kWheelSpin while the highlight
    // steps wheelSteps times on from option wheelFrom, slowing down, and stops on wheelPick (255 = none yet).
    // wheelArmed: a long Down asked to delete the mode on show.
    static constexpr uint64_t kWheelSpin=3200000;
    uint64_t wheelSpunAt{};uint16_t wheelSteps{};uint8_t wheelFrom{},wheelPick{255};bool wheelArmed{};
    bool wheelSpinning() const { return wheelSpunAt&&uint64_t(esp_timer_get_time())-wheelSpunAt<kWheelSpin; }
    unsigned wheelLit() const;  // the option highlighted now
    // 待办提醒: page.todo while the page is open. todoNextAt/todoNext: the next reminder (epoch, 0 = none) and its text,
    // kept so it rings from any page; it rings on the page from todoRingAt (monotonic) for at most 3 minutes.
    // todoIndex 0 is the 录音 row, n the todo n-1; kTodoRows show from todoTop. todoArmed: a long Down asked to delete.
    static constexpr unsigned kTodoRows=4;
    uint32_t todoNextAt{};char todoNext[73]{};bool todoRinging{};uint64_t todoRingAt{};
    uint8_t todoIndex{},todoTop{};bool todoArmed{};
    // 语音记账: page.ledger while the page is open. ledgerIndex 0 is the 录音 row, n today's entry n-1 (newest first);
    // kLedgerRows show from ledgerTop. ledgerArmed: a long Down asked to delete.
    static constexpr unsigned kLedgerRows=3;
    uint8_t ledgerIndex{},ledgerTop{};bool ledgerArmed{};
    // 语音计算器: page.calc holds calcCount sums; calcIndex is the one on show (0 the newest), which a sum that
    // starts with an operator goes on from.
    uint8_t calcCount{},calcIndex{};
    // 离座锁屏: the settings and the selected row (0 离开距离, 1 离开确认, 2 防锁屏, 3 回来亮屏); the computer reports
    // guardPhase (0 waiting, 1 USB, 2 at the desk, 3 about to lock, 4 locked), its reading of the badge's signal and
    // the line under which you count as away (dBm, 0 = none), the calibrated level, and the seconds left.
    GuardPrefs guard;uint8_t guardRow{},guardPhase{},guardLeft{};int8_t guardRssi{},guardLine{},guardBase{};
    // 中英翻译: the first line of the translation on show (Up/Down scroll it); kTranslateLines fit on the page.
    static constexpr unsigned kTranslateLines=5;
    uint8_t translateScroll{};
    // 声音 and WiFi 工具箱 settings, loaded when either page opens.
    ToolPrefs tools;
    // WiFi 工具箱: tab 0 信道 / 1 延迟 / 2 设备 / 3 唤醒; the device's tools task fills page.wifi.
    // Confirm bumps wifiAction (rescan, retest, send), except on 设备 where it makes the device the wake target.
    uint8_t wifiApCount{},pingCount{},lanCount{},lanSelf{255},lanGateway{255};uint16_t dnsMs{};
    uint8_t wifiTab{},wifiIndex{},wifiAction{};bool wifiBusy{};char wifiNote[64]{};uint32_t wakeSentAt{};  // epoch of the last wake packet
    // 移动监控: WiFi CSI from the router, read by the device's tools task twice a second. motionPhase 0 WiFi 未连接 /
    // 1 校准中 / 2 安静 / 3 有人移动 / 4 收不到路由器的数据. motionLevel 0-100 is the disturbance against the quiet
    // baseline on a log scale (10 = baseline, +20 per doubling); motionHistory keeps the last 30 s, oldest first.
    // Confirm bumps motionCalibrate, which asks for a new baseline.
    uint8_t motionPhase{},motionLevel{},motionCalLeft{},motionRate{},motionCalibrate{},motionHistoryCount{};int8_t motionRssi{};
    std::array<uint8_t,60> motionHistory{};uint16_t motionEvents{};uint32_t motionLastAt{};  // epoch of the last movement
    // 声音: view 0 频谱与分贝 / 1 调音器 / 2 节拍器; the device's sound task fills the readings.
    uint8_t soundView{};std::array<uint8_t,24> soundBars{};  // 0-100 per band, low to high
    int16_t soundDb{},soundMax{};uint16_t soundHz{};          // dB and Hz in tenths; soundHz 0 = no steady pitch
    bool soundRunning{};uint8_t soundBeat{};                  // the metronome, and the beat it last played (0-3)
    bool soundFailed{};                                       // the microphone or speaker would not open
    // 蓝牙声纳: nearby advertisers copied from the scan once a second; the focus follows a device by address.
    struct SonarDevice{uint8_t addr[6];int8_t rssi;uint16_t company;uint32_t seen;char name[20];};
    static constexpr unsigned kSonarSlots=24;
    std::array<SonarDevice,kSonarSlots> sonar{};uint8_t sonarCount{},sonarFocus[6]{};bool sonarNamed{true},sonarList{};
    int8_t sonarPeerRssi{};  // the connected phone, 0 = none
    // 秒表: time banked before the current run, and when the run started (microseconds). The last seven lap
    // totals, newest first: six are shown, the seventh gives the sixth its split.
    bool swRunning{};uint64_t swBase{},swStarted{};std::array<uint64_t,7> swLaps{};unsigned swLapCount{};
    uint64_t stopwatchUs() const { return swBase+(swRunning?esp_timer_get_time()-swStarted:0); }
    // Visible devices (named or unnamed per sonarNamed), strongest first; returns the count.
    unsigned sonarVisible(uint8_t* order) const;
    char weather[80]{"晴 26°C   湿度 65%"};
    char lastNotice[160]{};
    void notify(const char* text);
    void tick(App active, uint64_t now);
    void action(App app, uint64_t now);
    bool selectFocus(int delta);
};
// Every list above has a fixed count, so the state that stays in RAM is known: about 9 KB (0.4.8). A feature that
// needs more raises this budget on purpose, not by accident.
static_assert(sizeof(Model) <= 10 * 1024);

class Worker {
    enum class Op : uint8_t { Bind, Stop, Halt };
    struct Command { Op op; App app; uint32_t generation, ticket; uint8_t* data; size_t size; };
    struct Ack { uint32_t ticket; };
    QueueHandle_t commands_{}, acks_{}, events_{};
    TaskHandle_t task_{};
    uint32_t ticket_{};
    std::atomic<bool> stallOnce_{false};
    static void run(void* arg);
    bool request(Op op, App app, uint32_t generation, uint8_t* data, size_t size);
public:
    bool init(QueueHandle_t events);
    bool bind(App app, uint32_t generation, void* data, size_t size);
    bool stop();
    void failNextStop() { stallOnce_ = true; }
    bool shutdown();
};

class Runtime {
    Worker worker_;
    QueueHandle_t events_{};
    void* workspace_{};
    App active_{App::Home};
    uint32_t generation_{1};
    size_t workspaceBytes_{};
    uint64_t lastInput_{}, unpluggedAt_{}, alarmChecked_{};
    uint64_t now_{};
    bool connected_{true}, suspended_{}, flashBusy_{};
    bool screenOff_{};  // long Confirm on the home page: screen off until the next key, on USB too
    bool quarantined_{};
    uint8_t selection_{};
    SystemSettings* settings_{};
public:
    Model model;
    uint32_t switches{}, rejectedEvents{}, queueDrops{};
    Result lastResult{Result::Ok};
    bool init(SystemSettings& settings);
    bool setIdleMinutes(uint32_t minutes);
    bool settingsKey(Key key);
    // 已保存的 WiFi: rereads the names; rememberWifi puts a joined network first and makes it the one tried at boot;
    // forgetWifi (a long Down on its row, twice) deletes the selected one.
    void reloadWifi();
    void rememberWifi(const char* ssid, const char* password);
    void forgetWifi();
    // Long Up / long Down on the word page: known / not known.
    void gradeWord(bool known, uint64_t now);
    // A key on the PPT page (long Up / long Down: black screen / end show).
    void slideKey(SlideKey key, uint64_t now);
    // Labels changed on the computer: reload them, keeping the shown label in range.
    void reloadStatus();
    // 状态牌: what the cloud heard becomes the first label and is shown (false: nothing usable, lastNotice says why).
    bool addStatus(const char* heard);
    void reloadCountdown();
    // What the cloud heard adds to 汇率 (currencies), 闹钟 (an alarm) or 倒数日 (an event); false: nothing changed,
    // lastNotice says why. 自选股 and the city are looked up on the device (NativeServices).
    bool addRate(const char* heard);
    bool addAlarm(const char* heard);
    bool addCountdown(const char* heard);
    // 待办提醒: what the cloud heard becomes a todo, its time (if said) its reminder (false: nothing added, lastNotice says
    // why); reloadTodo rereads the next reminder; ringing ends with the todo done, snoozed 10 minutes, or just stopped.
    bool addTodo(const char* heard);
    void reloadTodo();
    enum class TodoRing : uint8_t { Done, Snooze, Stop };
    void endTodoRing(TodoRing how);
    // 语音记账: what the cloud heard becomes an entry (false: nothing added, lastNotice says why). ledgerSynced:
    // the computer took or acknowledged entries since the last call (the open page is reloaded).
    bool addLedger(const char* heard);
    // 语音计算器: works out what the cloud heard, going on from the picked result (false: lastNotice says why).
    bool addCalc(const char* heard);
    bool ledgerSynced();
    // 随机转盘: Confirm spins the mode on show; what the cloud heard becomes a mode ("吃饭，烧烤，汉堡") or options
    // added to the mode on show (one or two items). false: nothing changed, lastNotice says why.
    void spinWheel();
    bool addWheel(const char* heard);
    // 语音笔记: rereads the count and the rows on show; addMemo saves what the cloud heard (false: the store is full).
    void reloadMemo();
    bool addMemo(const char* text, uint64_t now);
    // Rescans the stored QR slots and loads the shown entry (WiFi last, built from the settings).
    void reloadQr();
    unsigned qrCount() const;
    // Long Up / long Down: menu page, calendar month, sonar named filter, WiFi tab, sound view, motion alarm (up: 关 → 一次 → 连续, down: back).
    bool longKey(bool up, uint64_t now);
    const SystemSettings& settings() const { return *settings_; }
    Result switchTo(App target, bool failAlloc = false, bool failStart = false);
    void wake(uint64_t now) { tick(now);lastInput_=now;screenOff_=false; }
    // Woken early to set the clock before an alarm: stay dark until a key or the alarm.
    void darken() { screenOff_=true; }
    void key(Key key, uint64_t now);
    void poll();
    void tick(uint64_t now);
    bool codexNotice(const char* text);
    bool post(Event event);
    void connection(bool configuredSession, uint64_t now);
    void suspended(bool value) { suspended_ = value; }
    void flashBusy(bool value) { flashBusy_ = value; }
    Power power(uint64_t now) const;
    Power automaticPower(uint64_t now) const;  // power() without the manual screen-off
    App active() const { return active_; }
    uint32_t generation() const { return generation_; }
    uint8_t selection() const { return selection_; }
    size_t workspaceBytes() const { return workspaceBytes_; }
    bool connected() const { return connected_; }
    void failNextStop() { worker_.failNextStop(); }
    bool shutdown();
};

// 语音记账's hand-over to the computer, from any task: acks are the entries (id, revision) it has saved; month its
// totals for the month yyyymm (0: none sent). Writes the entries it has not got yet as JSON (LedgerBook::pendingJson).
struct LedgerAck { uint32_t id; uint8_t rev; };
void ledgerExchange(const LedgerAck* acks, unsigned count, int32_t monthOut, int32_t monthIn, uint32_t month, char* out, size_t size);
// Entries waiting for the computer, and its month totals (known: sent for this month).
struct LedgerStatus { unsigned pending; bool monthKnown; int32_t monthOut, monthIn; };
LedgerStatus ledgerStatus();
time_t ledgerToday();  // local midnight

void uiInit();
unsigned uiMemoryFree();
unsigned uiFlushPixels();
unsigned uiFlushMicros();
unsigned uiMemoryPeak();
void uiResourcesReady(bool);
bool uiPreparedFor(App);
bool uiPrepare(App);
unsigned uiTextLines(const char* text,int width=224);  // lines the page's paragraph needs for text, at 16 px across width px
unsigned uiWrappedLines(const char* text);  // the same, English wrapped between words (中英翻译)
void uiRender(const Runtime& runtime);
bool uiAnimating(const Runtime& runtime);
void uiCapture(const char* name);
void uiCheck();
void uiShutdown();
} // namespace passport
