#pragma once
#include "ledger.hpp"  // ledger_text::number and friends: the same spoken-number parser
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <initializer_list>
// 健康记录: 喝水 (a key per cup), 体重 / 血糖 / 体温 / 血压 (spoken: "62.5", "血糖 6.2 空腹", "120 80 心率 72") and 经期
// ("月经来了", "昨天结束的", or keys) with 亲密 (a key per time, or "昨天同房 有保护"). Everything is worked out on the badge; the computer's assistant takes every new
// or changed record over USB or Bluetooth into its CSV and acknowledges it by id and revision, as 语音记账 does.
// Unlike the ledger, the badge keeps the last kMax records (it shows their history), dropping the oldest the computer
// already has when it needs room.
namespace passport {
enum class HealthKind : uint8_t { Water, Weight, Glucose, Temp, Pressure, Period, Intimacy, Count };
inline constexpr unsigned kHealthKinds = static_cast<unsigned>(HealthKind::Count);
inline constexpr const char* kHealthNames[]{"喝水", "体重", "血糖", "体温", "血压", "经期", "亲密"};
inline constexpr const char* kHealthIds[]{"water", "weight", "glucose", "temperature", "pressure", "period", "intimacy"};
inline constexpr const char* kGlucoseTags[]{"随机", "空腹", "早餐后", "午餐后", "晚餐后", "睡前"};
inline constexpr uint8_t kGlucoseTagCount = 6;
inline constexpr const char* kFlowNames[]{"", "量少", "量中", "量多"};  // 经期 v1
inline constexpr const char* kProtectNames[]{"", "有保护", "无保护"};  // 亲密 tag
// Values: 喝水 v1 cups, v2 ml a cup (one record a day, at = the last cup); 体重 kg x10; 血糖 mmol/L x10 (tag
// kGlucoseTags); 体温 °C x10; 血压 v1/v2 mmHg, v3 pulse (0: none); 经期 tag 0 start / 1 end, v1 flow, at = that day's noon; 亲密 tag kProtectNames (at: the time, or noon of a day said).
struct HealthEntry { uint32_t id, at; uint8_t kind, tag, flags, rev; int16_t v1, v2, v3; };
// The local calendar day of an epoch, as days since 1970-01-01.
inline int healthDay(time_t at) {
    tm t{}; localtime_r(&at, &t); int y = t.tm_year + 1900; const int m = t.tm_mon + 1, d = t.tm_mday;
    y -= m <= 2; const int era = (y >= 0 ? y : y - 399) / 400; const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int(doe) - 719468;
}
// Noon, daysAgo days before `now` (local time).
inline time_t healthNoon(time_t now, int daysAgo) {
    tm t{}; localtime_r(&now, &t); t.tm_mday -= daysAgo; t.tm_hour = 12; t.tm_min = t.tm_sec = 0; t.tm_isdst = -1; return mktime(&t);
}

struct HealthBook {
    // 64 records fit the page block the other pages already need (1360 B), so the page costs no RAM; the computer's
    // CSV keeps the whole history.
    static constexpr uint8_t kVersion = 1, kMax = 64, kSynced = 1, kDeleted = 2, kKeepPeriods = 8;
    uint8_t version{kVersion}, count{}; uint16_t pad{}; uint32_t lastId{};
    HealthEntry items[kMax]{};  // in the order recorded
    bool valid() const {
        if (version != kVersion || count > kMax) return false;
        for (unsigned i = 0; i < count; ++i) if (items[i].kind >= kHealthKinds) return false;
        return true;
    }
    int find(uint32_t id) const { for (unsigned i = 0; i < count; ++i) if (items[i].id == id) return int(i); return -1; }
    void erase(unsigned i) { memmove(items + i, items + i + 1, (count - i - 1) * sizeof(HealthEntry)); items[--count] = {}; }
    unsigned pending() const { unsigned n = 0; for (unsigned i = 0; i < count; ++i) n += !(items[i].flags & kSynced); return n; }
    bool live(unsigned i) const { return !(items[i].flags & kDeleted); }
    void touch(unsigned i) { items[i].flags &= ~kSynced; ++items[i].rev; }
    // Removed here: one the computer never had just goes; otherwise it is kept, marked, until the computer is told.
    void remove(unsigned i) { if (!(items[i].flags & kSynced) && !items[i].rev) erase(i); else { items[i].flags |= kDeleted; touch(i); } }
    bool ack(uint32_t id, uint8_t rev) {
        const int i = find(id);
        if (i < 0 || items[i].rev != rev || (items[i].flags & kSynced)) return false;
        if (items[i].flags & kDeleted) erase(unsigned(i)); else items[i].flags |= kSynced;
        return true;
    }
    // A new record (false: full of records the computer has not taken yet). Room comes from the oldest one the
    // computer has, sparing the last kKeepPeriods 经期 records the prediction needs.
    bool add(HealthEntry e) {
        if (count == kMax) {
            unsigned periods = 0; for (unsigned i = 0; i < count; ++i) periods += items[i].kind == uint8_t(HealthKind::Period);
            int drop = -1;
            for (unsigned i = 0; i < count && drop < 0; ++i)
                if ((items[i].flags & kSynced) && (items[i].kind != uint8_t(HealthKind::Period) || periods > kKeepPeriods)) drop = int(i);
            if (drop < 0) return false;
            erase(unsigned(drop));
        }
        e.id = lastId = e.at > lastId ? e.at : lastId + 1; e.flags = 0; e.rev = 0;
        items[count++] = e;
        return true;
    }
    // Live records of a kind, newest first (by time); returns how many (at most `most`), their indexes in order.
    unsigned list(HealthKind kind, uint8_t* order, unsigned most) const {
        unsigned n = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (items[i].kind != uint8_t(kind) || !live(i)) continue;
            unsigned at = n < most ? n++ : most;  // a full list keeps the newest
            while (at > 0 && items[order[at - 1]].at <= items[i].at) { if (at < most) order[at] = order[at - 1]; --at; }
            if (at < most) order[at] = uint8_t(i);
        }
        return n;
    }
    int latest(HealthKind kind) const { uint8_t order[1]; return list(kind, order, 1) ? order[0] : -1; }
    // Today's 喝水 record (-1: none yet).
    int water(int today) const {
        for (unsigned i = count; i-- > 0;) if (items[i].kind == uint8_t(HealthKind::Water) && live(i) && healthDay(items[i].at) == today) return int(i);
        return -1;
    }
    unsigned cups(int today) const { const int i = water(today); return i < 0 ? 0 : unsigned(items[i].v1); }
};

