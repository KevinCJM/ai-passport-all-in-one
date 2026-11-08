#pragma once
#include "todo_time.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
// 高铁查询: a train by its number (12306's 小程序 timetable: every stop, with 检票口, 候车室 and 出站口 on the day),
// trips kept on the badge (train, day, where you get on and off), and 12306's 正晚点 for those two stations.
//   "G7" -> G7 today; "明天的 D701" -> D701 tomorrow; "鸡七" / "高铁七" -> G7; "K五九九 11月9号"; "动车七零一" -> D701.
// The day is the one the train leaves its first station (12306 asks for that). Station names outside GB2312, so the
// UI font has them too: 塱栟沚滘鄠鲊鲘.
namespace passport {
inline constexpr unsigned kTrainStops = 40, kTrainTrips = 5;
inline constexpr uint16_t kTrainNoTime = 0xffff;
inline constexpr int16_t kTrainNoLate = INT16_MIN;
inline constexpr uint8_t kTrainNone = 255;
// One stop: minutes from 00:00 of the train's first day (1440 and on: a later day); kTrainNoTime when not given.
struct TrainStop { char name[16]; uint16_t arrive, depart; };
// A trip: the train, its first day (yyyymmdd), where you get on and off (their index in the stops, and names), the
// times there (as TrainStop's), and the boarding station's 检票口 as last fetched.
struct TrainTrip {
    char code[8]; uint32_t date; uint8_t fromAt, toAt; char from[16], to[16]; uint16_t depart, arrive; char gate[48];
    bool valid() const {
        return code[0] && memchr(code, 0, sizeof(code)) && memchr(from, 0, sizeof(from)) && memchr(to, 0, sizeof(to)) && memchr(gate, 0, sizeof(gate))
            && date >= 20200101 && date <= 20991231 && fromAt < toAt && toAt < kTrainStops && depart != kTrainNoTime && arrive != kTrainNoTime && depart <= arrive;
    }
};
// The trip shown: page.train holds the train fetched (its stops), the stop `pick` whose 检票口 / 候车 / 出站 are
// below, and how late it leaves `from` and reaches `to` (kTrainNoLate: not known).
struct TrainView {
    char code[8]; uint32_t date; char style[16];
    uint8_t count, total, pick, from, to;  // count: stops held (0: not fetched yet); total: stops the train has
    int16_t late[2];
    char gate[64], wait[32], exit[48], toExit[48];
    TrainStop stops[kTrainStops];
    TrainTrip trip;  // the saved trip on show (Model::trainTrip), or what the page asks for (code, date)
};

namespace train {
inline uint32_t dateOf(time_t at) { tm t{}; localtime_r(&at, &t); return uint32_t((t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday); }
// Local time of `minutes` after 00:00 on day yyyymmdd (minutes past 1440 roll into the next days).
inline time_t epoch(uint32_t date, unsigned minutes) {
    tm t{}; t.tm_year = int(date / 10000) - 1900; t.tm_mon = int(date / 100 % 100) - 1; t.tm_mday = int(date % 100) + int(minutes / 1440);
    t.tm_hour = int(minutes % 1440 / 60); t.tm_min = int(minutes % 60); t.tm_isdst = -1; return mktime(&t);
}
inline uint32_t addDays(uint32_t date, int days) { return dateOf(epoch(date, 720) + days * 86400); }
// "0630" and the day it is on -> minutes; "--" or anything else: kTrainNoTime.
inline uint16_t minutes(const char* hhmm, int days) {
    if (!hhmm || strlen(hhmm) != 4 || days < 0 || days > 9) return kTrainNoTime;
    for (int i = 0; i < 4; ++i) if (hhmm[i] < '0' || hhmm[i] > '9') return kTrainNoTime;
    const int h = (hhmm[0] - '0') * 10 + hhmm[1] - '0', m = (hhmm[2] - '0') * 10 + hhmm[3] - '0';
    return h < 24 && m < 60 ? uint16_t(days * 1440 + h * 60 + m) : kTrainNoTime;
}
// 正晚点's answer ("预计G19次列车，济南西站到达时间为15:32") against the timetable's minutes: minutes late (negative:
// early), kTrainNoLate when it gives no time ("目前暂无…").
inline int16_t late(const char* message, unsigned scheduled) {
    const char* at = message ? strstr(message, "时间为") : nullptr;
    if (!at || scheduled == kTrainNoTime) return kTrainNoLate;
    at += strlen("时间为"); int h = 0, m = 0;
    if (sscanf(at, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return kTrainNoLate;
    int d = h * 60 + m - int(scheduled % 1440);
    if (d < -720) d += 1440; else if (d > 720) d -= 1440;
    return int16_t(d);
}

struct Ask { bool ok{}; char code[8]{}; uint32_t date{}; const char* error{}; };
// A train number said aloud, and the day (today unless one is said).
inline Ask parse(const char* heard, time_t now) {
    Ask r; std::string s;
    for (const char* c = heard ? heard : ""; *c; ++c) { const char ch = *c >= 'a' && *c <= 'z' ? char(*c - 32) : *c; if (!strchr(" ,.;:!?\t\n-", ch)) s += ch; }
    using todo_time::at;
    // Prefixes: the letter itself, the kind of train, or what the cloud writes for a spoken letter.
    struct Prefix { const char* word; char letter; };
    static constexpr Prefix kPrefixes[]{
        {"高铁", 'G'}, {"动车", 'D'}, {"城际", 'C'}, {"直达", 'Z'}, {"特快", 'T'}, {"快速", 'K'}, {"市域", 'S'}, {"市郊", 'S'},
        {"鸡", 'G'}, {"机", 'G'}, {"吉", 'G'}, {"基", 'G'}, {"及", 'G'}, {"级", 'G'}, {"极", 'G'}, {"寄", 'G'}, {"记", 'G'},
        {"迪", 'D'}, {"地", 'D'}, {"弟", 'D'}, {"帝", 'D'}, {"低", 'D'}, {"滴", 'D'}, {"西", 'C'}, {"希", 'C'}, {"吸", 'C'},
        {"开", 'K'}, {"凯", 'K'}, {"卡", 'K'}, {"贼", 'Z'}, {"资", 'Z'}, {"紫", 'Z'}, {"替", 'T'}, {"踢", 'T'}, {"提", 'T'}, {"梯", 'T'}};
    static const char* const kDigits[]{"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
    // A number at i (Arabic, read digit by digit "七零一", or counted "七百零一"); its bytes, 0 if none.
    const auto number = [&](size_t i, int& value) -> size_t {
        size_t n = i; value = 0;
        while (n < s.size() && s[n] >= '0' && s[n] <= '9' && n - i < 4) value = value * 10 + (s[n++] - '0');
        if (n > i) return n - i;
        const auto digit = [&](size_t k, int& d) {
            if (at(s, k, "两")) { d = 2; return true; } if (at(s, k, "幺")) { d = 1; return true; }
            if (at(s, k, "〇") || at(s, k, "洞")) { d = 0; return true; }
            for (int v = 0; v < 10; ++v) if (at(s, k, kDigits[v])) { d = v; return true; }
            return false;
        };
        // Digits alone are read one by one; with 十/百/千 they count (零 just holds a place).
        int sequence = 0, total = 0, last = 0, d = 0; unsigned digits = 0; bool counted = false;
        while (n < s.size()) {
            if (digit(n, d)) { sequence = sequence * 10 + d; last = d; ++digits; n += 3; continue; }
            const int unit = at(s, n, "千") ? 1000 : at(s, n, "百") ? 100 : at(s, n, "十") ? 10 : 0;
            if (!unit) break;
            total += (digits ? last : 1) * unit; last = 0; counted = true; ++digits; n += 3;
        }
        if (!digits || (!counted && digits > 4)) return 0;
        value = counted ? total + last : sequence;
        return value > 0 && value < 10000 ? n - i : 0;
    };
    size_t from = std::string::npos, to = 0; int value = 0; char letter = 0;
    for (size_t i = 0; i < s.size() && !letter; ) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (strchr("GDCKZTSYL", char(c)) && c) { if (const size_t n = number(i + 1, value)) { letter = char(c); from = i; to = i + 1 + n; break; } ++i; continue; }
        bool found = false;
        for (const auto& p : kPrefixes) if (at(s, i, p.word)) {
            size_t k = i + strlen(p.word);
            if (at(s, k, "G") || at(s, k, "D") || at(s, k, "C") || at(s, k, "K") || at(s, k, "Z") || at(s, k, "T")) { letter = s[k]; ++k; }
            if (const size_t n = number(k, value)) { if (!letter) letter = p.letter; from = i; to = k + n; found = true; break; }
            letter = 0;
        }
        if (found) break;
        i += c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    }
    // 普速 trains have no letter: a three or four digit number not followed by a date's or time's unit.
    if (!letter) for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9' || (i && s[i - 1] >= '0' && s[i - 1] <= '9')) continue;
        size_t n = i; while (n < s.size() && s[n] >= '0' && s[n] <= '9') ++n;
        if (n - i >= 3 && n - i <= 4 && !at(s, n, "月") && !at(s, n, "号") && !at(s, n, "日") && !at(s, n, "点") && !at(s, n, "年")) { value = atoi(s.substr(i, n - i).c_str()); from = i; to = n; break; }
    }
    if (from == std::string::npos) { r.error = "没听出车次，例：G7、D701"; return r; }
    if (letter) snprintf(r.code, sizeof(r.code), "%c%d", letter, value); else snprintf(r.code, sizeof(r.code), "%d", value);
    std::string rest = s.substr(0, from) + s.substr(to);
    if (at(rest, 0, "次")) rest.erase(0, 3);
    const auto said = parseTodo(rest.c_str(), now);
    r.date = dateOf(said.at ? said.at : now); r.ok = true;
    return r;
}

// The saved trips, the next one first; trips that ended over two hours ago are dropped.
struct Trips {
    uint8_t version{1}, count{}; TrainTrip items[kTrainTrips]{};
    bool valid() const { if (version != 1 || count > kTrainTrips) return false; for (unsigned i = 0; i < count; ++i) if (!items[i].valid()) return false; return true; }
    time_t leaves(unsigned i) const { return epoch(items[i].date, items[i].depart); }
    void sort() { std::sort(items, items + count, [](const TrainTrip& a, const TrainTrip& b) { return epoch(a.date, a.depart) < epoch(b.date, b.depart); }); }
    bool prune(time_t now) {
        unsigned n = 0; for (unsigned i = 0; i < count; ++i) if (epoch(items[i].date, items[i].arrive) + 7200 > now) items[n++] = items[i];
        const bool changed = n != count; count = uint8_t(n); return changed;
    }
    // Index of the trip with this train and day, or -1.
    int find(const char* code, uint32_t date) const { for (unsigned i = 0; i < count; ++i) if (items[i].date == date && !strcmp(items[i].code, code)) return int(i); return -1; }
    // Saves a trip (the same train and day replaces the old one); -1 when full. Returns its index once sorted.
    int put(const TrainTrip& trip) {
        int at = find(trip.code, trip.date);
        if (at < 0) { if (count == kTrainTrips) return -1; at = count++; }
        items[at] = trip; sort(); return find(trip.code, trip.date);
    }
    void remove(unsigned i) { if (i >= count) return; std::move(items + i + 1, items + count, items + i); --count; }
    // The trip for the home page: the first one leaving today that has not left more than ten minutes ago.
    int today(time_t now) const {
        const uint32_t day = dateOf(now);
        for (unsigned i = 0; i < count; ++i) if (dateOf(leaves(i)) == day && leaves(i) + 600 > now) return int(i);
        return -1;
    }
};
}
using TrainTrips = train::Trips;
}
