#pragma once
#include "apps.hpp"
#include "city_list.hpp"
#include "voice_sounds.hpp"
#include "lunar.hpp"
#include "settings.hpp"
#include "todo_time.hpp"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <string>
// What a spoken sentence adds to 汇率, 闹钟, 倒数日, 自选股 or the city in 系统设置, worked out on the badge:
//   "加上日元和泰铢" -> JPY, THB; "工作日早上7点半" -> 07:30 工作日; "妈妈生日 农历八月十五" -> 每年 农历 8/15;
//   "我在苏州" -> 苏州; "添加比亚迪" -> the search word 比亚迪; "打开中英翻译" -> the 中英翻译 page.
namespace passport {
namespace voice {
using todo_time::at;
inline size_t charBytes(unsigned char c) { return c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4; }
// The sentence without punctuation and spaces (the cloud ends most with "。"); colons stay, for 7:30.
inline std::string bare(const char* heard) {
    std::string out;
    for (const char* s = heard; *s;) {
        if (strchr(" ,.;!?\t\n", *s)) { ++s; continue; }
        bool mark = false;
        for (const char* m : {"，", "、", "。", "；", "！", "？", "～", "“", "”", "「", "」"}) if (!strncmp(s, m, strlen(m))) { s += strlen(m); mark = true; break; }
        if (mark) continue;
        const size_t n = charBytes(static_cast<unsigned char>(*s));
        out.append(s, strnlen(s, n)); s += strnlen(s, n);
    }
    return out;
}
inline void erase(std::string& s, std::initializer_list<const char*> words) {
    for (const char* w : words) for (size_t i; (i = s.find(w)) != std::string::npos;) s.erase(i, strlen(w));
}
// At most `chars` characters of s.
inline std::string head(const std::string& s, unsigned chars) {
    size_t n = 0;
    while (n < s.size() && chars--) n += charBytes(static_cast<unsigned char>(s[n]));
    return s.substr(0, n);
}
inline unsigned chars(const std::string& s) { unsigned n = 0; for (unsigned char c : s) n += (c & 0xc0) != 0x80; return n; }
// The code point of the three-byte UTF-8 character at s (every Chinese one), else 0.
inline uint32_t codePoint(const char* s) {
    const auto c = static_cast<unsigned char>(s[0]);
    return c >= 0xe0 && c < 0xf0 && s[1] && s[2] ? (c & 15u) << 12 | (s[1] & 63u) << 6 | (s[2] & 63u) : 0;
}
// Two characters read alike (a pinyin without tones in common, as voice_sounds.hpp lists them).
inline bool soundsAlike(const char* a, const char* b) {
    const uint32_t x = codePoint(a), y = codePoint(b);
    if (!x || !y) return false;
    if (x == y) return true;
    const auto readings = [](uint32_t c) { return std::equal_range(std::begin(kSoundCodes), std::end(kSoundCodes), uint16_t(c)); };
    const auto [a0, a1] = readings(x); const auto [b0, b1] = readings(y);
    for (auto i = a0; i != a1; ++i) for (auto j = b0; j != b1; ++j) if (kSounds[i - kSoundCodes] == kSounds[j - kSoundCodes]) return true;
    return false;
}
// The bytes of s that say word, character by character the same or alike; 0 when they do not.
inline size_t soundsLike(const char* s, const char* word) {
    size_t used = 0;
    for (const char* w = word; *w;) {
        const size_t n = charBytes(static_cast<unsigned char>(*w)), m = charBytes(static_cast<unsigned char>(s[used]));
        if (!s[used] || (strncmp(s + used, w, n) && !soundsAlike(s + used, w))) return 0;
        used += m; w += n;
    }
    return used;
}
}

// 汇率: the currencies named in the sentence, in order, each once (codes from kCurrencies). Besides their names it
// knows what people say: 美金, 日币, 韩币, 台币, 港元, 澳币, 加币, 新币, 泰币, 马币 ... When none is named exactly, a
// homophone of a name counts: the speech service, given two or three words, writes 含元 for 韩元 or 奥园 for 澳元.
struct CurrencyWord { const char* word; const char* code; };
inline constexpr CurrencyWord kCurrencyWords[]{
    {"美金","USD"},{"美刀","USD"},{"日币","JPY"},{"日圆","JPY"},{"韩币","KRW"},{"韩国元","KRW"},{"台币","TWD"},{"港元","HKD"},{"港纸","HKD"},
    {"澳门币","MOP"},{"澳币","AUD"},{"澳大利亚元","AUD"},{"加币","CAD"},{"加拿大元","CAD"},{"新币","SGD"},{"新加坡币","SGD"},{"泰币","THB"},
    {"马币","MYR"},{"马来西亚币","MYR"},{"卢比","INR"},{"印度卢布","INR"},{"英磅","GBP"},{"法郎","CHF"},{"越南币","VND"},{"菲律宾币","PHP"},
    {"印尼币","IDR"},{"俄罗斯卢布","RUB"},{"里拉","TRY"},{"雷亚尔","BRL"},{"兰特","ZAR"},{"坚戈","KZT"},{"图格里克","MNT"},{"基普","LAK"},
    {"瑞尔","KHR"},{"缅币","MMK"},{"人民币元","CNY"}};
inline unsigned spokenCurrencies(const char* heard, const char** codes, unsigned capacity) {
    unsigned count = 0;
    for (bool alike : {false, true}) for (const char* s = heard; *s && !(alike && count);) {
        // The longest name at this place: 新台币 before 台币, 澳门元 before 澳元.
        const char* code = nullptr; size_t length = 0;
        const auto match = [&](const char* word, const char* c) {
            const size_t n = alike ? voice::soundsLike(s, word) : !strncmp(s, word, strlen(word)) ? strlen(word) : 0;
            if (n > length) { length = n; code = c; } };
        for (const auto& c : kCurrencies) match(c.name, c.code);
        for (const auto& w : kCurrencyWords) match(w.word, w.code);
        if (!code) { s += voice::charBytes(static_cast<unsigned char>(*s)); continue; }
        s += length;
        bool seen = false;
        for (unsigned i = 0; i < count; ++i) seen |= !strcmp(codes[i], code);
        if (!seen && count < capacity) codes[count++] = code;
    }
    return count;
}

// 闹钟: the time said, and how it repeats (每天 / 工作日; otherwise once, within the next 24 hours).
// mode as AlarmPrefs (1 每天, 2 工作日, 3 一次); error says why not when ok is false.
struct AlarmParse { bool ok{}; uint8_t hour{}, minute{}, mode{3}; time_t at{}; const char* error{}; };
inline AlarmParse parseAlarm(const char* heard, time_t now) {
    AlarmParse out; std::string s = voice::bare(heard);
    const auto has = [&](std::initializer_list<const char*> words) { bool found = false; for (const char* w : words) if (s.find(w) != std::string::npos) found = true; voice::erase(s, words); return found; };
    if (has({"工作日", "上班日", "周一到周五", "星期一到星期五"})) out.mode = 2;
    else if (has({"每天", "天天", "每日", "每一天"})) out.mode = 1;
    // The whole sentence is a time, so a bare "七点" counts (the todo parser wants a day beside Chinese numerals).
    const bool clock = s.find("点") != std::string::npos || s.find(":") != std::string::npos || s.find("：") != std::string::npos;
    if (out.mode != 3) {
        // A repeating alarm keeps the hour said: worked out from midnight, nothing has passed yet.
        tm day{}; localtime_r(&now, &day); day.tm_hour = day.tm_min = day.tm_sec = 0; day.tm_isdst = -1;
        const time_t midnight = mktime(&day);
        auto parsed = parseTodo(s.c_str(), midnight);
        if (!parsed.at && clock) parsed = parseTodo(("今天" + s).c_str(), midnight);
        out.at = parsed.at;
    } else {
        auto parsed = parseTodo(s.c_str(), now);
        if (!parsed.at && clock) parsed = parseTodo(("今天" + s).c_str(), now);
        out.at = parsed.at ? (parsed.at + 30) / 60 * 60 : 0;  // "20分钟后" to the nearest minute
        if (out.at && out.at <= now) out.at += 86400;
        if (out.at > now + 86400) { out.error = "闹钟只能定 24 小时内，更远的请用待办提醒"; return out; }
    }
    if (!out.at) { out.error = "没听到时间，例：明早 7 点、工作日 7 点半"; return out; }
    tm t{}; localtime_r(&out.at, &t); out.hour = static_cast<uint8_t>(t.tm_hour); out.minute = static_cast<uint8_t>(t.tm_min);
    out.ok = true;
    return out;
}

// 倒数日: a name and its date. "考研 12月20日", "春节回家 2027年2月5日", "100天后 出发", "结婚纪念日 5月20号" (every year:
// said 每年, or a 生日/纪念日/周年), "妈妈生日 农历八月十五" (lunar dates repeat unless a year is said).
// A one-off date without a year is the next one to come.
struct CountdownParse { bool ok{}; CountdownBoard::Event event{}; bool cut{}; const char* error{}; };
inline CountdownParse parseCountdown(const char* heard, Date today) {
    using todo_time::number; using todo_time::at;
    CountdownParse out; std::string s = voice::bare(heard);
    const auto has = [&](std::initializer_list<const char*> words) { bool found = false; for (const char* w : words) if (s.find(w) != std::string::npos) found = true; voice::erase(s, words); return found; };
    bool lunar = has({"农历", "阴历", "旧历"});
    bool yearly = has({"每年的", "每年"});
    voice::erase(s, {"：", ":", "帮我", "添加", "新增", "加一个", "加个", "记一下", "设置", "倒数日", "倒计时", "还有多少天", "还有几天", "多少天", "距离"});
    std::string keep(s.size(), 1);
    const auto cut = [&](size_t from, size_t to) { for (size_t k = from; k < to && k < s.size(); ++k) keep[k] = 0; };
    int year = 0, month = 0, day = 0, offset = -1;
    for (size_t i = 0; i < s.size();) {
        int value; bool arabic; size_t n;
        // A year: four digits (2027, 二零二七) before 年; 今年, 明年.
        if (at(s, i, "今年") || at(s, i, "明年")) { year = today.year + at(s, i, "明年"); cut(i, i + 6); i += 6; continue; }
        {
            size_t k = i; int y = 0, digits = 0;
            while (k < s.size() && digits < 4) {
                if (s[k] >= '0' && s[k] <= '9') { y = y * 10 + (s[k] - '0'); ++k; ++digits; continue; }
                static const char* const names[]{"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
                int d = -1; for (int v = 0; v < 10; ++v) if (at(s, k, names[v])) d = v;
                if (at(s, k, "〇")) d = 0;
                if (d < 0) break;
                y = y * 10 + d; k += 3; ++digits;
            }
            if (digits == 4 && at(s, k, "年")) { year = y; cut(i, k + 3); i = k + 3; continue; }
        }
        // Days from today: N 天后, 明天, 后天, 大后天.
        for (const auto& [word, days] : {std::pair{"大后天", 3}, std::pair{"后天", 2}, std::pair{"明天", 1}})
            if (at(s, i, word)) { offset = days; cut(i, i + strlen(word)); i += strlen(word); goto next; }
        if ((n = number(s, i, value, arabic)) && at(s, i + n, "天")) {
            size_t k = i + n + 3;
            for (const char* w : {"以后", "之后", "后"}) if (at(s, k, w)) { offset = value; cut(i, k + strlen(w)); i = k + strlen(w); goto next; }
        }
        // A month: 正月, 冬月, 腊月 (lunar), or N 月.
        {
            size_t k = i; int m = 0;
            // 正月, 冬月, 腊月, 初X and 廿X are only said of lunar dates.
            bool named = true;
            if (at(s, k, "正月")) { m = 1; k += 6; } else if (at(s, k, "冬月")) { m = 11; k += 6; } else if (at(s, k, "腊月")) { m = 12; k += 6; }
            else if ((n = number(s, k, value, arabic)) && at(s, k + n, "月") && value >= 1 && value <= 12) { m = value; k += n + 3; named = false; }
            if (m) {
                // Its day: 初X, N 日/号, or a bare number after the month (十五, 20).
                int d = 0; size_t used = 0;
                if (at(s, k, "初") && (used = number(s, k + 3, value, arabic)) && value >= 1 && value <= 10) { d = value; used += 3; named = true; }
                else if (at(s, k, "廿") && (used = number(s, k + 3, value, arabic)) && value >= 1 && value <= 9) { d = 20 + value; used += 3; named = true; }
                else if ((used = number(s, k, value, arabic)) && value >= 1 && value <= 31) d = value;
                if (d) {
                    k += used; if (at(s, k, "日") || at(s, k, "号")) k += 3;
                    month = m; day = d; lunar |= named; cut(i, k); i = k; continue;
                }
            }
        }
        i += voice::charBytes(static_cast<unsigned char>(s[i]));
    next:;
    }
    // The name: what is left, without the joining words.
    std::string name;
    for (size_t i = 0; i < s.size(); ++i) if (keep[i]) name += s[i];
    voice::erase(name, {"那天", "这天", "的时候", "是在", "是", "在"});
    while (name.size() >= 3 && (at(name, name.size() - 3, "的") || at(name, name.size() - 3, "号"))) name.resize(name.size() - 3);
    while (name.size() >= 3 && at(name, 0, "的")) name.erase(0, 3);
    if (name.find("生日") != std::string::npos || name.find("纪念日") != std::string::npos || name.find("周年") != std::string::npos) yearly = true;
    if (!month && offset < 0) { out.error = "没听到日期，例：考试 12月20日"; return out; }
    if (name.empty()) { out.error = "没听到名称，例：妈妈生日 农历八月十五"; return out; }
    auto& e = out.event;
    if (offset >= 0) {
        const Date d = fromDayNumber(dayNumber(today) + offset);
        e.date = static_cast<uint32_t>(d.year * 10000 + d.month * 100 + d.day);
    } else if (lunar) {
        if (day > 30) { out.error = "农历每月最多三十天"; return out; }
        if (!year) yearly = true;
        Date check{};
        if (!yearly && !fromLunar(year, month, day, check)) { out.error = "这一年的农历没有这一天"; return out; }
        e.date = static_cast<uint32_t>((year ? year : today.year) * 10000 + month * 100 + day);
        e.flags = CountdownBoard::kLunar;
    } else {
        int y = year ? year : today.year;
        if (!year && !yearly && dayNumber({y, month, std::min(day, monthDays(y, month))}) < dayNumber(today)) ++y;
        if (day > monthDays(y, month) && !(yearly && month == 2 && day == 29)) { out.error = "这个月没有这一天"; return out; }
        e.date = static_cast<uint32_t>(y * 10000 + month * 100 + day);
    }
    if (yearly && offset < 0) e.flags |= CountdownBoard::kYearly;
    // At most eight characters (the record holds 24 bytes).
    const std::string shown = voice::head(name, 8);
    out.cut = shown.size() < name.size();
    memcpy(e.name, shown.data(), shown.size());
    out.ok = true;
    return out;
}

// 系统设置 > 所在城市: the city named in the sentence (the longest name found: 呼和浩特 before 和田), or nullptr.
inline const CityEntry* spokenCity(const char* heard) {
    const CityEntry* found = nullptr; size_t length = 0;
    for (const auto& c : kCities) { const size_t n = strlen(c.name); if (n > length && strstr(heard, c.name)) { found = &c; length = n; } }
    return found;
}
// A city by its name as a lookup service gives it ("杭州市", "杭州"), or nullptr.
inline const CityEntry* cityNamed(const char* name) {
    for (const auto& c : kCities) { const size_t n = strlen(c.name); if (!strncmp(name, c.name, n) && (!name[n] || !strcmp(name + n, "市"))) return &c; }
    return nullptr;
}
// A province as a lookup service gives it, the way the setup page keeps it: 浙江省 -> 浙江, 广西壮族自治区 -> 广西,
// and "" for the municipalities.
inline std::string provinceName(const char* name) {
    for (const char* city : {"北京", "上海", "天津", "重庆"}) if (!strncmp(name, city, strlen(city))) return "";
    std::string s(name);
    for (const char* tail : {"壮族自治区", "回族自治区", "维吾尔自治区", "自治区", "特别行政区", "省", "市"})
        if (s.size() > strlen(tail) && !s.compare(s.size() - strlen(tail), strlen(tail), tail)) { s.resize(s.size() - strlen(tail)); break; }
    return s;
}

// 自选股: the name or code to search for, without the asking around it ("帮我添加比亚迪" -> 比亚迪).
inline std::string stockQuery(const char* heard) {
    std::string s = voice::bare(heard);
    voice::erase(s, {"：", ":", "帮我", "请", "添加", "加入", "加上", "加一个", "加个", "关注", "自选股", "自选", "股票", "一下", "这只", "这个"});
    if (s.size() > 3 && !s.compare(0, 3, "加")) s.erase(0, 3);
    while (s.size() >= 3 && !s.compare(s.size() - 3, 3, "的")) s.resize(s.size() - 3);
    return voice::head(s, 12);
}

// 说功能名跳转 (语音指令): the page a whole sentence names, else App::Count. Only the whole sentence counts, so a
// question about a page ("中英翻译怎么用") stays a question. Around the name it allows asking words: "小智，打开汇率",
// "回到首页", "翻译页面". A homophone of a whole name counts too (会率 for 汇率), as for currencies.
// Names are kept bare and in lower case, the way the sentence is compared. sim_main checks every menu title is here.
struct AppWord { App app; const char* word; };
inline constexpr AppWord kAppWords[]{
    {App::Home,"首页"},{App::Home,"主页"},{App::Home,"时钟与天气"},{App::Home,"时钟"},{App::Home,"返回"},{App::Home,"桌面"},
    {App::Menu,"功能列表"},{App::Menu,"功能"},{App::Menu,"菜单"},
    {App::Chat,"小智语音"},{App::Chat,"小智"},{App::Chat,"聊天"},{App::Chat,"对话"},
    {App::Memo,"语音笔记"},{App::Memo,"笔记"},{App::Memo,"记笔记"},{App::Dictation,"语音输入"},{App::Dictation,"听写"},
    {App::Codex,"ai额度"},{App::Codex,"额度"},{App::Monitor,"电脑状态"},
    {App::Slides,"ppt翻页"},{App::Slides,"ppt"},{App::Slides,"翻页笔"},{App::Slides,"幻灯片"},
    {App::Focus,"番茄钟"},{App::Focus,"番茄"},{App::Focus,"专注"},{App::Stopwatch,"秒表"},{App::Stopwatch,"计时"},
    {App::Alarm,"闹钟"},{App::Alarm,"闹铃"},{App::Calendar,"万年历"},{App::Calendar,"日历"},{App::Calendar,"黄历"},
    {App::Countdown,"倒数日"},{App::Countdown,"倒数"},{App::History,"历史今日"},{App::History,"历史上的今天"},
    {App::Forecast,"天气预报"},{App::Forecast,"天气"},{App::Typhoon,"台风路径"},{App::Typhoon,"台风"},
    {App::Quake,"地震速报"},{App::Quake,"地震"},{App::News,"财经新闻"},{App::News,"新闻"},
    {App::Stock,"自选股"},{App::Stock,"股票"},{App::Stock,"自选"},{App::Stock,"股市"},{App::Rate,"汇率"},
    {App::Hot,"百度热榜"},{App::Hot,"热榜"},{App::Hot,"热搜"},{App::Hacker,"hackernews"},{App::Hacker,"hn头条"},
    {App::Quiz,"知识问答"},{App::Quiz,"问答"},{App::Quiz,"答题"},{App::Words,"背单词"},{App::Words,"单词"},
    {App::Daily,"每日一句"},{App::Radio,"收音机"},{App::Radio,"电台"},{App::Radio,"广播"},{App::Podcast,"播客"},
    {App::Noise,"白噪音"},{App::Noise,"噪音"},{App::Sound,"声音"},{App::Sound,"分贝"},{App::Status,"状态牌"},
    {App::Qr,"二维码"},{App::Torch,"手电筒"},{App::Torch,"手电"},{App::Alert,"警示灯"},{App::Alert,"爆闪"},
    {App::Motion,"移动监控"},{App::Motion,"监控"},{App::Sonar,"蓝牙声纳"},{App::Sonar,"声纳"},{App::Sonar,"声呐"},
    {App::Wheel,"随机转盘"},{App::Wheel,"转盘"},{App::Wifi,"wifi工具箱"},{App::Wifi,"wifi"},
    {App::Settings,"系统设置"},{App::Settings,"设置"},{App::Translate,"中英翻译"},{App::Translate,"翻译"},
    {App::Todo,"待办提醒"},{App::Todo,"待办"},{App::Todo,"提醒"},{App::Guard,"离座锁屏"},{App::Guard,"锁屏"},
    {App::Ledger,"语音记账"},{App::Ledger,"记账"},{App::Ledger,"账本"},
    {App::Calc,"语音计算器"},{App::Calc,"计算器"},{App::Calc,"语音计算"},{App::Calc,"计算"}};
inline App spokenApp(const char* heard) {
    std::string s = voice::bare(heard);
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    const auto named = [](const std::string& t, bool alike) {
        for (const auto& w : kAppWords)
            if (alike ? voice::chars(t) == voice::chars(w.word) && voice::soundsLike(t.c_str(), w.word) == t.size() : t == w.word) return w.app;
        return App::Count;
    };
    // Exact names first, then homophones; each tried as said, then shedding one asking word at a time.
    for (bool alike : {false, true}) {
        for (std::string t = s; !t.empty();) {
            if (const App app = named(t, alike); app != App::Count) return app;
            bool shed = false;
            for (const char* w : {"小智", "请", "帮我", "给我", "麻烦", "我要", "我想", "在胸牌上", "在胸牌", "胸牌上的", "胸牌里的", "胸牌的", "胸牌上", "胸牌", "打开", "进入", "切换到", "切换", "切到", "跳转到", "跳到", "转到",
                                  "回到", "返回到", "返回", "启动", "开启", "查看", "看看", "看一下", "看下", "看", "去", "到"})
                if (!t.compare(0, strlen(w), w)) { t.erase(0, strlen(w)); shed = true; break; }
            if (!shed) for (const char* w : {"页面", "界面", "功能", "一下", "页", "吧", "了", "呀", "啊"})
                if (t.size() > strlen(w) && !t.compare(t.size() - strlen(w), strlen(w), w)) { t.resize(t.size() - strlen(w)); shed = true; break; }
            if (!shed) break;
        }
    }
    return App::Count;
}
// 打开 Mac 程序 (语音指令): the program a sentence asks the computer to open, else "". Every sentence led by
// 打开/启动 is a command: a badge page's name wins ("打开计算器", "打开胸牌的计算器", "打开电脑状态"); 「Mac 的」 or
// 「电脑上」 sends the rest to the computer ("打开 Mac 的计算器", "电脑上打开计算器"), and so does a name no page has
// ("打开 Chrome", "打开微信怎么用" too); the computer looks it up among its programs. Not led by one, it is a chat.
// The speech service often writes "Mac" as 麦克, 迈克, 麦客 ...: any 麦/迈/买/卖/脉 then a ke sound, but not 麦克风.
inline bool saysMac(const std::string& s, size_t at) {
    const auto starts = [&s](size_t i, std::initializer_list<const char*> words) {
        for (const char* w : words) if (!s.compare(i, strlen(w), w)) return true;
        return false;
    };
    return starts(at, {"麦", "迈", "买", "卖", "脉", "埋"}) && starts(at + 3, {"克", "客", "可", "科", "课", "刻", "柯", "颗", "壳", "氪", "珂", "磕", "咳", "苛", "坷", "轲"})
        && !starts(at + 6, {"风", "笔", "尔"});
}
inline std::string spokenMac(const char* heard) {
    if (spokenApp(heard) != App::Count) return {};
    std::string s = voice::bare(heard);
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    const auto shed = [&s](std::initializer_list<const char*> words) {
        for (const char* w : words) if (!s.compare(0, strlen(w), w)) { s.erase(0, strlen(w)); return true; }
        return false;
    };
    const auto place = [&] {  // 「在 Mac 上」, 「电脑里的」, 「麦克的」 ...
        for (const char* at : {"在", "用", ""}) {
            if (s.compare(0, strlen(at), at)) continue;
            size_t n = saysMac(s, strlen(at)) ? 6 : 0;
            for (const char* where : {"苹果电脑", "mac", "电脑"}) if (!n && !s.compare(strlen(at), strlen(where), where)) n = strlen(where);
            if (n) { s.erase(0, strlen(at) + n); shed({"上的", "里的", "上", "里", "的"}); return; }
        }
    };
    while (shed({"小智", "请", "帮我", "给我", "麻烦", "我要", "我想", "你", "能不能", "可不可以", "可以", "能"})) {}
    place();
    if (!shed({"打开", "启动", "开启", "运行", "切换到", "切到", "切换"})) return {};
    shed({"一下"});
    if (!s.compare(0, 6, "胸牌") || !s.compare(0, 9, "在胸牌")) return {};  // the badge's, and no page of it
    place();
    for (bool more = true; more && !s.empty();) {
        more = false;
        for (const char* w : {"好不好", "好吗", "这个", "一下", "程序", "应用", "软件", "app", "吗", "呢", "吧", "了", "呀", "啊"})
            if (s.size() > strlen(w) && !s.compare(s.size() - strlen(w), strlen(w), w)) { s.resize(s.size() - strlen(w)); more = true; break; }
    }
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '"' || c == '\\'; }), s.end());
    return voice::head(s, 12);
}
}