// Reading a record: a level (0 fine, 1 low, 2 a little high, 3 high) and its word, by the usual Chinese guidelines.
// The badge only records; the page says it does not diagnose.
struct HealthFlag { uint8_t level; const char* text; };
inline HealthFlag healthFlag(const HealthEntry& e) {
    switch (HealthKind(e.kind)) {
    case HealthKind::Glucose:
        if (e.v1 < 39) return {1, "偏低"};
        if (e.tag == 1) return e.v1 >= 70 ? HealthFlag{3, "偏高"} : e.v1 >= 61 ? HealthFlag{2, "略高"} : HealthFlag{0, ""};
        return e.v1 >= 111 ? HealthFlag{3, "偏高"} : e.v1 >= 78 ? HealthFlag{2, "略高"} : HealthFlag{0, ""};
    case HealthKind::Temp:
        return e.v1 >= 385 ? HealthFlag{3, "高烧"} : e.v1 >= 373 ? HealthFlag{2, "低烧"} : e.v1 < 350 ? HealthFlag{1, "偏低"} : HealthFlag{0, ""};
    case HealthKind::Pressure:
        if (e.v1 >= 140 || e.v2 >= 90) return {3, "偏高"};
        if (e.v1 >= 130 || e.v2 >= 85) return {2, "略高"};
        return e.v1 < 90 || e.v2 < 60 ? HealthFlag{1, "偏低"} : HealthFlag{0, ""};
    default: return {0, ""};
    }
}
// The value as the page and the CSV show it: "62.5", "6.2", "36.6", "118/76", "3".
inline void healthValue(const HealthEntry& e, char* out, size_t size) {
    switch (HealthKind(e.kind)) {
    case HealthKind::Pressure: snprintf(out, size, "%d/%d", e.v1, e.v2); break;
    case HealthKind::Water: snprintf(out, size, "%d", e.v1); break;
    case HealthKind::Period: snprintf(out, size, "%s", e.tag ? "结束" : "开始"); break;
    case HealthKind::Intimacy: snprintf(out, size, "%s", e.tag ? kProtectNames[e.tag % 3] : "已记录"); break;
    default: snprintf(out, size, "%d.%d", e.v1 / 10, e.v1 % 10);
    }
}
inline const char* healthUnit(HealthKind kind) {
    static const char* const units[]{"杯", "kg", "mmol/L", "℃", "mmHg", "", ""};
    return units[unsigned(kind)];
}
// Up to `most` records the computer has not got yet, as JSON: {"entries":[{"id":..,"rev":..,"at":..,"kind":"glucose",
// "v":[62,0,0],"tag":"空腹","flag":"偏高"} or {"id":..,"rev":..,"del":1}],"left":N}.
inline void healthJson(const HealthBook& b, char* out, size_t size, unsigned most) {
    size_t used = snprintf(out, size, "{\"entries\":[");unsigned sent = 0;
    for (unsigned i = 0; i < b.count && sent < most; ++i) {
        const auto& e = b.items[i];
        if (e.flags & HealthBook::kSynced) continue;
        char one[200];
        if (e.flags & HealthBook::kDeleted) snprintf(one, sizeof(one), "%s{\"id\":%lu,\"rev\":%u,\"del\":1}", sent ? "," : "", (unsigned long)e.id, e.rev);
        else {
            const auto kind = HealthKind(e.kind);
            const char* tag = kind == HealthKind::Glucose ? kGlucoseTags[e.tag % kGlucoseTagCount] : kind == HealthKind::Period ? (e.tag ? "结束" : "开始") : kind == HealthKind::Intimacy ? kProtectNames[e.tag % 3] : "";
            snprintf(one, sizeof(one), "%s{\"id\":%lu,\"rev\":%u,\"at\":%lu,\"kind\":\"%s\",\"v\":[%d,%d,%d],\"tag\":\"%s\",\"flag\":\"%s\"}", sent ? "," : "",
                     (unsigned long)e.id, e.rev, (unsigned long)e.at, kHealthIds[e.kind], e.v1, e.v2, e.v3, tag, healthFlag(e).text);
        }
        if (used + strlen(one) + 24 >= size) break;
        used += snprintf(out + used, size - used, "%s", one); ++sent;
    }
    snprintf(out + used, size - used, "],\"left\":%u}", b.pending() - sent);
}

