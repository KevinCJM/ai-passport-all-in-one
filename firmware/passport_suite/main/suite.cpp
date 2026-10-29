#include "suite.hpp"
#include <algorithm>
#include <cstring>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "todo_time.hpp"
#include "voice_add.hpp"
#include "freertos/semphr.h"
#include "word_list.hpp"
#include <ctime>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <cstdio>

namespace passport {
namespace {
constexpr Profile kProfiles[] = {
    {"home", "时钟与天气", 0, false, false},
    {"chat", "小智语音", 88 * 1024, true, false},
    {"dictation", "语音输入", 32 * 1024, true, false},
    {"codex", "AI 额度", 4 * 1024, false, false},
    {"focus", "番茄钟", 2 * 1024, false, false},
    {"monitor", "电脑状态", 8 * 1024, false, false},
    {"reserved", "", 0, false, false}, // Protocol ID 6 is retired; never routable.
    {"menu", "功能列表", 0, false, false},
    {"retired-otp", "", 0, false, false},
    {"radio", "收音机", 64 * 1024, true, false},
    {"settings", "系统设置", 0, false, false},
    {"words", "背单词", 64 * 1024, true, false},
    {"slides", "PPT 翻页", 2 * 1024, false, false},
    {"alert", "警示灯", 0, false, false},
    {"status", "状态牌", 0, false, false},
    {"calendar", "万年历", 0, false, false},
    {"countdown", "倒数日", 0, false, false},
    {"torch", "手电筒", 0, false, false},
    {"qr", "二维码", 0, false, false},
    {"news", "财经新闻", 0, false, false},
    {"sonar", "蓝牙声纳", 0, false, false},
    {"rate", "汇率", 0, false, false},
    {"stopwatch", "秒表", 0, false, false},
    {"history", "历史今日", 0, false, false},
    {"daily", "每日一句", 0, true, false},
    {"quake", "地震速报", 0, false, false},
    {"quiz", "知识问答", 0, false, false},
    {"hacker", "Hacker News", 0, false, false, "HN头条"},
    {"wifi", "WiFi 工具箱", 0, false, false, "WiFi"},
    {"sound", "声音", 0, true, false},
    {"motion", "移动监控", 0, false, false},
    {"podcast", "播客", 0, true, false},
    {"forecast", "天气预报", 0, false, false},
    {"noise", "白噪音", 0, true, false},
    {"alarm", "闹钟", 0, false, false},
    {"stock", "自选股", 0, false, false},
    {"typhoon", "台风路径", 0, false, false, "台风"},
    {"hot", "百度热榜", 0, false, false, "热榜"},
    {"memo", "语音笔记", 0, true, false},
    {"wheel", "随机转盘", 0, false, false},
    {"translate", "中英翻译", 0, true, false},
    {"guard", "离座锁屏", 0, false, false},
    {"todo", "待办提醒", 0, false, false},
    {"ledger", "语音记账", 0, false, false},
    {"calc", "语音计算器", 0, false, false, "计算器"},
    {"voice", "语音指令", 0, true, false},
    {"walkie", "对讲机", 0, true, false},
};
static_assert(std::size(kProfiles) == static_cast<unsigned>(App::Count));
}
namespace {
StaticSemaphore_t ledgerLockStorage;
SemaphoreHandle_t ledgerLock;
std::atomic<unsigned> ledgerPending;
std::atomic<int32_t> ledgerMonthOut, ledgerMonthIn;
std::atomic<uint32_t> ledgerMonth;
std::atomic<bool> ledgerTouched;
uint32_t monthKey() { const time_t now = time(nullptr); tm t{}; localtime_r(&now, &t); return uint32_t((t.tm_year + 1900) * 100 + t.tm_mon + 1); }
template <class Edit> bool editLedger(LedgerBook& book, Edit edit) {
    if (!ledgerLock || xSemaphoreTake(ledgerLock, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
    SettingsStore::loadLedger(book);
    const bool pruned = time(nullptr) > 1700000000 && book.prune(ledgerToday());
    const bool ok = !(edit(book) || pruned) || SettingsStore::saveLedger(book);
    ledgerPending = book.pending();
    xSemaphoreGive(ledgerLock);
    return ok;
}
void ledgerScroll(Model& m) {
    if (!m.ledgerIndex) m.ledgerTop = 0;
    else if (m.ledgerIndex - 1 < m.ledgerTop) m.ledgerTop = m.ledgerIndex - 1;
    else if (m.ledgerIndex > m.ledgerTop + Model::kLedgerRows) m.ledgerTop = static_cast<uint8_t>(m.ledgerIndex - Model::kLedgerRows);
}
}
const Profile& profile(App app) { return kProfiles[static_cast<unsigned>(app)]; }

bool Worker::init(QueueHandle_t events) {
    events_ = events;
#ifdef PASSPORT_DEVICE
    // Device pages own their real I/O; the synthetic worker only serves the simulator's fault tests.
    return true;
#endif
    commands_ = xQueueCreate(4, sizeof(Command));
    acks_ = xQueueCreate(4, sizeof(Ack));
    const bool ready = commands_ && acks_ && xTaskCreate(run, "suite_worker", 4096, this, 3, &task_) == pdPASS;
    if (!ready) {
        if (commands_) vQueueDelete(commands_);
        if (acks_) vQueueDelete(acks_);
        commands_ = acks_ = nullptr;
    }
    return ready;
}
void Worker::run(void* arg) {
    auto& self = *static_cast<Worker*>(arg);
    uint8_t* data = nullptr;
    size_t size = 0;
    App app = App::Home;
    uint32_t generation = 0, frames = 0;
    for (;;) {
        Command command{};
        if (xQueueReceive(self.commands_, &command, pdMS_TO_TICKS(2)) == pdTRUE) {
            if (command.op == Op::Stop && self.stallOnce_.exchange(false)) {
                continue; // Fault injection: deliberately omit ACK, keep owning memory.
            }
            if (command.op == Op::Bind) {
                data = command.data; size = command.size;
                app = command.app; generation = command.generation;
            } else {
                data = nullptr; size = 0;
            }
            const Ack ack{command.ticket};
            xQueueSend(self.acks_, &ack, 0);
            if (command.op == Op::Halt) { vTaskSuspend(nullptr); }
        }
        if (data && size) {
            // Real CPU work and writes into the owned arena, not a memory counter.
            // These deterministic sample blocks replace external transports/codecs.
            for (size_t i = 0; i < 640 && i < size; ++i) data[i] = (i + frames) & 255;
            ++frames;
            if ((frames & 7) == 0) {
                const Event e{app, generation, frames};
                xQueueSend(self.events_, &e, 0);
            }
        }
    }
}
bool Worker::request(Op op, App app, uint32_t generation, uint8_t* data, size_t size) {
    if (!commands_) return true; // No worker task: nothing to quiesce.
    const uint32_t ticket = ++ticket_;
    const Command command{op, app, generation, ticket, data, size};
    if (xQueueSend(commands_, &command, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    const auto started = xTaskGetTickCount();
    Ack ack{};
    do {
        if (xQueueReceive(acks_, &ack, pdMS_TO_TICKS(5)) == pdTRUE && ack.ticket == ticket) return true;
    } while (static_cast<TickType_t>(xTaskGetTickCount() - started) < pdMS_TO_TICKS(100));
    return false;
}
bool Worker::bind(App app, uint32_t generation, void* data, size_t size) {
    return request(Op::Bind, app, generation, static_cast<uint8_t*>(data), size);
}
bool Worker::stop() { return request(Op::Stop, App::Home, 0, nullptr, 0); }
bool Worker::shutdown() {
    if (task_ && !request(Op::Halt, App::Home, 0, nullptr, 0)) return false;
    if (task_) { vTaskDelete(task_); task_ = nullptr; }
    if (commands_) vQueueDelete(commands_);
    if (acks_) vQueueDelete(acks_);
    commands_ = acks_ = nullptr;
    return true;
}
bool Runtime::init(SystemSettings& settings) {
    settings_ = &settings;
    const auto screen = SettingsStore::loadByte("home_screen");
    model.wakeWord = SettingsStore::loadByte("wake_word") == 1;
    model.voiceJump = SettingsStore::loadByte("jump_off") != 1;  // stored as off, so a badge that never saved it has it on
    model.macOpen = SettingsStore::loadByte("mac_off") != 1;
    model.usbAwake = SettingsStore::loadByte("usb_dim") != 1;
    model.walkieListen = SettingsStore::loadByte("walkie_bg") == 1;
    if (const auto channel = SettingsStore::loadByte("walkie_ch"); channel >= 1 && channel <= 9) model.walkieChannel = channel;
    if (!SettingsStore::loadAlarms(model.alarms)) model.alarms = {};
    reloadTodo();
    if (!ledgerLock) ledgerLock = xSemaphoreCreateMutexStatic(&ledgerLockStorage);
    if (std::unique_ptr<LedgerBook> book(new (std::nothrow) LedgerBook); book) editLedger(*book, [](LedgerBook&) { return false; });
    if (screen < static_cast<uint8_t>(HomeScreen::Count)) model.homeScreen = static_cast<HomeScreen>(screen);
    events_ = xQueueCreate(32, sizeof(Event));
    if (events_ && worker_.init(events_)) return true;
    if (events_) vQueueDelete(events_);
    events_ = nullptr;
    return false;
}
void Model::chatRecord(unsigned turn, const char* heard, const char* sentence) {
    const auto push=[this](bool user,const char* text){
        if(chatCount==chat.size()){std::move(chat.begin()+1,chat.end(),chat.begin());--chatCount;}
        chat[chatCount].user=user;snprintf(chat[chatCount].text,sizeof(chat[chatCount].text),"%s",text);++chatCount;
    };
    if(turn!=chatTurn&&*heard){chatTurn=turn;push(true,heard);chatSentence[0]=0;++chatRevision;}
    if(!*sentence||!strcmp(sentence,chatSentence))return;
    snprintf(chatSentence,sizeof(chatSentence),"%s",sentence);++chatRevision;
    auto& last=chat[chatCount?chatCount-1:0];
    if(chatCount&&!last.user&&strlen(last.text)+strlen(sentence)<sizeof(last.text))strcat(last.text,sentence);else push(false,sentence);
}
unsigned Model::statusCount() const {
    unsigned count = std::size(kStatusLabels);
    for (auto& text : status.custom) count += *text != 0;
    return count;
}
StatusLabel Model::statusLabel(unsigned index) const {
    for (auto& text : status.custom) if (*text && !index--) return {text, "", 0x0f766e};
    return kStatusLabels[std::min<unsigned>(index, std::size(kStatusLabels) - 1)];
}
namespace {
Date today(int* minutes = nullptr) {
    const time_t now = time(nullptr); tm t{}; localtime_r(&now, &t);
    if (minutes) *minutes = t.tm_hour * 60 + t.tm_min;
    return {t.tm_year + 1900, t.tm_mon + 1, t.tm_mday};
}
// The next date on or after `from` with this month/day (29 Feb waits for a leap year).
Date nextSolar(Date from, int month, int day) {
    for (int y = from.year; y < from.year + 8; ++y)
        if (day <= monthDays(y, month) && dayNumber({y, month, day}) >= dayNumber(from)) return {y, month, day};
    return from;
}
// Lunar month/day, the day clamped to a short month; month 0 = 除夕 (the day before 春节).
Date nextLunar(Date from, int month, int day) {
    for (int y = from.year - 1; y <= from.year + 1; ++y) {
        Date d{};
        if (!month) { if (!fromLunar(y + 1, 1, 1, d)) continue; d = fromDayNumber(dayNumber(d) - 1); }
        else if (!fromLunar(y, month, std::min(day, lunarMonthDays(y, month, false)), d)) continue;
        if (dayNumber(d) >= dayNumber(from)) return d;
    }
    return from;
}
}
unsigned Model::countdownItems(Date now, int minutes, CountdownItem* out, unsigned capacity) const {
    unsigned count = 0;
    const int base = dayNumber(now), week = weekday(now), off = holiday(now);
    const auto add = [&](const char* name, Date date, int mins = -1) { if (count < capacity) out[count++] = {name, date, dayNumber(date) - base, mins}; };
    if (countdown.offwork && minutes < countdown.offwork && ((week >= 1 && week <= 5 && off != 1) || off == 2)) add("下班", now, countdown.offwork - minutes);
    add("周末", week == 6 || week == 0 ? now : fromDayNumber(base + 6 - week));
    add("元旦", nextSolar(now, 1, 1)); add("春节", nextLunar(now, 1, 1)); add("除夕", nextLunar(now, 0, 0));
    for (int y : {now.year, now.year + 1}) if (dayNumber({y, 4, solarTermDay(y, 6)}) >= base) { add("清明", {y, 4, solarTermDay(y, 6)}); break; }
    add("劳动节", nextSolar(now, 5, 1)); add("端午", nextLunar(now, 5, 5)); add("七夕", nextLunar(now, 7, 7));
    add("中秋", nextLunar(now, 8, 15)); add("国庆", nextSolar(now, 10, 1)); add("圣诞", nextSolar(now, 12, 25));
    for (auto& e : countdown.events) {
        if (!*e.name) continue;
        const int y = e.date / 10000, m = e.date / 100 % 100, d = e.date % 100;
        Date date{y, m, d};
        if (e.flags & CountdownBoard::kYearly) date = e.flags & CountdownBoard::kLunar ? nextLunar(now, m, d) : nextSolar(now, m, d);
        else if (e.flags & CountdownBoard::kLunar && !fromLunar(y, m, d, date)) continue;
        add(e.name, date);
    }
    // Nearest first (下班 leads today); one-off events already past go last, most recent first.
    const auto rank = [](const CountdownItem& i) { return i.days >= 0 ? i.days * 2 + (i.minutes < 0) : 1000000 - i.days; };
    std::stable_sort(out, out + count, [&](const CountdownItem& a, const CountdownItem& b) { return rank(a) < rank(b); });
    return count;
}
unsigned Model::sonarVisible(uint8_t* order) const {
    unsigned count = 0;
    for (unsigned i = 0; i < sonarCount; ++i) if ((*sonar[i].name != 0) == sonarNamed) order[count++] = i;
    std::sort(order, order + count, [this](uint8_t a, uint8_t b) { return sonar[a].rssi > sonar[b].rssi; });
    return count;
}
void Runtime::reloadMemo() {
    auto& m = model;
    m.memoCount = static_cast<uint8_t>(MemoStore::count());
    if (m.memoIndex > m.memoCount) m.memoIndex = m.memoCount;
    // Keep the picked note on show, and the window full where there are notes enough.
    const unsigned note = m.memoIndex ? m.memoIndex - 1 : 0;
    if (note < m.memoTop) m.memoTop = note;
    if (note >= m.memoTop + Model::kMemoRows) m.memoTop = note - Model::kMemoRows + 1;
    m.memoTop = std::min<unsigned>(m.memoTop, m.memoCount > Model::kMemoRows ? m.memoCount - Model::kMemoRows : 0);
    for (unsigned i = 0; i < Model::kMemoRows; ++i) {
        auto& row = m.page.memo.rows[i]; row = {};
        auto& note = m.page.memo.note;  // scratch: the list shows no open note
        if (m.memoTop + i >= m.memoCount || !MemoStore::load(m.memoTop + i, note)) continue;
        row.at = note.at; const size_t bytes = strlen(note.text);
        for (size_t k = 0; k < bytes; ++k) row.chars += (static_cast<unsigned char>(note.text[k]) & 0xc0) != 0x80;
        size_t n = std::min(bytes, sizeof(row.head) - 1);
        while (n && (static_cast<unsigned char>(note.text[n]) & 0xc0) == 0x80) --n;
        memcpy(row.head, note.text, n);
    }
}
bool Runtime::addMemo(const char* text, uint64_t now) {
    auto& m = model; m.lastNotice[0] = 0;
    const time_t clock = time(nullptr);
    if (!m.memoMounted || !MemoStore::add(text, clock > 1700000000 ? static_cast<uint32_t>(clock) : 0)) {
        strcpy(m.lastNotice, m.memoCount >= MemoNote::kMax ? "已存满 100 条，请先删除旧笔记" : "笔记保存失败，空间不足");
        return false;
    }
    // The 录音 row stays picked, so the next Confirm records the next note; the new one is first below it.
    m.memoDetail = m.memoArmed = false; m.memoIndex = 0; m.memoTop = 0; m.memoSavedAt = now;
    reloadMemo(); wake(now);
    return true;
}
void Runtime::reloadCountdown() {
    if (!SettingsStore::loadCountdown(model.countdown)) model.countdown = {};
    model.countdownIndex = 0;
}
unsigned Runtime::qrCount() const { return __builtin_popcount(model.qrMask) + (*settings_->wifiSsid ? 1 : 0); }
void Runtime::reloadQr() {
    auto& m = model; QrEntry entry{};
    m.qrMask = 0;
    for (unsigned i = 0; i < QrEntry::kCount; ++i) if (SettingsStore::loadQr(i, entry) && *entry.text) m.qrMask |= 1 << i;
    if (m.qrIndex >= qrCount()) m.qrIndex = 0;
    m.qrTitle[0] = m.qrText[0] = 0;
    unsigned skip = m.qrIndex;
    for (unsigned i = 0; i < QrEntry::kCount; ++i)
        if (m.qrMask >> i & 1 && !skip--) { SettingsStore::loadQr(i, entry); strcpy(m.qrTitle, entry.title); strcpy(m.qrText, entry.text); return; }
    if (!*settings_->wifiSsid) return;
    // The phone camera's WiFi format; \ ; , : " in the name or password are escaped.
    const auto escape = [](char* out, const char* in) { for (; *in; ++in) { if (strchr("\\;,:\"", *in)) *out++ = '\\'; *out++ = *in; } *out = 0; };
    char ssid[65], password[129];
    escape(ssid, settings_->wifiSsid); escape(password, settings_->wifiPassword);
    snprintf(m.qrTitle, sizeof(m.qrTitle), "WiFi · %s", settings_->wifiSsid);
    snprintf(m.qrText, sizeof(m.qrText), "WIFI:T:%s;S:%s;P:%s;;", *password ? "WPA" : "nopass", ssid, password);
}
bool Runtime::longKey(bool up, uint64_t now) {
    tick(now); lastInput_ = now;
    auto& m = model;
    if (active_ == App::Menu) {
        constexpr unsigned pages = std::size(kMenuPages);
        selection_ = kMenuStarts[(menuPageOf(selection_) + (up ? pages - 1 : 1)) % pages];
        return true;
    }
    if (active_ == App::Calendar) {
        const int month = m.calendarYear * 12 + m.calendarMonth - 1 + (up ? -1 : 1);
        if (month / 12 > kLunarFirstYear && month / 12 < kLunarLastYear) {
            m.calendarYear = month / 12; m.calendarMonth = month % 12 + 1;
            m.calendarDay = std::min(m.calendarDay, monthDays(m.calendarYear, m.calendarMonth));
        }
        return true;
    }
    // 对讲机: long Up/Down step the channel round 1-9 (not while talking) and save it.
    if (active_ == App::Walkie) {
        m.walkieAuto = false; m.lastNotice[0] = 0;
        if (m.walkieTalk) { strcpy(m.lastNotice, "说话时不能换频道"); return true; }
        m.walkieChannel = static_cast<uint8_t>((m.walkieChannel + (up ? 7 : 0)) % 9 + 1);
        if (!SettingsStore::saveByte("walkie_ch", m.walkieChannel)) strcpy(m.lastNotice, "保存失败");
        return true;
    }
    if (active_ == App::Wifi) { m.wifiTab = (m.wifiTab + (up ? 3 : 1)) % 4; m.wifiIndex = 0; return true; }
    if (active_ == App::Motion) { m.tools.motionAlarm = static_cast<uint8_t>((m.tools.alarm() + (up ? 1 : 2)) % 3); if (!SettingsStore::saveTools(m.tools)) strcpy(m.lastNotice, "保存失败"); return true; }
    // 白噪音: long Up/Down set the volume (main hands it to the codec); 闹钟: in an alarm's clock they move it an hour,
    // in the list a long Down asks to delete the picked alarm and a second one deletes it.
    if (active_ == App::Noise) { m.volume = static_cast<uint8_t>(std::clamp(int(m.volume) + (up ? 10 : -10), 0, 100)); return true; }
    if (active_ == App::Alarm && !m.alarmRinging && !m.voiceBusy()) {
        auto& list = m.alarms;
        if (m.alarmEdit && m.alarmIndex && m.alarmIndex <= list.count) {
            auto& a = list.items[m.alarmIndex - 1];
            a.hour = (a.hour + (up ? 1 : 23)) % 24; m.alarmSnooze = 0;
            if (!SettingsStore::saveAlarms(list)) strcpy(m.lastNotice, "保存失败");
            return true;
        }
        if (up || !m.alarmIndex || m.alarmIndex > list.count) { m.alarmArmed = false; return true; }
        if (!m.alarmArmed) { m.alarmArmed = true; m.lastNotice[0] = 0; return true; }
        m.alarmArmed = false;
        std::copy(list.items + m.alarmIndex, list.items + list.count, list.items + m.alarmIndex - 1); list.items[--list.count] = {};
        if (m.alarmIndex > list.count) m.alarmIndex = list.count;
        strcpy(m.lastNotice, SettingsStore::saveAlarms(list) ? "已删除" : "保存失败");
        return true;
    }
    // 汇率: a long Down asks to delete the picked currency, a second one deletes it (the base stays).
    if (active_ == App::Rate) {
        if (up || !m.rateCount || m.voiceBusy()) { m.rateArmed = false; return true; }
        if (m.rateCount == 1) { strcpy(m.lastNotice, "至少保留 1 种货币"); return true; }
        if (!m.rateArmed) { m.rateArmed = true; m.lastNotice[0] = 0; return true; }
        m.rateArmed = false;
        const unsigned at = m.rateIndex % m.rateCount; char code[4]; strcpy(code, m.rates[at].code);
        RateBoard board; if (!SettingsStore::loadRates(board)) board = {};
        unsigned kept = 0;
        for (auto& t : board.targets) if (*t && strcmp(t, code)) memmove(board.targets[kept++], t, sizeof(t));
        for (unsigned i = kept; i < RateBoard::kTargets; ++i) *board.targets[i] = 0;
        if (!SettingsStore::saveRates(board)) { strcpy(m.lastNotice, "保存失败"); return true; }
        std::copy(m.rates.begin() + at + 1, m.rates.begin() + m.rateCount, m.rates.begin() + at); --m.rateCount;
        if (m.rateIndex >= m.rateCount) m.rateIndex = m.rateCount - 1;
        snprintf(m.lastNotice, sizeof(m.lastNotice), "已删除%s", currencyName(code));
        return true;
    }
    // 倒数日: a long Down asks to delete the event on show (only those added), a second one deletes it.
    if (active_ == App::Countdown) {
        if (up || m.voiceBusy()) { m.countdownArmed = false; return true; }
        CountdownItem items[20]; int minutes = 0; const Date now = today(&minutes);
        const unsigned count = m.countdownItems(now, minutes, items, std::size(items));
        if (!count) return true;
        const char* name = items[m.countdownIndex % count].name; int slot = -1;
        for (unsigned i = 0; i < CountdownBoard::kEvents; ++i) if (name == m.countdown.events[i].name) slot = static_cast<int>(i);
        if (slot < 0) { m.countdownArmed = false; snprintf(m.lastNotice, sizeof(m.lastNotice), "「%s」是内置的，不能删除", name); return true; }
        if (!m.countdownArmed) { m.countdownArmed = true; m.lastNotice[0] = 0; return true; }
        m.countdownArmed = false;
        char removed[25]; strcpy(removed, name);
        m.countdown.events[slot] = {};
        if (!SettingsStore::saveCountdown(m.countdown)) { strcpy(m.lastNotice, "保存失败"); return true; }
        if (m.countdownIndex + 1u >= count && m.countdownIndex) --m.countdownIndex;
        snprintf(m.lastNotice, sizeof(m.lastNotice), "已删除「%s」", removed);
        return true;
    }
    // 语音笔记: a long Down on the picked or open note asks to delete it, a second one deletes it; anything else cancels.
    if (active_ == App::Memo) {
        if (!m.memoIndex || up || m.voiceBusy()) { m.memoArmed = false; return true; }
        if (!m.memoArmed) { m.memoArmed = true; return true; }
        m.memoArmed = m.memoDetail = false;
        if (!MemoStore::remove(m.memoIndex - 1)) strcpy(m.lastNotice, "删除失败");
        reloadMemo();
        return true;
    }
    if (active_ == App::Sound) { m.soundView = (m.soundView + (up ? 2 : 1)) % 3; m.soundRunning = false; return true; }
    if (active_ == App::Status) {
        if (m.voiceBusy()) return true;
        m.status.awake = !m.status.awake;
        if (!SettingsStore::saveStatus(m.status)) strcpy(m.lastNotice, "保存失败");
        return true;
    }
    // 语音记账: a long Down asks to delete the selected entry, a second one deletes it; anything else cancels.
    if (active_ == App::Ledger) {
        if (!m.ledgerIndex || up || m.voiceBusy()) { m.ledgerArmed = false; return true; }
        if (!m.ledgerArmed) { m.ledgerArmed = true; m.lastNotice[0] = 0; return true; }
        m.ledgerArmed = false;
        uint8_t order[LedgerBook::kMax];
        if (m.ledgerIndex > m.page.ledger.shown(ledgerToday(), order)) return true;
        const uint32_t id = m.page.ledger.items[order[m.ledgerIndex - 1]].id;
        const bool ok = editLedger(m.page.ledger, [&](LedgerBook& b) { const int i = b.find(id); if (i >= 0) b.remove(static_cast<unsigned>(i)); return i >= 0; });
        const unsigned shown = m.page.ledger.shown(ledgerToday(), order);
        if (m.ledgerIndex > shown) m.ledgerIndex = static_cast<uint8_t>(shown);
        ledgerScroll(m);
        strcpy(m.lastNotice, ok ? "已删除" : "保存失败");
        return true;
    }
    // 待办提醒: a long Up clears the done todos; a long Down asks to delete the selected one, a second one deletes it.
    if (active_ == App::Todo) {
        auto& list = m.page.todo;
        if (m.todoRinging || m.voiceBusy()) return true;
        if (up) {
            m.todoArmed = false; unsigned kept = 0;
            for (unsigned i = 0; i < list.count; ++i) if (!(list.items[i].flags & TodoList::kDone)) list.items[kept++] = list.items[i];
            const unsigned removed = list.count - kept;
            for (unsigned i = kept; i < list.count; ++i) list.items[i] = {};
            list.count = static_cast<uint8_t>(kept); m.todoIndex = m.todoTop = 0;
            if (!removed) strcpy(m.lastNotice, "没有已完成的待办");
            else if (!SettingsStore::saveTodo(list)) strcpy(m.lastNotice, "保存失败");
            else snprintf(m.lastNotice, sizeof(m.lastNotice), "已清除 %u 条已完成", removed);
            reloadTodo();
            return true;
        }
        if (!m.todoIndex) { m.todoArmed = false; return true; }
        if (!m.todoArmed) { m.todoArmed = true; m.lastNotice[0] = 0; return true; }
        m.todoArmed = false;
        std::copy(list.items + m.todoIndex, list.items + list.count, list.items + m.todoIndex - 1); list.items[--list.count] = {};
        if (m.todoIndex > list.count) m.todoIndex = list.count;
        if (m.todoTop && m.todoTop + Model::kTodoRows > list.count) m.todoTop = static_cast<uint8_t>(list.count > Model::kTodoRows ? list.count - Model::kTodoRows : 0);
        strcpy(m.lastNotice, SettingsStore::saveTodo(list) ? "已删除" : "保存失败");
        reloadTodo();
        return true;
    }
    // 随机转盘: a long Down asks to delete the mode on show, a second one deletes it; anything else cancels.
    // (A long Up speaks: main.cpp hands it to LiveServices.)
    if (active_ == App::Wheel) {
        auto& w = m.page.wheel;
        if (up || m.voiceBusy() || m.wheelSpinning() || !w.count) { m.wheelArmed = false; return true; }
        if (!m.wheelArmed) { m.wheelArmed = true; m.lastNotice[0] = 0; return true; }
        m.wheelArmed = false; m.wheelPick = 255;
        std::copy(w.modes + w.current + 1, w.modes + w.count, w.modes + w.current); w.modes[--w.count] = {};
        if (w.current >= w.count) w.current = w.count ? w.count - 1 : 0;
        strcpy(m.lastNotice, SettingsStore::saveWheel(w) ? "已删除" : "保存失败");
        return true;
    }
    if (active_ != App::Sonar) return false;
    m.sonarNamed = !m.sonarNamed;
    memset(m.sonarFocus, 0, sizeof(m.sonarFocus));
    return true;
}
unsigned Model::wheelLit() const {
    const auto& mode = page.wheel.modes[page.wheel.current];
    if (!mode.count) return 0;
    if (!wheelSpinning()) return wheelPick % mode.count;
    // Ease out: fast at first, then slower and slower until it stops.
    const float x = float(uint64_t(esp_timer_get_time()) - wheelSpunAt) / kWheelSpin, left = 1 - x;
    return (wheelFrom + unsigned((1 - left * left * left) * wheelSteps)) % mode.count;
}
void Runtime::spinWheel() {
    auto& m = model; const auto& w = m.page.wheel;
    if (!w.count || w.modes[w.current].count < 2) { strcpy(m.lastNotice, "至少要 2 个选项才能转"); return; }
    const unsigned n = w.modes[w.current].count, loops = std::max(3u, (20 + n - 1) / n);
    m.wheelFrom = m.wheelPick < n ? m.wheelPick : 0;
    m.wheelPick = esp_random() % n;
    m.wheelSteps = loops * n + (m.wheelPick + n - m.wheelFrom) % n;
    m.wheelSpunAt = esp_timer_get_time(); m.wheelArmed = false; m.lastNotice[0] = 0;
}
namespace {
// Bytes of the separator at s (punctuation or a space the cloud puts between spoken items), or 0.
size_t separatorAt(const char* s) {
    if (strchr(" ,.;:!?\n", *s) && *s) return 1;
    for (const char* mark : {"，", "、", "。", "；", "：", "！", "？", "～"}) if (!strncmp(s, mark, 3)) return 3;
    return 0;
}
}
bool Runtime::addWheel(const char* heard) {
    auto& m = model; auto& w = m.page.wheel;
    char parts[WheelBoard::kOptions + 1][WheelBoard::kText]; unsigned count = 0; bool tooLong = false, tooMany = false, named = false;
    // The cloud often runs a short name into the first option ("吃饭烧烤，汉堡"), so 有 also ends the first part:
    // "吃饭有烧烤、汉堡、炸鸡".
    const auto ends = [&](const char* s, const char* start) { return separatorAt(s) || (!count && s != start && !strncmp(s, "有", 3)); };
    for (const char* s = heard; *s;) {
        if (const size_t n = separatorAt(s)) { s += n; continue; }
        const char* start = s; while (*s && !ends(s, start)) ++s;
        const size_t n = s - start;
        if (!count && !strncmp(s, "有", 3)) { s += 3; named = true; }
        if (!WheelBoard::validText(start, n)) { tooLong = true; continue; }
        if (count == std::size(parts)) { tooMany = true; continue; }
        memcpy(parts[count], start, n); parts[count][n] = 0; ++count;
    }
    if (tooLong) { strcpy(m.lastNotice, "每项最多 8 个字，说完一项停一下"); return false; }
    if (!count) { strcpy(m.lastNotice, "没有听清，请再说一次"); return false; }
    const auto has = [](const WheelBoard::Mode& mode, const char* option) {
        for (unsigned i = 0; i < mode.count; ++i) if (!strcmp(mode.options[i], option)) return true;
        return false;
    };
    if (count >= 3 || (named && count == 2)) {
        // A mode: its name (said before 有, or the first of three or more), then its options; saying an existing name
        // again replaces its options.
        if (tooMany) { strcpy(m.lastNotice, "一个模式最多 8 个选项"); return false; }
        int at = w.find(parts[0]);
        if (at < 0 && w.count == WheelBoard::kModes) { strcpy(m.lastNotice, "最多 5 个模式，长按下键删一个"); return false; }
        if (at < 0) at = w.count++;
        auto& mode = w.modes[at]; mode = {}; strcpy(mode.name, parts[0]);
        for (unsigned i = 1; i < count; ++i) if (!has(mode, parts[i])) strcpy(mode.options[mode.count++], parts[i]);
        w.current = at;
        snprintf(m.lastNotice, sizeof(m.lastNotice), "「%s」%u 个选项", mode.name, mode.count);
    } else {
        // One or two items join the mode on show.
        if (!w.count) { strcpy(m.lastNotice, "先说模式名和选项，如：吃饭有面条、米饭"); return false; }
        auto& mode = w.modes[w.current]; unsigned added = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (has(mode, parts[i])) continue;
            if (mode.count == WheelBoard::kOptions) { strcpy(m.lastNotice, "这个模式已满 8 个选项"); break; }
            strcpy(mode.options[mode.count++], parts[i]); ++added;
        }
        if (!added) { if (mode.count < WheelBoard::kOptions) strcpy(m.lastNotice, "已经有这个选项了"); return false; }
        snprintf(m.lastNotice, sizeof(m.lastNotice), "已加到「%s」", mode.name);
    }
    m.wheelPick = 255; m.wheelSpunAt = 0;
    if (!SettingsStore::saveWheel(w)) strcpy(m.lastNotice, "保存失败");
    return true;
}
void Runtime::reloadTodo() {
    std::unique_ptr<TodoList> owned; const TodoList* list = &model.page.todo;
    if (active_ != App::Todo) { owned.reset(new (std::nothrow) TodoList); if (!owned) return; SettingsStore::loadTodo(*owned); list = owned.get(); }
    const auto* next = list->next();
    model.todoNextAt = next ? next->at : 0; strcpy(model.todoNext, next ? next->text : "");
}
void Runtime::endTodoRing(TodoRing how) {
    auto& m = model; m.todoRinging = false;
    // The page's own copy, or (a reminder missed while the badge was off) the stored one.
    std::unique_ptr<TodoList> owned; TodoList* list = &m.page.todo;
    if (active_ != App::Todo) { owned.reset(new (std::nothrow) TodoList); if (!owned) return; SettingsStore::loadTodo(*owned); list = owned.get(); }
    for (unsigned i = 0; i < list->count; ++i) {
        auto& t = list->items[i];
        if (t.at != m.todoNextAt || strcmp(t.text, m.todoNext)) continue;
        if (how == TodoRing::Done) t.flags |= TodoList::kDone | TodoList::kRung;
        else if (how == TodoRing::Snooze) { t.at = static_cast<uint32_t>(time(nullptr) + 600); t.flags &= ~TodoList::kRung; }
        else t.flags |= TodoList::kRung;
        break;
    }
    list->sort();
    if (!SettingsStore::saveTodo(*list)) strcpy(m.lastNotice, "保存失败");
    else if (how == TodoRing::Snooze) strcpy(m.lastNotice, "10 分钟后再提醒");
    reloadTodo();
}
namespace {
// Selects the todo with this reminder and text (after a sort moved it): its row, scrolled into view.
void selectTodo(Model& m, uint32_t at, const char* text) {
    for (unsigned i = 0; i < m.page.todo.count; ++i)
        if (m.page.todo.items[i].at == at && !strcmp(m.page.todo.items[i].text, text)) { m.todoIndex = static_cast<uint8_t>(i + 1); break; }
    if (m.todoIndex == 0) m.todoTop = 0;
    else if (m.todoIndex - 1 < m.todoTop) m.todoTop = m.todoIndex - 1;
    else if (m.todoIndex > m.todoTop + Model::kTodoRows) m.todoTop = static_cast<uint8_t>(m.todoIndex - Model::kTodoRows);
}
}
bool Runtime::addTodo(const char* heard) {
    auto& m = model; auto& list = m.page.todo;
    auto parsed = parseTodo(heard, time(nullptr));
    const std::string& text = parsed.text;
    if (text.empty()) { strcpy(m.lastNotice, "没有听清，请再说一次"); return false; }
    if (list.count == TodoList::kMax) { strcpy(m.lastNotice, "最多 16 条，先长按上键清除已完成的"); return false; }
    if (!m.clockValid) parsed.at = 0;  // no reminder from a clock that is not set
    // At most kChars characters (and what fits the record).
    size_t bytes = 0; unsigned chars = 0;
    while (bytes < text.size() && chars < TodoList::kChars) {
        const auto c = static_cast<unsigned char>(text[bytes]); const size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        if (bytes + n >= sizeof(TodoItem::text)) break;
        bytes += n; ++chars;
    }
    TodoItem item{}; item.at = static_cast<uint32_t>(parsed.at); memcpy(item.text, text.data(), bytes);
    list.items[list.count++] = item; list.sort();
    selectTodo(m, item.at, item.text); m.todoArmed = false;
    if (!SettingsStore::saveTodo(list)) { strcpy(m.lastNotice, "保存失败"); reloadTodo(); return true; }
    char when[32] = "";
    if (item.at) todoWhen(item.at, time(nullptr), when, sizeof(when));
    if (bytes < text.size()) strcpy(m.lastNotice, "太长了，只记下前 24 个字");
    else if (item.at) snprintf(m.lastNotice, sizeof(m.lastNotice), "已添加 · %s 提醒", when);
    else strcpy(m.lastNotice, "已添加（没说时间，不提醒）");
    reloadTodo();
    return true;
}
// 语音记账: the book lives in NVS; the open page holds a copy. Every change (here, or the computer's acknowledgements
// from its task) loads, edits and saves it under one lock, after dropping what the computer already has from before
// today.
time_t ledgerToday() { const time_t now = time(nullptr); tm t{}; localtime_r(&now, &t); t.tm_hour = t.tm_min = t.tm_sec = 0; t.tm_isdst = -1; return mktime(&t); }
LedgerStatus ledgerStatus() { return {ledgerPending.load(), ledgerMonth.load() == monthKey(), ledgerMonthOut.load(), ledgerMonthIn.load()}; }
void ledgerExchange(const LedgerAck* acks, unsigned count, int32_t monthOut, int32_t monthIn, uint32_t month, char* out, size_t size) {
    if (month) { ledgerMonthOut = monthOut; ledgerMonthIn = monthIn; ledgerMonth = month; }
    std::unique_ptr<LedgerBook> book(new (std::nothrow) LedgerBook);
    bool changed = false;
    if (!book || !editLedger(*book, [&](LedgerBook& b) { for (unsigned i = 0; i < count; ++i) changed |= b.ack(acks[i].id, acks[i].rev); return changed; })) {
        snprintf(out, size, "{\"error\":\"busy\"}"); return;
    }
    book->pendingJson(out, size, 6);
    if (changed || month) ledgerTouched = true;
}
bool Runtime::ledgerSynced() {
    if (!ledgerTouched.exchange(false)) return false;
    if (active_ != App::Ledger) return true;
    auto& m = model; editLedger(m.page.ledger, [](LedgerBook&) { return false; });
    uint8_t order[LedgerBook::kMax];
    const unsigned shown = m.page.ledger.shown(ledgerToday(), order);
    if (m.ledgerIndex > shown) m.ledgerIndex = static_cast<uint8_t>(shown);
    ledgerScroll(m);
    return true;
}
bool Runtime::addLedger(const char* heard) {
    auto& m = model;
    if (!*heard) { strcpy(m.lastNotice, "没有听清，请再说一次"); return false; }
    const auto parsed = parseLedger(heard);
    if (!parsed.ok) {
        // What was heard, cut at a character.
        size_t bytes = 0; while (heard[bytes] && bytes < 90) { const auto c = static_cast<unsigned char>(heard[bytes]); bytes += c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4; }
        snprintf(m.lastNotice, sizeof(m.lastNotice), "没听到金额：%.*s", int(bytes), heard); return false;
    }
    if (!m.clockValid) { strcpy(m.lastNotice, "时间未同步，暂不能记账"); return false; }
    bool full = false;
    const bool ok = editLedger(m.page.ledger, [&](LedgerBook& b) {
        full = !b.add(static_cast<uint32_t>(time(nullptr)), parsed.cents, parsed.category, parsed.note.c_str()); return !full; });
    m.ledgerIndex = m.ledgerTop = 0; m.ledgerArmed = false;
    if (full) { strcpy(m.lastNotice, ledgerPending ? "已满 30 笔，请连接电脑同步" : "今天已记满 30 笔"); return false; }
    if (!ok) { strcpy(m.lastNotice, "保存失败"); return false; }
    char amount[24]; ledgerAmount(parsed.cents, amount, sizeof(amount));
    snprintf(m.lastNotice, sizeof(m.lastNotice), "已记下 · %s %s %s", parsed.note.c_str(), kLedgerCategories[parsed.category], amount);
    return true;
}
bool Runtime::addCalc(const char* heard) {
    auto& m = model; auto& rows = m.page.calc;
    const double* ans = m.calcCount ? &rows[m.calcIndex].value : nullptr;
    const auto r = calc::evaluate(heard, ans);
    if (!r.ok) {
        size_t bytes = 0; while (heard[bytes] && bytes < 60) bytes += voice::charBytes(static_cast<unsigned char>(heard[bytes]));
        if (*heard) snprintf(m.lastNotice, sizeof(m.lastNotice), "%s：%.*s", r.error, int(bytes), heard); else strcpy(m.lastNotice, "没有听清，请再说一次");
        return false;
    }
    // The newest first; the oldest drops off a full list. A long sum is cut at a character.
    std::move_backward(rows.begin(), rows.end() - 1, rows.end());
    calc::Row& row = rows[0]; row.value = r.value;
    size_t bytes = 0; while (bytes < r.shown.size()) { const size_t n = voice::charBytes(static_cast<unsigned char>(r.shown[bytes])); if (bytes + n >= sizeof(row.shown)) break; bytes += n; }
    memcpy(row.shown, r.shown.data(), bytes); row.shown[bytes] = 0;
    m.calcCount = static_cast<uint8_t>(std::min<size_t>(m.calcCount + 1u, rows.size())); m.calcIndex = 0; m.lastNotice[0] = 0;
    return true;
}
bool Runtime::addRate(const char* heard) {
    auto& m = model;
    const char* codes[8]; const unsigned count = spokenCurrencies(heard, codes, std::size(codes));
    if (!count) {
        size_t bytes = 0; while (heard[bytes] && bytes < 60) bytes += voice::charBytes(static_cast<unsigned char>(heard[bytes]));
        snprintf(m.lastNotice, sizeof(m.lastNotice), *heard ? "没听出货币：%.*s" : "没有听清，请再说一次", int(bytes), heard); return false;
    }
    RateBoard board; if (!SettingsStore::loadRates(board)) board = {};
    char added[96] = ""; unsigned used = 0, fresh = 0;
    for (auto& t : board.targets) used += *t != 0;
    for (unsigned i = 0; i < count; ++i) {
        const char* code = codes[i]; int at = -1;
        for (unsigned k = 0; k < RateBoard::kTargets; ++k) if (!strcmp(board.targets[k], code)) at = static_cast<int>(k);
        if (!strcmp(board.base, code)) { snprintf(m.lastNotice, sizeof(m.lastNotice), "%s是计价货币，不用添加", currencyName(code)); continue; }
        if (at >= 0) { m.rateIndex = static_cast<uint8_t>(at); snprintf(m.lastNotice, sizeof(m.lastNotice), "已经有%s了", currencyName(code)); continue; }
        if (used == RateBoard::kTargets) { strcpy(m.lastNotice, "最多 6 种，先长按下键删掉一种"); break; }
        strcpy(board.targets[used], code); m.rateIndex = static_cast<uint8_t>(used++); ++fresh;
        if (strlen(added) + strlen(currencyName(code)) + 4 < sizeof(added)) { if (*added) strcat(added, "、"); strcat(added, currencyName(code)); }
    }
    if (!fresh) return false;
    if (!SettingsStore::saveRates(board)) { strcpy(m.lastNotice, "保存失败"); return false; }
    snprintf(m.lastNotice, sizeof(m.lastNotice), "已添加%s，正在更新", added);
    m.rateArmed = false;
    return true;
}
bool Runtime::addAlarm(const char* heard) {
    auto& m = model; auto& list = m.alarms;
    if (!*heard) { strcpy(m.lastNotice, "没有听清，请再说一次"); return false; }
    if (!m.clockValid) { strcpy(m.lastNotice, "时间未同步，暂不能设闹钟"); return false; }
    const auto parsed = parseAlarm(heard, time(nullptr));
    if (!parsed.ok) { strcpy(m.lastNotice, parsed.error); return false; }
    // The same time again just takes the new repeat.
    int at = -1;
    for (unsigned i = 0; i < list.count; ++i) if (list.items[i].hour == parsed.hour && list.items[i].minute == parsed.minute) at = static_cast<int>(i);
    if (at < 0 && list.count == AlarmList::kMax) { strcpy(m.lastNotice, "最多 5 个闹钟，先长按下键删一个"); return false; }
    if (at < 0) at = list.count++;
    auto& a = list.items[at]; a = {}; a.hour = parsed.hour; a.minute = parsed.minute; a.mode = parsed.mode;
    m.alarmIndex = static_cast<uint8_t>(at + 1); m.alarmArmed = m.alarmEdit = false;
    if (!SettingsStore::saveAlarms(list)) { strcpy(m.lastNotice, "保存失败"); return false; }
    static const char* const modes[]{"", "每天", "工作日", ""};
    if (parsed.mode == 3) { char when[32]; todoWhen(nextAlarm(a, time(nullptr)), time(nullptr), when, sizeof(when)); snprintf(m.lastNotice, sizeof(m.lastNotice), "已设好 · %s 响一次", when); }
    else snprintf(m.lastNotice, sizeof(m.lastNotice), "已设好 · %s %02u:%02u", modes[parsed.mode], a.hour, a.minute);
    return true;
}
bool Runtime::addCountdown(const char* heard) {
    auto& m = model; auto& board = m.countdown;
    if (!*heard) { strcpy(m.lastNotice, "没有听清，请再说一次"); return false; }
    if (!m.clockValid) { strcpy(m.lastNotice, "时间未同步，暂不能添加"); return false; }
    const auto parsed = parseCountdown(heard, today());
    if (!parsed.ok) { strcpy(m.lastNotice, parsed.error); return false; }
    // The same name again replaces its date; otherwise the first free place.
    int slot = -1;
    for (unsigned i = 0; i < CountdownBoard::kEvents; ++i) if (!strcmp(board.events[i].name, parsed.event.name)) slot = static_cast<int>(i);
    for (unsigned i = 0; i < CountdownBoard::kEvents && slot < 0; ++i) if (!*board.events[i].name) slot = static_cast<int>(i);
    if (slot < 0) { strcpy(m.lastNotice, "最多 6 个，先长按下键删一个"); return false; }
    board.events[slot] = parsed.event;
    if (!SettingsStore::saveCountdown(board)) { strcpy(m.lastNotice, "保存失败"); return false; }
    // Show it.
    CountdownItem items[20]; int minutes = 0; const Date now = today(&minutes);
    const unsigned count = m.countdownItems(now, minutes, items, std::size(items));
    for (unsigned i = 0; i < count; ++i) if (items[i].name == board.events[slot].name) m.countdownIndex = static_cast<uint8_t>(i);
    m.countdownArmed = false;
    // The card shows its date; the notice only names it.
    snprintf(m.lastNotice, sizeof(m.lastNotice), parsed.cut ? "已添加「%s」，名称只留 8 个字" : "已添加「%s」", board.events[slot].name);
    return true;
}
void Runtime::reloadStatus() {
    SettingsStore::loadStatus(model.status);
    if (model.status.index >= model.statusCount()) model.status.index = 0;
}
bool Runtime::addStatus(const char* heard) {
    auto& b = model.status;
    while (*heard == ' ') ++heard;
    // A sign needs no closing punctuation: the cloud ends most sentences with one.
    size_t n = strlen(heard);
    for (bool cut = true; cut && n;) {
        cut = false;
        if (strchr(" .,!?", heard[n - 1])) { --n; cut = true; continue; }
        for (const char* mark : {"。", "，", "、", "！", "？", "～"})
            if (n >= 3 && !memcmp(heard + n - 3, mark, 3)) { n -= 3; cut = true; break; }
    }
    if (!n) { strcpy(model.lastNotice, "没有听清，请再说一次"); return false; }
    // The card holds three lines of four 48 px characters.
    unsigned chars = 0;
    for (size_t i = 0; i < n; ++i) chars += (heard[i] & 0xC0) != 0x80;
    if (chars > 12 || n >= sizeof(b.custom[0])) { strcpy(model.lastNotice, "太长了，请说 12 个字以内"); return false; }
    // Saying a label again brings it to the front instead of keeping two.
    unsigned last = StatusBoard::kCustom - 1;
    for (unsigned i = 0; i < StatusBoard::kCustom; ++i)
        if (strlen(b.custom[i]) == n && !memcmp(b.custom[i], heard, n)) { last = i; break; }
    memmove(b.custom[1], b.custom[0], last * sizeof(b.custom[0]));
    memcpy(b.custom[0], heard, n); b.custom[0][n] = 0;
    b.index = 0; model.lastNotice[0] = 0;
    if (!SettingsStore::saveStatus(b)) strcpy(model.lastNotice, "保存失败");
    return true;
}
Result Runtime::switchTo(App target, bool failAlloc, bool failStart) {
    if (model.todoRinging && target != App::Todo) endTodoRing(TodoRing::Stop);
    if (target >= App::Count || (static_cast<unsigned>(target)==6||static_cast<unsigned>(target)==8)) return lastResult = Result::Invalid;
    if (target == active_ && !quarantined_) return lastResult = Result::Ok;
    ++generation_; // Invalidate every old callback before requesting quiescence.
    if (!worker_.stop()) {
        quarantined_ = true;
        return lastResult = Result::StopTimeout; // Never free an unacknowledged owner.
    }
    quarantined_ = false;
    if (active_ == App::Focus && model.focusRunning) model.action(App::Focus, now_);
    if (model.memoMounted) { MemoStore::unmount(); model.memoMounted = false; }
    model.lastNotice[0] = 0;
    if (workspace_) heap_caps_free(workspace_);
    workspace_ = nullptr; workspaceBytes_ = 0;
    active_ = App::Home;
    // Live adapters allocate their real codec/I/O buffers themselves. Synthetic
    // workspaces are retained only by the separate architecture fault tests.
    const auto wanted = model.live ? 0U : profile(target).workspace;
    constexpr unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    if (wanted && (failAlloc || heap_caps_get_free_size(caps) < wanted + kReserve
          || heap_caps_get_largest_free_block(caps) < wanted)) return lastResult = Result::LowMemory;
    if (wanted) {
        workspace_ = heap_caps_malloc(wanted, caps);
        if (!workspace_) return lastResult = Result::LowMemory;
        workspaceBytes_ = wanted;
        memset(workspace_, 0, wanted);
    }
    // A failure after bind may still leave the worker using the arena. Quiesce first.
    if (!worker_.bind(target, generation_, workspace_, wanted)) {
        quarantined_ = true;
        return lastResult = Result::StartFailed;
    }
    if (failStart) {
        if (!worker_.stop()) { quarantined_ = true; return lastResult = Result::StopTimeout; }
        if (workspace_) heap_caps_free(workspace_);
        workspace_ = nullptr; workspaceBytes_ = 0;
        return lastResult = Result::StartFailed;
    }
    active_ = target;
    ++switches;
    // The menu shows the page last opened, however it was opened (说功能名跳转 too): Back lands on its icon.
    if (const auto at = std::find(kFunctions.begin(), kFunctions.end(), target); at != kFunctions.end()) selection_ = static_cast<uint8_t>(at - kFunctions.begin());
    if (target == App::Words) {
        // A deck that fails to load or no longer matches the list starts over.
        if (!SettingsStore::loadWords(model.words) || model.words.next > kWordCount) model.words = {};
        wordsBegin(model.words, model.wordSession, wordDay(time(nullptr)), kWordCount);
        SettingsStore::saveWords(model.words);
    }
    // Each visit to the PPT page is a new talk: the timer starts at 00:00 and ends when the page closes.
    if (target == App::Slides) { model.slidesStarted = now_; model.slidesPressed = false; }
    // Like the cloud session, the chat record starts empty on every visit.
    if (target == App::Alert) model.alertChanged = now_;
    if (target == App::Status) reloadStatus();
    if (target == App::Todo) {
        auto& m = model; SettingsStore::loadTodo(m.page.todo); m.page.todo.sort();
        m.todoIndex = m.todoTop = 0; m.todoArmed = false; m.listenAt = 0;
    }
    if (target == App::Ledger) {
        auto& m = model; editLedger(m.page.ledger, [](LedgerBook&) { return false; });
        m.ledgerIndex = m.ledgerTop = 0; m.ledgerArmed = false; m.listenAt = 0;
    }
    if (target == App::Calc) { model.calcCount = model.calcIndex = 0; model.listenAt = 0; }
    if (target == App::Voice) model.listenAt = 0;
    if (target == App::Walkie) { auto& m = model; m.walkieTalk = m.walkieAuto = false; m.walkiePhase = 0; m.walkieFrom[0] = 0; m.walkieAt = now_; }
    if (target == App::Guard) {
        auto& m = model; SettingsStore::loadGuard(m.guard);
        m.guardRow = m.guardPhase = m.guardLeft = 0; m.guardRssi = m.guardLine = m.guardBase = 0;
    }
    if (target == App::Translate) { auto& t = model.page.translation; t.source[0] = t.result[0] = 0; t.engine = 0; t.at = 0; model.translateScroll = 0; model.listenAt = 0; }
    if (target == App::Wheel) {
        auto& m = model; SettingsStore::loadWheel(m.page.wheel);
        m.wheelSpunAt = 0; m.wheelPick = 255; m.wheelArmed = false; m.listenAt = 0;
    }
    if (target == App::Countdown) { reloadCountdown(); model.countdownArmed = false; model.listenAt = 0; }
    if (target == App::Alarm) { auto& m = model; m.alarmIndex = 0; m.alarmEdit = m.alarmArmed = false; m.listenAt = 0; }
    if (target == App::Qr) { model.qrBright = false; reloadQr(); }
    if (target == App::Torch) model.torchChanged = now_;
    if (target == App::News) { model.newsCount = model.newsIndex = model.newsScroll = 0; model.newsPage = model.newsWant = 1; model.page.news.text[0] = 0; model.page.news.textFor = 255; }
    if (target == App::Rate) { model.rateIndex = 0; model.rateArmed = false; model.listenAt = 0; }
    if (target == App::Noise) { model.noisePlaying = false; model.noiseUntil = 0; model.noiseRow = model.noiseKind; }
    if (target == App::Stock) { auto& m = model; m.stockCount = m.stockIndex = m.chartCount = 0; m.chartFor = 255; m.stockDetail = m.stockDays = m.stockArmed = false; m.listenAt = 0; }
    if (target == App::Forecast) { model.page.forecast.days = model.page.forecast.hours = model.page.forecast.airHours = model.page.forecast.warnings = 0; model.page.forecast.detailFor = 0xff; model.forecastView = model.forecastWarn = 0; }
    if (target == App::History) model.eventCount = model.eventIndex = 0;
    if (target == App::Daily) { model.page.daily.en[0] = model.page.daily.cn[0] = 0; model.dailyBack = model.dailyWant = 0; }
    if (target == App::Quake) model.quakeIndex = 0;
    if (target == App::Hacker) model.storyIndex = 0;
    if (target == App::Typhoon) { model.typhoonIndex = 0; model.page.typhoon.count = 0; model.page.typhoon.shown = 255; }
    if (target == App::Memo) {
        auto& m = model; m.memoIndex = m.memoTop = 0; m.memoDetail = m.memoArmed = false; m.memoScroll = 0; m.listenAt = m.memoSavedAt = 0;
        m.memoMounted = MemoStore::mount(); if (!m.memoMounted) strcpy(m.lastNotice, "笔记存储无法打开");
        reloadMemo();
    }
    if (target == App::Hot) { auto& m = model; m.hotCount = m.hotIndex = 0; m.hotDetail = false; m.hotUpdated = 0; m.page.hot.detailFor = 255; }
    if (target == App::Quiz) { model.quizCount = 0; model.quizSerial = 0; model.quizShown = model.quizWant = false; model.quizRight = model.quizAsked = 0; }
    if (target == App::Wifi || target == App::Sound || target == App::Motion) { if (!SettingsStore::loadTools(model.tools)) model.tools = {}; }
    if (target == App::Wifi) { auto& m = model; m.wifiTab = m.wifiIndex = 0; m.wifiApCount = m.pingCount = m.lanCount = 0; m.wifiBusy = false; m.wifiNote[0] = 0; m.wakeSentAt = 0; }
    if (target == App::Motion) { auto& m = model; m.motionPhase = m.motionLevel = m.motionCalLeft = m.motionRate = m.motionHistoryCount = 0; m.motionRssi = 0; m.motionEvents = 0; m.motionLastAt = 0; }
    if (target == App::Sound) { model.soundView = 0; model.soundRunning = false; model.soundBars = {}; model.soundDb = model.soundMax = 0; model.soundHz = 0; model.soundFailed = false; }
    if (target == App::Calendar) {
        const Date d = today(); model.calendarView = 0;
        if (d.year > 2000) { model.calendarYear = d.year; model.calendarMonth = d.month; model.calendarDay = d.day; }
    }
    if (target == App::Podcast) model.podcastPlayer = false;
    if (target == App::Radio || target == App::Podcast) { model.mediaTimer = 0; model.mediaUntil = 0; }
    if (target == App::Chat) { model.chatCount = 0; model.chatTurn = 0; model.chatSentence[0] = 0; ++model.chatRevision; }
    return lastResult = Result::Ok;
}
void Runtime::key(Key key, uint64_t now) {
    tick(now);
    lastInput_ = now;screenOff_ = false; model.confirmTaken = false;
    if(active_==App::Settings){settingsKey(key);return;}
    if (key == Key::Back) {
        // The podcast player goes back to its program list first.
        if (active_ == App::Podcast && model.podcastPlayer) { model.podcastPlayer = false; return; }
        // So does a stock's detail; a ringing alarm stops first.
        if (active_ == App::Stock && model.stockDetail) { model.stockDetail = false; return; }
        if (active_ == App::Hot && model.hotDetail) { model.hotDetail = false; return; }
        if (active_ == App::Memo && model.memoDetail && !model.voiceBusy()) { model.memoDetail = model.memoArmed = false; return; }
        if (active_ == App::Alarm && model.alarmRinging) { model.alarmRinging = false; return; }
        if (active_ == App::Alarm && model.alarmEdit && !model.voiceBusy()) { model.alarmEdit = false; return; }
        if (active_ == App::Todo && model.todoRinging) { endTodoRing(TodoRing::Stop); return; }
        if (active_ == App::Home) { screenOff_ = true; return; }
        // A call that opened 对讲机 goes back to the page it interrupted.
        if (active_ == App::Walkie && model.walkieAuto) { switchTo(model.walkieBack); return; }
        switchTo(active_ == App::Menu || active_ == App::Settings || active_ == App::Voice ? App::Home :
                 active_ == App::Home ? App::Home : App::Menu);
        return;
    }
    if (key == Key::OpenSettings && (active_ == App::Home || active_ == App::Menu)) {
        switchTo(App::Settings); return;
    }
    if (active_ == App::Home) {
        if (key == Key::Confirm) switchTo(App::Menu);
        if (key == Key::Up || key == Key::Down) {
            constexpr unsigned count = static_cast<unsigned>(HomeScreen::Count);
            model.homeScreen = static_cast<HomeScreen>((static_cast<unsigned>(model.homeScreen) + (key == Key::Up ? count - 1 : 1)) % count);
            SettingsStore::saveByte("home_screen", static_cast<uint8_t>(model.homeScreen));
        }
    } else if (active_ == App::Menu) {
        if (key == Key::Up) selection_ = (selection_ + kFunctions.size() - 1) % kFunctions.size();
        if (key == Key::Down) selection_ = (selection_ + 1) % kFunctions.size();
        if (key == Key::Confirm) switchTo(kFunctions[selection_]);
    } else if (active_ == App::Words) {
        auto& session = model.wordSession;
        if (key == Key::Up || key == Key::Down) wordsMove(session, key == Key::Up ? -1 : 1);
        // Confirm shows the meaning and reads the word aloud (LiveServices); once the day is done it adds ten more.
        if (key == Key::Confirm && session.size) session.revealed = true;
        else if (key == Key::Confirm) { wordsMore(model.words, session, kWordCount); SettingsStore::saveWords(model.words); }
    } else if (active_ == App::Slides) {
        if (key == Key::Up || key == Key::Down || key == Key::Confirm)
            slideKey(key == Key::Up ? SlideKey::Previous : key == Key::Down ? SlideKey::Next : SlideKey::Start, now);
    } else if (active_ == App::Status) {
        // Confirm speaks a new label (LiveServices); a long Up/Down switches 常亮 (longKey).
        auto& board = model.status;const unsigned count = model.statusCount();
        if (model.voiceBusy() || (key != Key::Up && key != Key::Down)) return;
        model.lastNotice[0] = 0;
        board.index = (board.index + (key == Key::Up ? count - 1 : 1)) % count;
        if (!SettingsStore::saveStatus(board)) strcpy(model.lastNotice, "保存失败");
    } else if (active_ == App::Todo) {
        // Ringing: Confirm marks it done, Up/Down ask again in 10 minutes (long Confirm stops it, in Back above).
        // Otherwise Up/Down move through the 录音 row and the todos, Confirm ticks a todo off or back on (on the
        // 录音 row LiveServices starts listening).
        auto& m = model; auto& list = m.page.todo;
        if (m.todoRinging) { if (key == Key::Confirm) endTodoRing(TodoRing::Done); else if (key == Key::Up || key == Key::Down) endTodoRing(TodoRing::Snooze); m.confirmTaken = true; return; }
        if (m.voiceBusy()) return;
        m.todoArmed = false;
        const unsigned rows = list.count + 1u;
        if (key == Key::Up || key == Key::Down) {
            m.lastNotice[0] = 0;
            m.todoIndex = static_cast<uint8_t>((m.todoIndex + (key == Key::Up ? rows - 1 : 1)) % rows);
            if (m.todoIndex) selectTodo(m, list.items[m.todoIndex - 1].at, list.items[m.todoIndex - 1].text); else m.todoTop = 0;
        }
        if (key == Key::Confirm && m.todoIndex) {
            m.lastNotice[0] = 0;
            TodoItem item = list.items[m.todoIndex - 1]; list.items[m.todoIndex - 1].flags ^= TodoList::kDone;
            list.sort(); selectTodo(m, item.at, item.text);
            if (!SettingsStore::saveTodo(list)) strcpy(m.lastNotice, "保存失败");
            reloadTodo();
        }
    } else if (active_ == App::Ledger) {
        // Up/Down move through the 录音 row and today's entries, newest first; Confirm on an entry steps its category
        // (收入 last; on the 录音 row LiveServices starts listening).
        auto& m = model;
        if (m.voiceBusy()) return;
        m.ledgerArmed = false;
        uint8_t order[LedgerBook::kMax];
        const unsigned rows = m.page.ledger.shown(ledgerToday(), order) + 1u;
        if (key == Key::Up || key == Key::Down) {
            m.lastNotice[0] = 0;
            m.ledgerIndex = static_cast<uint8_t>((m.ledgerIndex + (key == Key::Up ? rows - 1 : 1)) % rows);
            ledgerScroll(m);
        }
        if (key == Key::Confirm && m.ledgerIndex && m.ledgerIndex < rows) {
            const uint32_t id = m.page.ledger.items[order[m.ledgerIndex - 1]].id; uint8_t category = 0;
            const bool ok = editLedger(m.page.ledger, [&](LedgerBook& b) {
                const int i = b.find(id); if (i < 0) return false;
                b.items[i].category = static_cast<uint8_t>((b.items[i].category + 1) % kLedgerCount); category = b.items[i].category;
                b.touch(static_cast<unsigned>(i)); return true; });
            if (ok) snprintf(m.lastNotice, sizeof(m.lastNotice), "分类改为 %s", kLedgerCategories[category]); else strcpy(m.lastNotice, "保存失败");
        }
    } else if (active_ == App::Walkie) {
        // Any key keeps the page open after a call. Up/Down set the volume (main hands it to the codec), long Up/Down
        // the channel (longKey); Confirm starts and ends talking (the device's task sends meanwhile), but not over
        // someone already talking.
        auto& m = model; m.walkieAuto = false; m.lastNotice[0] = 0;
        if (key == Key::Up || key == Key::Down) m.volume = static_cast<uint8_t>(std::clamp(int(m.volume) + (key == Key::Up ? 10 : -10), 0, 100));
        if (key == Key::Confirm) {
            if (m.walkiePhase == 2 && !m.walkieTalk) { snprintf(m.lastNotice, sizeof(m.lastNotice), "%s 正在说话，请稍候", *m.walkieFrom ? m.walkieFrom : "对方"); return; }
            m.walkieTalk = !m.walkieTalk; m.walkieAt = now;
        }
    } else if (active_ == App::Calc) {
        // Up/Down step through this visit's sums (Up newer, Down older); Confirm speaks the next one (LiveServices).
        auto& m = model;
        if (m.voiceBusy() || !m.calcCount || (key != Key::Up && key != Key::Down)) return;
        m.lastNotice[0] = 0;
        m.calcIndex = static_cast<uint8_t>((m.calcIndex + (key == Key::Up ? m.calcCount - 1 : 1)) % m.calcCount);
    } else if (active_ == App::Guard) {
        // Up/Down pick a row, Confirm steps its value (saved; LiveServices sends it to the computer).
        auto& m = model; auto& g = m.guard;
        if (key == Key::Up) m.guardRow = (m.guardRow + 3) % 4;
        if (key == Key::Down) m.guardRow = (m.guardRow + 1) % 4;
        if (key == Key::Confirm) {
            if (m.guardRow == 0) g.distance = (g.distance + 1) % 3;
            else if (m.guardRow == 1) g.delay = (g.delay + 1) % 4;
            else if (m.guardRow == 2) g.keep = (g.keep + 1) % 3;
            else g.wake = !g.wake;
            if (!SettingsStore::saveGuard(g)) strcpy(m.lastNotice, "保存失败");
        }
    } else if (active_ == App::Translate) {
        // Up/Down scroll a long translation; Confirm speaks (LiveServices), a long Up reads the translation aloud.
        auto& m = model;
        if (m.voiceBusy()) return;
        if (key == Key::Up && m.translateScroll) --m.translateScroll;
        if (key == Key::Down && m.translateScroll + Model::kTranslateLines < uiWrappedLines(m.page.translation.result)) ++m.translateScroll;
    } else if (active_ == App::Wheel) {
        // Up/Down switch the mode, Confirm spins (while speaking, LiveServices ends it instead); keys wait out a spin.
        auto& m = model; auto& w = m.page.wheel;
        if (m.voiceBusy() || m.wheelSpinning() || m.livePhase != LivePhase::Ready) return;
        m.wheelArmed = false;
        if ((key == Key::Up || key == Key::Down) && w.count) {
            w.current = (w.current + (key == Key::Up ? w.count - 1 : 1)) % w.count; m.wheelPick = 255; m.lastNotice[0] = 0;
            SettingsStore::saveWheel(w);
        }
        if (key == Key::Confirm) spinWheel();
    } else if (active_ == App::Torch) {
        auto& m = model;
        if (key == Key::Up || key == Key::Down) m.torchLevel = std::clamp(m.torchLevel + (key == Key::Up ? 1 : -1), 1, 5);
        if (key == Key::Confirm) m.torchColor = (m.torchColor + 1) % std::size(kTorchColors);
        m.torchChanged = now;
    } else if (active_ == App::Qr) {
        const unsigned count = qrCount();
        if (count && (key == Key::Up || key == Key::Down)) { model.qrIndex = (model.qrIndex + (key == Key::Up ? count - 1 : 1)) % count; reloadQr(); }
        if (key == Key::Confirm) model.qrBright = !model.qrBright;
    } else if (active_ == App::Calendar) {
        // Up/Down step one day (across months); Confirm steps 月历 → 黄历 → 月相节气 → 月历.
        auto& m = model;
        if (key == Key::Up || key == Key::Down) {
            const Date d = fromDayNumber(dayNumber({m.calendarYear, m.calendarMonth, m.calendarDay}) + (key == Key::Up ? -1 : 1));
            if (d.year > kLunarFirstYear && d.year < kLunarLastYear) { m.calendarYear = d.year; m.calendarMonth = d.month; m.calendarDay = d.day; }
        }
        if (key == Key::Confirm) m.calendarView = (m.calendarView + 1) % 3;
    } else if (active_ == App::Countdown) {
        // Up/Down switch, Confirm shows the list (while speaking, LiveServices ends it instead).
        if (model.voiceBusy()) return;
        CountdownItem items[20]; int minutes = 0; const Date now = today(&minutes);
        const unsigned count = model.countdownItems(now, minutes, items, std::size(items));
        model.countdownArmed = false; model.lastNotice[0] = 0;
        if (count && (key == Key::Up || key == Key::Down)) model.countdownIndex = (model.countdownIndex + (key == Key::Up ? count - 1 : 1)) % count;
        if (key == Key::Confirm) model.countdownList = !model.countdownList;
    } else if (active_ == App::News) {
        // Up/Down scroll the headline's text a screen at a time (a line kept), then step to the next headline.
        auto& m = model; const auto& n = m.page.news;
        const unsigned shown = m.newsScroll ? Model::kNewsMoreLines : Model::kNewsLines;
        if (key == Key::Down && m.newsCount) {
            if (n.textFor == m.newsIndex && m.newsScroll + shown < uiTextLines(n.text, 200)) m.newsScroll += shown - 1;
            else if (m.newsIndex + 1 < m.newsCount) { ++m.newsIndex; m.newsScroll = 0; }
            else if (m.newsPage < 20) { m.newsWant = m.newsPage + 1; m.newsFromEnd = false; }
        }
        if (key == Key::Up && m.newsCount) {
            if (m.newsScroll) m.newsScroll = m.newsScroll >= Model::kNewsLines - 1 + Model::kNewsMoreLines - 1 ? m.newsScroll - (Model::kNewsMoreLines - 1) : 0;
            else if (m.newsIndex) --m.newsIndex;
            else if (m.newsPage > 1) { m.newsWant = m.newsPage - 1; m.newsFromEnd = true; }
        }
    } else if (active_ == App::Stopwatch) {
        auto& m = model; const uint64_t us = esp_timer_get_time();
        if (key == Key::Confirm) { if (m.swRunning) m.swBase += us - m.swStarted; else m.swStarted = us; m.swRunning = !m.swRunning; }
        if (key == Key::Up && m.swRunning) { std::move_backward(m.swLaps.begin(), m.swLaps.end() - 1, m.swLaps.end()); m.swLaps[0] = m.stopwatchUs(); ++m.swLapCount; }
        if (key == Key::Down && !m.swRunning) { m.swBase = 0; m.swLapCount = 0; m.swLaps = {}; }
    } else if (active_ == App::History) {
        if (model.eventCount && (key == Key::Up || key == Key::Down)) model.eventIndex = (model.eventIndex + (key == Key::Up ? model.eventCount - 1 : 1)) % model.eventCount;
    } else if (active_ == App::Daily) {
        // Up goes a day back, Down a day forward (not past today); LiveServices fetches dailyWant.
        if (key == Key::Up && *model.page.daily.en) model.dailyWant = std::min(model.dailyBack + 1, 365);
        if (key == Key::Down && model.dailyBack) model.dailyWant = model.dailyBack - 1;
    } else if (active_ == App::Quake || active_ == App::Hacker) {
        auto& m = model; const bool quake = active_ == App::Quake;
        const unsigned count = quake ? m.quakeCount : m.storyCount; auto& index = quake ? m.quakeIndex : m.storyIndex;
        if (count && (key == Key::Up || key == Key::Down)) index = (index + (key == Key::Up ? count - 1 : 1)) % count;
    } else if (active_ == App::Quiz) {
        // Up/Down pick an answer; Confirm reveals it, and once revealed asks for the next question.
        auto& m = model;
        if (m.quizCount && !m.quizShown && (key == Key::Up || key == Key::Down)) m.quizPick = (m.quizPick + (key == Key::Up ? m.quizCount - 1 : 1)) % m.quizCount;
        if (key == Key::Confirm && m.quizCount) {
            if (m.quizShown) m.quizWant = true;
            else { m.quizShown = true; ++m.quizAsked; m.quizRight += m.quizPick == m.quizCorrect; }
        }
    } else if (active_ == App::Wifi) {
        auto& m = model; const unsigned count = m.wifiTab == 0 ? m.wifiApCount : m.wifiTab == 2 ? m.lanCount : 0;
        if (count && (key == Key::Up || key == Key::Down)) m.wifiIndex = (m.wifiIndex + (key == Key::Up ? count - 1 : 1)) % count;
        // On 设备 Confirm makes the chosen device the wake target and opens 唤醒; elsewhere it reruns the tab.
        if (key == Key::Confirm && m.wifiTab == 2 && m.lanCount) {
            memcpy(m.tools.wakeMac, m.page.wifi.lan[m.wifiIndex % m.lanCount].mac, 6); m.tools.wakeSet = true;
            if (!SettingsStore::saveTools(m.tools)) strcpy(m.lastNotice, "保存失败");
            m.wifiTab = 3; m.wifiIndex = 0;
        } else if (key == Key::Confirm && m.wifiTab != 2) ++m.wifiAction;
    } else if (active_ == App::Sound) {
        // Up/Down: dB calibration, the tuner's A4 or the tempo (all kept); Confirm: clear the maximum or start/stop the metronome.
        auto& m = model; auto& t = m.tools; const int step = key == Key::Up ? 1 : key == Key::Down ? -1 : 0;
        if (step) {
            if (m.soundView == 0) t.soundCal = std::clamp(t.soundCal + step, -40, 40);
            else if (m.soundView == 1) t.a4 = std::clamp(t.a4 + step, 430, 450);
            else t.bpm = std::clamp(t.bpm + step * 5, 30, 240);
            if (!SettingsStore::saveTools(t)) strcpy(m.lastNotice, "保存失败");
        }
        if (key == Key::Confirm && m.soundView == 0) m.soundMax = 0;
        if (key == Key::Confirm && m.soundView == 2) m.soundRunning = !m.soundRunning;
    } else if (active_ == App::Motion) {
        // Up/Down: sensitivity (kept); Confirm: take a new quiet baseline.
        auto& t = model.tools; const int step = key == Key::Up ? 1 : key == Key::Down ? -1 : 0;
        if (step) { t.motionSense = static_cast<uint8_t>(std::clamp(t.sense() + step, 1, 9)); if (!SettingsStore::saveTools(t)) strcpy(model.lastNotice, "保存失败"); }
        if (key == Key::Confirm) ++model.motionCalibrate;
    } else if (active_ == App::Noise) {
        // Up/Down pick a row; Confirm plays or stops that sound, or on the last row steps the sleep timer.
        auto& m = model; constexpr unsigned rows = std::size(kNoiseSounds) + 1;
        if (key == Key::Up || key == Key::Down) m.noiseRow = (m.noiseRow + (key == Key::Up ? rows - 1 : 1)) % rows;
        if (key == Key::Confirm && m.noiseRow == rows - 1) {
            m.noiseTimer = nextSleepTimer(m.noiseTimer);
        } else if (key == Key::Confirm) {
            m.noisePlaying = !(m.noisePlaying && m.noiseKind == m.noiseRow); m.noiseKind = m.noiseRow;
        }
        if (key == Key::Confirm) m.noiseUntil = m.noisePlaying && m.noiseTimer ? now + m.noiseTimer * 60u : 0;
    } else if (active_ == App::Alarm) {
        // Ringing: Up/Down snooze 5 minutes, Confirm stops. An alarm's clock: Up/Down move it 5 minutes, Confirm steps
        // the repeat (off, every day, workdays, once). The list: Up/Down pick the 录音 row or an alarm, Confirm opens the
        // alarm (on the 录音 row LiveServices starts listening), or first cancels a snooze.
        auto& m = model; auto& list = m.alarms;
        if (m.alarmRinging) {
            m.alarmRinging = false; m.confirmTaken = true;
            if (key == Key::Up || key == Key::Down) m.alarmSnooze = static_cast<uint32_t>(time(nullptr) + 300);
        } else if (m.voiceBusy()) return;
        else if (key == Key::Confirm && m.alarmSnooze) { m.alarmSnooze = 0; m.confirmTaken = true; strcpy(m.lastNotice, "已取消贪睡"); }
        else if (m.alarmEdit && m.alarmIndex && m.alarmIndex <= list.count) {
            auto& a = list.items[m.alarmIndex - 1];
            if (key == Key::Up || key == Key::Down) { const int minutes = (a.hour * 60 + a.minute / 5 * 5 + (key == Key::Up ? 5 : -5) + 1440) % 1440; a.hour = minutes / 60; a.minute = minutes % 60; }
            if (key == Key::Confirm) a.mode = (a.mode + 1) % 4;
            if (!SettingsStore::saveAlarms(list)) strcpy(m.lastNotice, "保存失败");
        } else {
            m.alarmEdit = m.alarmArmed = false;
            const unsigned rows = list.count + 1u;
            if (key == Key::Up || key == Key::Down) { m.lastNotice[0] = 0; m.alarmIndex = static_cast<uint8_t>((m.alarmIndex + (key == Key::Up ? rows - 1 : 1)) % rows); }
            if (key == Key::Confirm && m.alarmIndex) { m.alarmEdit = true; m.lastNotice[0] = 0; }
        }
    } else if (active_ == App::Stock) {
        // The list: Up/Down pick, Confirm opens the detail. The detail: Up/Down step through the list, Confirm
        // switches the chart between today's minutes and 30 days. LiveServices fetches what is shown.
        auto& m = model;
        if (m.voiceBusy()) return;
        m.stockArmed = false; if (key != Key::Confirm || !m.stockDetail) m.lastNotice[0] = 0;
        if (m.stockCount && (key == Key::Up || key == Key::Down)) m.stockIndex = (m.stockIndex + (key == Key::Up ? m.stockCount - 1 : 1)) % m.stockCount;
        if (key == Key::Confirm && m.stockDetail) m.stockDays = !m.stockDays;
        else if (key == Key::Confirm && m.stockCount) m.stockDetail = true;
    } else if (active_ == App::Typhoon) {
        // Up/Down pick a typhoon (LiveServices fetches its track); Confirm refreshes (LiveServices).
        auto& t = model.page.typhoon;
        if (t.count && (key == Key::Up || key == Key::Down)) model.typhoonIndex = (model.typhoonIndex + (key == Key::Up ? t.count - 1 : 1)) % t.count;
    } else if (active_ == App::Hot) {
        // Up/Down move through the board, in the list or the detail; Confirm opens or closes the detail.
        auto& m = model;
        if (m.hotCount && (key == Key::Up || key == Key::Down)) m.hotIndex = (m.hotIndex + (key == Key::Up ? m.hotCount - 1 : 1)) % m.hotCount;
        if (key == Key::Confirm && m.hotCount) m.hotDetail = !m.hotDetail;
    } else if (active_ == App::Memo) {
        // The list: Up/Down pick a row; Confirm on the 录音 row records (LiveServices), on a note opens it. An open
        // note: Up/Down scroll it, Confirm closes it. While recording the keys belong to LiveServices.
        auto& m = model;
        if (m.voiceBusy()) return;
        m.memoArmed = false; m.lastNotice[0] = 0;
        if (m.memoDetail) {
            const unsigned lines = uiTextLines(m.page.memo.note.text);
            if (key == Key::Up) m.memoScroll -= std::min<unsigned>(m.memoScroll, Model::kMemoLines - 1);
            if (key == Key::Down && m.memoScroll + Model::kMemoLines < lines) m.memoScroll += Model::kMemoLines - 1;
            if (key == Key::Confirm) m.memoDetail = false;
        } else {
            const unsigned rows = m.memoCount + 1;
            if (key == Key::Up || key == Key::Down) { m.memoIndex = (m.memoIndex + (key == Key::Up ? rows - 1 : 1)) % rows; reloadMemo(); }
            if (key == Key::Confirm && m.memoIndex) {
                if (MemoStore::load(m.memoIndex - 1, m.page.memo.note)) { m.memoDetail = true; m.memoScroll = 0; }
                else strcpy(m.lastNotice, "读取失败");
            }
        }
    } else if (active_ == App::Forecast) {
        // Up/Down switch tabs. On the 预警 tab Confirm opens each warning's text in turn, then the list again.
        auto& f = model.page.forecast;
        if (key == Key::Up || key == Key::Down) { model.forecastView = (model.forecastView + (key == Key::Up ? 3 : 1)) % 4; model.forecastWarn = 0; }
        if (key == Key::Confirm && model.forecastView == 3 && f.warnings && f.warnings != 0xff) model.forecastWarn = (model.forecastWarn + 1) % (f.warnings + 1);
    } else if (active_ == App::Rate) {
        // Up/Down pick; Confirm refreshes (LiveServices; while speaking it ends it instead).
        if (model.voiceBusy()) return;
        model.rateArmed = false; model.lastNotice[0] = 0;
        if (model.rateCount && (key == Key::Up || key == Key::Down)) model.rateIndex = (model.rateIndex + (key == Key::Up ? model.rateCount - 1 : 1)) % model.rateCount;
    } else if (active_ == App::Sonar) {
        auto& m = model; uint8_t order[Model::kSonarSlots]; const unsigned count = m.sonarVisible(order);
        if (count && (key == Key::Up || key == Key::Down)) {
            unsigned at = count;
            for (unsigned i = 0; i < count; ++i) if (!memcmp(m.sonar[order[i]].addr, m.sonarFocus, 6)) at = i;
            at = at == count ? 0 : (at + (key == Key::Up ? count - 1 : 1)) % count;
            memcpy(m.sonarFocus, m.sonar[order[at]].addr, 6);
        }
        if (key == Key::Confirm) m.sonarList = !m.sonarList;
    } else if (active_ == App::Podcast) {
        // The list: Up/Down pick a program, Confirm opens it (LiveServices asks the device to play it).
        // The player: Up/Down set the volume; Confirm plays or pauses (LiveServices).
        auto& m = model;
        if (!m.podcastPlayer) {
            if (m.podcastCount && (key == Key::Up || key == Key::Down)) m.podcastIndex = (m.podcastIndex + (key == Key::Up ? m.podcastCount - 1 : 1)) % m.podcastCount;
            if (key == Key::Confirm && m.podcastCount) m.podcastPlayer = true;
        } else if (key == Key::Up || key == Key::Down) m.volume = static_cast<uint8_t>(std::clamp(int(m.volume) + (key == Key::Up ? 5 : -5), 0, 100));
    } else if (active_ == App::Alert) {
        constexpr unsigned count = std::size(kAlertModes);
        if (key == Key::Up || key == Key::Down) { model.alertMode = (model.alertMode + (key == Key::Up ? count - 1 : 1)) % count; model.alertChanged = now; }
    } else {
        if (key == Key::Confirm) model.action(active_, now);
        if(active_==App::Focus&&(key==Key::Up||key==Key::Down))model.selectFocus(key==Key::Up?-1:1);
        if((active_==App::Chat||active_==App::Radio)&&(key==Key::Up||key==Key::Down))model.volume=static_cast<uint8_t>(std::clamp(int(model.volume)+(key==Key::Up?5:-5),0,100));
    }
}
// 收音机/播客: long Down steps the sleep timer (白噪音's choices); it counts from now, LiveServices stops the stream.
void Runtime::walkieCall(uint64_t now) {
    if (active_ == App::Walkie) return;
    const App back = active_;
    wake(now);
    if (switchTo(App::Walkie) != Result::Ok) return;
    model.walkieBack = back; model.walkieAuto = true;
}
void Runtime::stepMediaTimer(uint64_t now) {
    if (active_ != App::Radio && active_ != App::Podcast) return;
    tick(now); lastInput_ = now;
    model.mediaTimer = nextSleepTimer(model.mediaTimer);
    model.mediaUntil = model.mediaTimer ? now + model.mediaTimer * 60u : 0;
}
void Runtime::gradeWord(bool known, uint64_t now) {
    if (active_ != App::Words) return;
    tick(now); lastInput_ = now;
    wordsGrade(model.words, model.wordSession, known);
    if (!SettingsStore::saveWords(model.words)) strcpy(model.lastNotice, "进度保存失败");
}
void Runtime::slideKey(SlideKey key, uint64_t now) {
    if (active_ != App::Slides) return;
    tick(now); lastInput_ = now;
    auto& m = model;
    m.slidesLast = key; m.slidesPressed = true; m.slidesFailed = false;
}
void Runtime::reloadWifi() {
    auto& m = model; auto list = std::unique_ptr<WifiList>(new (std::nothrow) WifiList);
    if (list && !SettingsStore::loadWifi(*list)) *list = {};
    m.wifiSavedCount = list ? list->count : 0;
    for (unsigned i = 0; i < WifiList::kMax; ++i) strcpy(m.wifiSaved[i], i < m.wifiSavedCount ? list->items[i].ssid : "");
    if (m.wifiRow > m.wifiSavedCount) m.wifiRow = m.wifiSavedCount;
}
void Runtime::rememberWifi(const char* ssid, const char* password) {
    if (!*ssid || strlen(ssid) >= sizeof(WifiNetwork::ssid) || strlen(password) >= sizeof(WifiNetwork::password)) return;
    auto list = std::unique_ptr<WifiList>(new (std::nothrow) WifiList);
    if (!list) return;
    if (!SettingsStore::loadWifi(*list)) *list = {};
    if (list->remember(ssid, password) && !SettingsStore::saveWifi(*list)) strcpy(model.lastNotice, "WiFi 列表保存失败");
    if (strcmp(settings_->wifiSsid, ssid) || strcmp(settings_->wifiPassword, password)) {
        auto candidate = *settings_; snprintf(candidate.wifiSsid, sizeof(candidate.wifiSsid), "%s", ssid);
        snprintf(candidate.wifiPassword, sizeof(candidate.wifiPassword), "%s", password);
        SettingsStore store; if (store.save(candidate)) *settings_ = candidate;
    }
    reloadWifi();
}
void Runtime::forgetWifi() {
    auto& m = model;
    if (!m.wifiRow || m.wifiRow > m.wifiSavedCount) return;
    if (!m.wifiArmed) { m.wifiArmed = true; strcpy(m.lastNotice, "再长按下键删除这个 WiFi"); return; }
    m.wifiArmed = false;
    auto list = std::unique_ptr<WifiList>(new (std::nothrow) WifiList);
    if (!list || !SettingsStore::loadWifi(*list)) { strcpy(m.lastNotice, "删除失败"); return; }
    const int at = list->find(m.wifiSaved[m.wifiRow - 1]);
    if (at < 0) { reloadWifi(); return; }
    list->remove(at);
    if (!SettingsStore::saveWifi(*list)) { strcpy(m.lastNotice, "删除失败"); return; }
    // The one tried at boot becomes the next saved (or none), so a forgotten password is never used again.
    if (!strcmp(settings_->wifiSsid, m.wifiSaved[m.wifiRow - 1])) {
        auto candidate = *settings_; strcpy(candidate.wifiSsid, list->count ? list->items[0].ssid : "");
        strcpy(candidate.wifiPassword, list->count ? list->items[0].password : "");
        SettingsStore store; if (store.save(candidate)) *settings_ = candidate;
    }
    strcpy(m.lastNotice, "已删除");
    reloadWifi();
}
bool Runtime::settingsKey(Key key) {
    auto& page=model.settingsPage;
    if(key==Key::Back&&page==SettingsPage::Idle&&model.idleRow==2){model.idleRow=0;model.settingsValue=settings_->idleSeconds/60;return true;}
    if(key==Key::Back){if(page!=SettingsPage::List)page=SettingsPage::List;else switchTo(App::Menu);return true;}
    if(page==SettingsPage::List){
        if(key==Key::Up)model.settingsSelection=(model.settingsSelection+kSettingsRows-1)%kSettingsRows;
        if(key==Key::Down)model.settingsSelection=(model.settingsSelection+1)%kSettingsRows;
        if(key==Key::Confirm){
            page=static_cast<SettingsPage>(model.settingsSelection+1);
            if(page==SettingsPage::Wifi){model.wifiRow=0;model.wifiArmed=false;model.lastNotice[0]=0;reloadWifi();}
            if(page==SettingsPage::City){model.cityRow=0;model.lastNotice[0]=0;model.confirmTaken=true;}
            if(page==SettingsPage::Wake)model.wakeRow=0;
            if(page==SettingsPage::Idle)model.idleRow=0;
            model.settingsValue=page==SettingsPage::Brightness?settings_->brightness:page==SettingsPage::Volume?model.volume:settings_->idleSeconds/60;
        }
        return true;
    }
    // WiFi: Up/Down pick 添加 WiFi or a saved network (the device joins it, or starts the setup hotspot, on Confirm;
    // a long Down forgets it); while the hotspot is on the page shows the setup steps instead.
    if(page==SettingsPage::Wifi){
        model.wifiArmed=false;model.lastNotice[0]=0;
        const unsigned rows=model.wifiSavedCount+1u;
        if(!model.wifiSetup&&(key==Key::Up||key==Key::Down))model.wifiRow=(model.wifiRow+(key==Key::Up?rows-1:1))%rows;
        return true;
    }
    // 所在城市: Up/Down pick 语音输入 or 按 IP 定位; Confirm (LiveServices) has the device listen or look it up.
    if(page==SettingsPage::City){
        if(!model.voiceBusy()&&(key==Key::Up||key==Key::Down)){model.cityRow^=1;model.lastNotice[0]=0;}
        return true;
    }
    // 唤醒与对讲: Up/Down pick the wake word, 说功能名跳转, 打开 Mac 程序 or 后台接收对讲; Confirm switches the one picked and saves it.
    if(page==SettingsPage::Wake){
        if(key==Key::Up||key==Key::Down){model.wakeRow=(model.wakeRow+(key==Key::Down?1:3))%4;model.lastNotice[0]=0;return true;}
        if(key!=Key::Confirm)return true;
        bool& on=model.wakeRow==3?model.walkieListen:model.wakeRow==2?model.macOpen:model.wakeRow?model.voiceJump:model.wakeWord;
        const bool storedOff=model.wakeRow==1||model.wakeRow==2;  // these two are stored as "off", so they start on
        if(!SettingsStore::saveByte(model.wakeRow==3?"walkie_bg":model.wakeRow==2?"mac_off":model.wakeRow?"jump_off":"wake_word",storedOff?on:!on)){strcpy(model.lastNotice,"保存失败，请重试");return false;}
        on=!on;return true;
    }
    // 自动息屏: Up/Down pick the time or 连电脑时常亮; Confirm on the time starts changing it (Up/Down, Confirm saves,
    // long Confirm cancels), on the other switches it and saves.
    if(page==SettingsPage::Idle&&model.idleRow!=2){
        model.lastNotice[0]=0;
        if(key==Key::Up||key==Key::Down){model.idleRow^=1;return true;}
        if(key!=Key::Confirm)return true;
        if(!model.idleRow){model.idleRow=2;return true;}
        if(!SettingsStore::saveByte("usb_dim",model.usbAwake)){strcpy(model.lastNotice,"保存失败，请重试");return false;}
        model.usbAwake=!model.usbAwake;return true;
    }
    if(page==SettingsPage::Bluetooth){
        if(key!=Key::Confirm)return true;
        auto candidate=*settings_;candidate.bluetoothEnabled=!candidate.bluetoothEnabled;SettingsStore store;
        if(!store.save(candidate)){strcpy(model.lastNotice,"保存失败，请重试");return false;}
        *settings_=candidate;return true;
    }
    if(key==Key::Up||key==Key::Down){
        const int step=page==SettingsPage::Idle?1:page==SettingsPage::Volume?5:10;
        const int low=page==SettingsPage::Brightness?10:0,high=page==SettingsPage::Idle?1440:100;
        model.settingsValue=std::clamp(int(model.settingsValue)+(key==Key::Up?step:-step),low,high);return true;
    }
    if(key==Key::Confirm){
        if(page==SettingsPage::Volume)model.volume=model.settingsValue;
        else{
            auto candidate=*settings_;
            if(page==SettingsPage::Brightness)candidate.brightness=model.settingsValue;else candidate.idleSeconds=model.settingsValue*60;
            SettingsStore store;if(!store.save(candidate)){strcpy(model.lastNotice,"保存失败，请重试");return false;}*settings_=candidate;
        }
        if(page==SettingsPage::Idle)model.idleRow=0;else page=SettingsPage::List;
        strcpy(model.lastNotice,"已保存");
    }
    return true;
}
bool Runtime::post(Event event) {
    if (xQueueSend(events_, &event, 0) == pdTRUE) return true;
    ++queueDrops;
    return false;
}
void Runtime::poll() {
    Event event{};
    while (xQueueReceive(events_, &event, 0) == pdTRUE) {
        if (event.generation != generation_ || event.app != active_) ++rejectedEvents;
        else ++model.framesAccepted;
    }
}
void Runtime::tick(uint64_t now) {
    now_ = now;
    const bool wasFinished=model.focusFinished;
    model.tick(active_, now);
    if(active_==App::Focus&&!wasFinished&&model.focusFinished)lastInput_=now;
    auto& m = model;
    if (m.noisePlaying && m.noiseUntil && now >= m.noiseUntil) { m.noisePlaying = false; m.noiseUntil = 0; }
    if (m.alarmRinging && now >= m.alarmRingAt + 180) m.alarmRinging = false;
    if (m.todoRinging && now >= m.todoRingAt + 180) endTodoRing(TodoRing::Stop);
    // 对讲机: a talk ends by itself after kWalkieTalk seconds; a page a call opened goes back once the call is over.
    if (active_ == App::Walkie && m.walkieTalk && now >= m.walkieAt + Model::kWalkieTalk) { m.walkieTalk = false; m.walkieAt = now; }
    if (active_ == App::Walkie && m.walkieAuto && !m.walkieTalk && m.walkiePhase != 2 && now >= m.walkieAt + Model::kWalkieQuiet) switchTo(m.walkieBack);
    // 闹钟: checked once a second. A ring is due for a minute from its time (a snoozed one from its new time), so
    // a page switch or a boot from deep sleep a few seconds late still rings; alarmFired keeps it to once.
    if (!m.clockValid || now == alarmChecked_) return;
    alarmChecked_ = now;
    const time_t clock = time(nullptr);
    // 待办提醒: the next reminder rings once, from any page. One missed by more than six hours (the badge was off) is
    // only marked: the list shows it overdue.
    if (m.todoNextAt && !m.todoRinging && !m.alarmRinging && clock >= static_cast<time_t>(m.todoNextAt)) {
        if (clock - m.todoNextAt > 6 * 3600) endTodoRing(TodoRing::Stop);
        else if (switchTo(App::Todo) == Result::Ok) { m.todoRinging = true; m.todoRingAt = now; lastInput_ = now; screenOff_ = false; }
    }
    const time_t due = m.alarmSnooze ? m.alarmSnooze : nextAlarm(m.alarms, clock - 59);
    if (m.alarmRinging || !due || due > clock || static_cast<uint32_t>(due) == m.alarmFired) return;
    // A one-off alarm turns itself off once it rings.
    if (!m.alarmSnooze) {
        bool changed = false;
        for (unsigned i = 0; i < m.alarms.count; ++i) if (auto& a = m.alarms.items[i]; a.mode == 3 && nextAlarm(a, clock - 59) == due) { a.mode = 0; changed = true; }
        if (changed) SettingsStore::saveAlarms(m.alarms);
    }
    m.alarmFired = static_cast<uint32_t>(due); m.alarmSnooze = 0;
    if (switchTo(App::Alarm) != Result::Ok) return;
    m.alarmRinging = true; m.alarmRingAt = now; lastInput_ = now; screenOff_ = false;
}
bool Runtime::codexNotice(const char* text) {
    if (active_ != App::Codex) return false;
    model.notify(text);
    return true;
}
void Runtime::connection(bool configuredSession, uint64_t now) {
    if(connected_==configuredSession) return;
    connected_ = configuredSession;
    if (!connected_) unpluggedAt_ = lastInput_ = now;
}
bool Runtime::setIdleMinutes(uint32_t minutes) {
    if(!settings_||minutes>1440)return false;
    auto candidate=*settings_;candidate.idleSeconds=minutes*60;
    SettingsStore store;if(!store.save(candidate))return false;
    *settings_=candidate;lastInput_=now_;return true;
}
Power Runtime::power(uint64_t now) const {
    // Turned off by hand on the home page: dark even on USB, and idle may still take it to deep sleep.
    if (screenOff_ && active_ == App::Home) { const auto power = automaticPower(now); return power == Power::Awake ? Power::ScreenOff : power; }
    return automaticPower(now);
}
Power Runtime::automaticPower(uint64_t now) const {
    // This returns a decision; the simulator NEVER enters physical deep sleep.
    // A warning light that dims or sleeps is no warning.
    // So is a status sign that went dark; the sign's own switch decides.
    // A running metronome keeps its beat on screen.
    // A ringing alarm lights the screen.
    if ((connected_ && model.usbAwake) || flashBusy_ || quarantined_ || active_ == App::Alert || active_ == App::Torch || (active_ == App::Status && model.status.awake) || (active_ == App::Alarm && model.alarmRinging) || (active_ == App::Todo && model.todoRinging)
        || (active_ == App::Sound && model.soundRunning) || (active_ == App::Walkie && (model.walkieTalk || model.walkiePhase == 2))) return Power::Awake;
    (void)suspended_; // USB suspend alone must not destroy an enumerated session.
    const auto idle = settings_ ? settings_->idleSeconds : 360;
    if (!idle || now - lastInput_ < idle || now - unpluggedAt_ < idle) return Power::Awake;
    // An open audio page or a BLE connection is not user activity.
    // Nor is a quiet stretch of a talk: the PPT page keeps its computer link with the screen off, and so does
    // 语音输入 (deep sleep would drop the Bluetooth link, and Confirm cannot wake it).
    // A running stopwatch must not lose its time to deep sleep either, nor the motion monitor its watch, nor
    // 离座锁屏 its Bluetooth link (the computer reads its signal; a lost link locks the screen).
    // Voice wake listens on the home page and the menu, which deep sleep would end, and a playing radio or podcast plays on.
    // 对讲机 listens on its page, and on every page with 后台接收对讲 on.
    // White noise plays on too, and a snoozed alarm must still be awake to ring again.
    // On USB with 连电脑时常亮 off only the screen goes dark: deep sleep would drop the computer link.
    if (connected_ || ((active_ == App::Radio || active_ == App::Podcast) && model.mediaPlaying) || (active_ == App::Noise && model.noisePlaying) || model.alarmSnooze || model.focusRunning || model.swRunning || active_ == App::Motion || active_ == App::Guard || ((active_ == App::Slides || active_ == App::Dictation) && model.bleConnected)
        || ((active_ == App::Home || active_ == App::Menu) && model.wakeWord) || model.walkieListen || active_ == App::Walkie) return Power::ScreenOff;
    return Power::DeepSleepIntent;
}
bool Runtime::shutdown() {
    if (switchTo(App::Home) != Result::Ok || !worker_.shutdown()) return false;
    if (workspace_) heap_caps_free(workspace_);
    workspace_ = nullptr;
    if (events_) vQueueDelete(events_);
    events_ = nullptr;
    return true;
}
} // namespace passport
