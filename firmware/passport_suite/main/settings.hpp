#pragma once
#include "ledger.hpp"
#include <cstddef>
#include <cstdint>

namespace passport {
struct WordDeck;
// One system record shared by every app. Values here are simulator fixtures.
struct SystemSettings {
    uint32_t version{1};
    uint32_t idleSeconds{360};
    char wifiSsid[33]{"myssid"};
    char wifiPassword[65]{"mypassword"};
    char timezone[40]{"CST-8"};
    uint8_t brightness{80};
    uint8_t bluetoothEnabled{1};
};
// Status sign: the label on show, whether the page keeps the screen lit, and labels typed on the computer or
// spoken on the page (newest first; a new one pushes the oldest out).
struct StatusBoard {
    static constexpr uint8_t kVersion=1, kCustom=5;
    uint8_t version{kVersion}, index{}, awake{1};
    char custom[kCustom][40]{};
};
static constexpr size_t kStatusV1Size = 3 + 4 * 40;  // saved with four labels: loads with the fifth empty
// 倒数日: events typed on the computer (date yyyymmdd; flags 1 = every year, 2 = lunar month/day) and the 下班 time.
struct CountdownBoard {
    static constexpr uint8_t kVersion=1, kEvents=6, kYearly=1, kLunar=2;
    struct Event { char name[25]; uint8_t flags; uint32_t date; };
    uint8_t version{kVersion}; uint16_t offwork{18*60};  // minutes after midnight, 0 = no 下班 countdown
    Event events[kEvents]{};
};
// 二维码: one entry typed on the computer; an empty text means unused.
struct QrEntry { static constexpr uint8_t kCount=4; char title[40]; char text[301]; };
// 汇率: the base currency and up to six to show, set on the phone setup page.
struct RateBoard {
    static constexpr uint8_t kVersion=1, kTargets=6;
    uint8_t version{kVersion}; char base[4]{"CNY"};
    char targets[kTargets][4]{"USD","EUR","JPY","HKD","GBP","KRW"};
};
// 声音, WiFi 工具箱 and 移动监控: the dB calibration, the tuner's A4, the metronome tempo, the Wake-on-LAN target
// and the motion sensitivity (1-9; it sits in what was padding, so older saves read back as 0 and mean 5) and alarm.
// The blob grew from 14 bytes: a shorter save loads with the newer fields at their defaults.
struct ToolPrefs {
    static constexpr uint8_t kVersion=1;
    uint8_t version{kVersion}; int8_t soundCal{}; uint16_t a4{440}, bpm{100};
    uint8_t wakeMac[6]{}; bool wakeSet{}; uint8_t motionSense{5}; uint8_t motionAlarm{1};
    // 移动监控 alarm: 0 off, 1 once per movement, 2 every 3 s while it lasts. Saved as a bool before, so on = once.
    uint8_t alarm() const { return motionAlarm <= 2 ? motionAlarm : 1; }
    uint8_t sense() const { return motionSense >= 1 && motionSense <= 9 ? motionSense : 5; }
    // The activity level (0-100) above which 移动监控 calls it movement: 82 at sensitivity 1, 18 at 9.
    uint8_t motionLimit() const { return static_cast<uint8_t>(90 - 8 * sense()); }
};
static constexpr size_t kToolPrefsV1Size = 14;  // before motionAlarm
static_assert(sizeof(ToolPrefs) == 16);
// 已保存的 WiFi: networks joined before, the last joined first; a sixth pushes the oldest out. Read from NVS only when
// needed (passwords never stay in RAM); SystemSettings' wifiSsid is the last joined, tried first at boot.
struct WifiNetwork { char ssid[33]; char password[65]; };
struct WifiList {
    static constexpr uint8_t kVersion=1, kMax=5;
    uint8_t version{kVersion}, count{};
    WifiNetwork items[kMax]{};
    int find(const char* ssid) const;
    // Puts the network first (new, or moved with its current password); false when it already was, unchanged.
    bool remember(const char* ssid, const char* password);
    void remove(unsigned index);
};
// 待办提醒: up to kMax todos of up to kChars characters, kept in order: the soonest reminder first, then those without
// one, then the done ones. at: the reminder (epoch, 0 = none); flags: kDone, kRung (its reminder has gone off).
struct TodoItem { uint32_t at; uint8_t flags; char text[73]; };
struct TodoList {
    static constexpr uint8_t kVersion=1, kMax=16, kChars=24, kDone=1, kRung=2;
    uint8_t version{kVersion}, count{};
    TodoItem items[kMax]{};
    void sort();
    bool valid() const;
    // The next reminder still to go off (nullptr: none).
    const TodoItem* next() const;
};
// 离座锁屏: how far counts as away (0 近 1 中 2 远), the delay before locking (an index into kDelays), keeping the
// computer awake (0 off, 1 the display, 2 also "online": a mouse nudge when idle) and lighting its screen on return.
struct GuardPrefs {
    static constexpr uint8_t kVersion=1, kDelays[]{5,10,20,30};
    uint8_t version{kVersion}, distance{1}, delay{1}, keep{2}, wake{1};
    bool valid() const { return version==kVersion&&distance<3&&delay<4&&keep<3&&wake<2; }
};
// 闹钟: one alarm. mode 0 off, 1 每天, 2 工作日 (Monday to Friday, following official days off and make-up
// working days), 3 仅一次 (turns itself off once it has rung).
struct AlarmPrefs {
    static constexpr uint8_t kVersion=1;
    uint8_t version{kVersion}, hour{7}, minute{30}, mode{};
};
// Up to five of them, in the order added (the single alarm saved before 0.5.4 becomes the first).
struct AlarmList {
    static constexpr uint8_t kVersion=2, kMax=5;
    uint8_t version{kVersion}, count{};
    AlarmPrefs items[kMax]{};
    bool valid() const;
};
// 随机转盘: modes (吃饭, 喝什么 ...) of two to eight options, set on the phone setup page or spoken on the page.
// current is the mode on show. Loaded into the page's own memory while the page is open.
struct WheelBoard {
    static constexpr uint8_t kVersion=1, kModes=5, kOptions=8, kChars=8, kText=25;
    struct Mode { char name[kText]; uint8_t count; char options[kOptions][kText]; };
    uint8_t version{kVersion}, count{}, current{};
    Mode modes[kModes]{};
    // A name or option: 1 to kChars characters, no control characters.
    static bool validText(const char* text, size_t bytes);
    bool valid() const;
    int find(const char* name) const;
};
WheelBoard defaultWheel();
// 语音笔记: text only, newest first. On the device the notes have an NVS of their own in free flash (0x6B0000, after
// the wake model), mounted while the page is open, so they never crowd the settings.
struct MemoNote { static constexpr unsigned kMax = 100, kText = 1024; uint32_t at; char text[kText]; };
class MemoStore {
public:
    static bool mount();
    static void unmount();
    static unsigned count();
    static bool load(unsigned position, MemoNote& note);  // 0 = newest
    // False when the store is full (kMax notes or out of space): nothing is dropped to make room.
    static bool add(const char* text, uint32_t at);
    static bool remove(unsigned position);
};
class SettingsStore {
public:
    uint32_t reads{}, writes{};
    bool load(SystemSettings& settings);
    bool save(const SystemSettings& settings);
    bool loadOrCreate(SystemSettings& settings, bool& created);
    // Separate key: the config_v1 blob layout must not change.
    // One-byte system choices kept beside config_v1: "home_screen", "wake_word".
    static uint8_t loadByte(const char* key);
    static bool saveByte(const char* key, uint8_t value);
    static bool loadWords(WordDeck& deck);
    static bool saveWords(const WordDeck& deck);
    static bool loadStatus(StatusBoard& board);
    static bool saveStatus(const StatusBoard& board);
    static bool loadRates(RateBoard& board);
    static bool saveRates(const RateBoard& board);
    static bool loadCountdown(CountdownBoard& board);
    static bool saveCountdown(const CountdownBoard& board);
    static bool loadQr(unsigned index, QrEntry& entry);
    static bool saveQr(unsigned index, const QrEntry& entry);
    static bool loadTools(ToolPrefs& prefs);
    static bool saveTools(const ToolPrefs& prefs);
    static bool loadAlarms(AlarmList& list);
    static bool saveAlarms(const AlarmList& list);
    static bool loadTodo(TodoList& list);
    static bool saveTodo(const TodoList& list);
    static bool loadLedger(LedgerBook& book);
    static bool saveLedger(const LedgerBook& book);
    static bool loadGuard(GuardPrefs& prefs);
    static bool saveGuard(const GuardPrefs& prefs);
    static bool loadWheel(WheelBoard& board);
    static bool saveWheel(const WheelBoard& board);
    static bool loadWifi(WifiList& list);
    static bool saveWifi(const WifiList& list);
};
} // namespace passport