// 经期: in a period (day N of it), or the next one in nextIn days (negative: late), from the average cycle and length
// of the records kept (28 and 5 days until there are some).
// fertile: inside the usual fertile window (ovulation about 14 days before the next one, five days before it to one
// after); ovulationIn: days to that ovulation (negative: past). An estimate, never contraception.
struct PeriodState { bool known{}, active{}, fertile{}; int day{}, nextIn{}, cycle{28}, length{5}, ovulationIn{}; };
inline PeriodState periodState(const HealthBook& b, time_t now) {
    PeriodState s; int16_t starts[HealthBook::kMax], ends[HealthBook::kMax]; unsigned ns = 0, ne = 0;
    for (unsigned i = 0; i < b.count; ++i) {
        const auto& e = b.items[i]; if (e.kind != uint8_t(HealthKind::Period) || !b.live(i)) continue;
        (e.tag ? ends[ne++] : starts[ns++]) = int16_t(healthDay(e.at) - 20000);  // days since 2024-10
    }
    if (!ns) return s;
    std::sort(starts, starts + ns); std::sort(ends, ends + ne);
    int cycles = 0, cycleSum = 0, lengths = 0, lengthSum = 0;
    for (unsigned i = ns; i-- > 1 && cycles < 3;) if (const int c = starts[i] - starts[i - 1]; c >= 15 && c <= 60) { cycleSum += c; ++cycles; }
    for (unsigned i = 0; i < ns; ++i) for (unsigned j = 0; j < ne; ++j) if (ends[j] >= starts[i] && ends[j] - starts[i] < 15) { lengthSum += ends[j] - starts[i] + 1; ++lengths; break; }
    if (cycles) s.cycle = (cycleSum + cycles / 2) / cycles;
    if (lengths) s.length = (lengthSum + lengths / 2) / lengths;
    const int today = healthDay(now) - 20000, last = starts[ns - 1];
    bool ended = false; for (unsigned j = 0; j < ne; ++j) ended |= ends[j] >= last;
    s.known = true; s.active = !ended && today >= last && today - last < 10; s.day = today - last + 1;
    s.nextIn = last + s.cycle - today;
    s.ovulationIn = s.nextIn - 14; s.fertile = !s.active && s.ovulationIn <= 5 && s.ovulationIn >= -1;
    return s;
}

// 喝水提醒: every kWaterEvery minutes inside the day's window, from the morning's start.
struct HealthPrefs {
    static constexpr uint8_t kVersion = 1;
    // period: 经期 and 亲密 are shown; periodVoice: they may be spoken too (else keys only).
    uint8_t version{kVersion}, water{3}, window{1}, cup{1}, goal{1}, period{}, periodVoice{1}, pad{};
    bool valid() const { return version == kVersion && water < 6 && window < 3 && cup < 5 && goal < 4 && period < 2 && periodVoice < 2; }
};
inline constexpr uint8_t kWaterEvery[]{0, 30, 45, 60, 90, 120};  // minutes, 0: off
inline constexpr uint8_t kWaterWindows[][2]{{9, 18}, {9, 21}, {8, 22}};
inline constexpr uint16_t kCupMl[]{200, 250, 300, 350, 500};
inline constexpr uint8_t kWaterGoals[]{6, 8, 10, 12};
// The next reminder after `from` (0: off).
inline time_t waterNext(const HealthPrefs& p, time_t from) {
    const unsigned every = kWaterEvery[p.water % 6]; if (!every) return 0;
    const auto& w = kWaterWindows[p.window % 3];
    time_t at = from + time_t(every) * 60; tm t{}; localtime_r(&at, &t);
    if (t.tm_hour >= w[0] && t.tm_hour < w[1]) return at;
    if (t.tm_hour >= w[1]) ++t.tm_mday;
    t.tm_hour = w[0]; t.tm_min = t.tm_sec = 0; t.tm_isdst = -1; return mktime(&t);
}

// What was said, for a record of kind `hint` (the row it was said on; words in it can name another kind).
// daysAgo: 经期's day ("昨天来的"). error: why nothing came of it. lastWeight (kg x10, 0: none) settles a bare "125":
// 斤 or kg, whichever is nearer the last weight; with none, 100 and up is 斤 (people say 斤 more than they weigh 100 kg).
struct HealthParse { bool ok{}; HealthKind kind{}; uint8_t tag{}; int16_t v1{}, v2{}, v3{}; uint8_t daysAgo{}; bool converted{}; const char* error{}; };
inline HealthParse parseHealth(const char* heard, HealthKind hint, time_t now, int lastWeight = 0) {
    using namespace ledger_text;
    const std::string s(heard ? heard : "");
    const auto has = [&](std::initializer_list<const char*> words) { for (const char* w : words) if (s.find(w) != std::string::npos) return true; return false; };
    HealthParse r; r.kind = hint;
    if (has({"同房", "亲密", "爱爱", "性生活", "啪啪"})) r.kind = HealthKind::Intimacy;
    else if (has({"月经", "姨妈", "例假", "经期"})) r.kind = HealthKind::Period;
    else if (has({"血压", "高压", "低压", "收缩压", "舒张压", "心率", "脉搏", "/"})) r.kind = HealthKind::Pressure;  // "120/80"
    else if (has({"血糖"})) r.kind = HealthKind::Glucose;
    else if (has({"体温", "发烧", "华氏", "度"})) r.kind = HealthKind::Temp;  // "三十七度八"
    else if (has({"体重", "公斤", "千克", "斤", "kg", "KG"})) r.kind = HealthKind::Weight;
    // Every number in order (value x100), with what follows it; counts ("两小时", "3号", "7点") are not readings.
    struct Number { int64_t v; size_t from, to; };
    Number numbers[8]; unsigned found = 0;
    static const char* const counts[]{"小时", "分钟", "分", "点", "号", "月", "日", "天", "岁", "次", "个", "杯", "年", "周", "秒"};
    for (size_t i = 0; i < s.size() && found < 8;) {
        int64_t v; bool chinese; unsigned terms;
        const size_t n = number(s, i, v, chinese, terms);
        if (!n) { i += width(s, i); continue; }
        size_t k = i + n;
        // "37度5", "三十七度五": tenths after 度.
        if (at(s, k, "度")) { k += 3; if (const int d = digit(s, k); d >= 0) { v += d * 10; k += width(s, k); } }
        bool counted = false; for (const char* c : counts) counted |= at(s, k, c);
        if (!counted) numbers[found++] = {v, i, k};
        i = k;
    }
    const auto tenths = [](int64_t v) { return int16_t((v + 5) / 10); };
    // The day said (昨天, 前天, N 天前, M月D号, D号; always the past), as days ago; -1: more than 90 days back.
    const auto daysAgo = [&]() {
        int ago = has({"大前天"}) ? 3 : has({"前天"}) ? 2 : has({"昨天", "昨晚", "昨日"}) ? 1 : 0;
        int month = 0, day = 0;
        for (size_t i = 0; i < s.size();) {
            int64_t v; bool chinese; unsigned terms; const size_t n = number(s, i, v, chinese, terms);
            if (!n) { i += width(s, i); continue; }
            const size_t k = i + n; const int value = int(v / 100);
            if (at(s, k, "天前")) ago = value;
            else if (at(s, k, "月")) month = value;
            else if (at(s, k, "号") || at(s, k, "日")) day = value;
            i = k;
        }
        if (day >= 1 && day <= 31) {
            tm t{}; localtime_r(&now, &t); tm then = t;
            if (month >= 1 && month <= 12) then.tm_mon = month - 1;
            then.tm_mday = day; then.tm_hour = 12; then.tm_min = then.tm_sec = 0; then.tm_isdst = -1;
            time_t when = mktime(&then);
            if (healthDay(when) > healthDay(now)) { if (month) --then.tm_year; else --then.tm_mon; then.tm_mday = day; then.tm_isdst = -1; when = mktime(&then); }
            ago = healthDay(now) - healthDay(when);
        }
        return ago < 0 || ago > 90 ? -1 : ago;
    };
    switch (r.kind) {
    case HealthKind::Weight: {
        int pick = -1; bool jin = false, kg = false;
        for (unsigned i = 0; i < found && pick < 0; ++i) {
            const size_t k = numbers[i].to;
            if (at(s, k, "斤")) { pick = int(i); jin = true; } else if (at(s, k, "公斤") || at(s, k, "千克") || at(s, k, "kg") || at(s, k, "KG")) { pick = int(i); kg = true; }
        }
        if (pick < 0 && found) pick = 0;
        if (pick < 0) { r.error = "没听到体重"; return r; }
        int64_t v = numbers[pick].v;
        if (!jin && !kg) jin = lastWeight ? std::abs(v / 20 - lastWeight) < std::abs(v / 10 - lastWeight) : v >= 10000;
        if (jin) { v /= 2; r.converted = true; }
        if (v < 2000 || v > 30000) { r.error = "体重不在 20-300 公斤之间"; return r; }
        r.v1 = tenths(v); break;
    }
    case HealthKind::Glucose: {
        if (!found) { r.error = "没听到血糖值"; return r; }
        int64_t v = numbers[0].v;
        if (v > 3500) { v = (v + 9) / 18; r.converted = true; }  // mg/dL
        if (v < 100 || v > 3500) { r.error = "血糖不在 1-35 mmol/L 之间"; return r; }
        r.v1 = tenths(v);
        tm t{}; localtime_r(&now, &t);
        const int meal = t.tm_hour < 11 ? 2 : t.tm_hour < 16 ? 3 : 4;
        if (has({"空腹"})) r.tag = 1;
        else if (has({"早餐后", "早饭后"})) r.tag = 2;
        else if (has({"午餐后", "午饭后", "中饭后", "中餐后"})) r.tag = 3;
        else if (has({"晚餐后", "晚饭后"})) r.tag = 4;
        else if (has({"餐后", "饭后"})) r.tag = uint8_t(meal);
        else if (has({"睡前"})) r.tag = 5;
        else if (has({"随机"})) r.tag = 0;
        else r.tag = t.tm_hour < 9 ? 1 : 0;  // first thing in the morning is most often 空腹; a Confirm changes it
        break;
    }
    case HealthKind::Temp: {
        int pick = -1;
        for (unsigned i = 0; i < found && pick < 0; ++i) if ((numbers[i].v >= 3400 && numbers[i].v <= 4300) || (numbers[i].v >= 9300 && numbers[i].v <= 11000)) pick = int(i);
        if (pick < 0) { r.error = found ? "体温不在 34-43℃ 之间" : "没听到体温"; return r; }
        int64_t v = numbers[pick].v;
        if (v >= 9300) { v = (v - 3200) * 5 / 9; r.converted = true; }
        r.v1 = tenths(v); break;
    }
    case HealthKind::Pressure: {
        if (found < 2) { r.error = "请说两个数，如 120 80"; return r; }
        int64_t a = numbers[0].v / 100, b = numbers[1].v / 100;
        if (a < b) std::swap(a, b);
        if (a < 60 || a > 260 || b < 30 || b > 160) { r.error = "血压数值不对，请再说一次"; return r; }
        r.v1 = int16_t(a); r.v2 = int16_t(b);
        if (found >= 3 && numbers[2].v >= 3000 && numbers[2].v <= 22000) r.v3 = int16_t(numbers[2].v / 100);
        break;
    }
    case HealthKind::Period: {
        if (has({"结束", "走了", "干净", "完了", "没了"})) r.tag = 1;
        else if (has({"来", "开始", "第一天"})) r.tag = 0;  // 来了, 来的, 来月经
        else { r.error = "请说「月经来了」或「月经结束了」"; return r; }
        if (has({"量多", "很多", "特别多", "量大", "比较多"})) r.v1 = 3;
        else if (has({"量少", "很少", "量小", "比较少"})) r.v1 = 1;
        else if (has({"中等", "正常", "一般", "量中"})) r.v1 = 2;
        const int ago = daysAgo();
        if (ago < 0) { r.error = "只能记 90 天内的日子"; return r; }
        r.daysAgo = uint8_t(ago); break;
    }
    case HealthKind::Intimacy: {
        if (has({"无保护", "没戴", "没有保护", "没采取", "没避孕", "没有避孕", "体外"})) r.tag = 2;
        else if (has({"有保护", "戴套", "避孕套", "安全套", "避孕"})) r.tag = 1;
        const int ago = daysAgo();
        if (ago < 0) { r.error = "只能记 90 天内的日子"; return r; }
        r.daysAgo = uint8_t(ago); break;
    }
    case HealthKind::Water: default:
        r.error = "喝水按确认记一杯"; return r;
    }
    r.ok = true; return r;
}
}
