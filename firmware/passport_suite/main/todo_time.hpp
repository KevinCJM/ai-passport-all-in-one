#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iterator>
#include <string>
// 待办提醒: the time said in a spoken todo, worked out on the badge, and the todo without it.
//   "20分钟后提醒我拿外卖" -> in 20 minutes, "拿外卖"; "明天下午3点在会议室开会" -> tomorrow 15:00; "周五交周报" -> Friday 9:00.
// Understood: N 分钟/小时/天 后 (also 半小时, 一个半小时, 一刻钟); 今天/明天/后天/大后天/今晚/明早/明晚; 周/星期/礼拜 X and
// 下周 X; M 月 D 日/号 and D 号; 凌晨/早上/上午/中午/下午/傍晚/晚上/夜里; H 点 (半, 一刻, 三刻, M 分) and H:MM.
// Chinese numerals count as a clock time only next to a day or a part of the day ("多喝一点水" is not 1 o'clock).
namespace passport {
struct TodoParse { std::string text; time_t at{}; };  // at 0: no time was said

namespace todo_time {
inline bool at(const std::string& s, size_t i, const char* word) { return s.compare(i, strlen(word), word) == 0; }
// A number at i: Arabic digits, or Chinese numerals up to 99 (两 = 2). Returns its bytes, 0 if none; arabic tells which.
inline size_t number(const std::string& s, size_t i, int& value, bool& arabic) {
    size_t n = i; value = 0;
    while (n < s.size() && s[n] >= '0' && s[n] <= '9' && n - i < 4) value = value * 10 + (s[n++] - '0');
    if (n > i) { arabic = true; return n - i; }
    arabic = false;
    static const char* const digits[]{"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
    const auto digit = [&](size_t k, int& d) {
        if (at(s, k, "两")) { d = 2; return true; }
        if (at(s, k, "〇")) { d = 0; return true; }
        for (int v = 0; v < 10; ++v) if (at(s, k, digits[v])) { d = v; return true; }
        return false;
    };
    int d = 0;
    if (at(s, n, "十")) { value = 10; n += 3; if (digit(n, d) && d) { value += d; n += 3; } return n - i; }
    if (!digit(n, d)) return 0;
    value = d; n += 3;
    if (at(s, n, "十")) { value = d * 10; n += 3; if (digit(n, d) && d) { value += d; n += 3; } }
    return n - i;
}
}

// When a reminder is, the way the list shows it: "今天 15:00", "明天 09:00", "周五 09:00", "11月3日 09:00".
inline void todoWhen(time_t at, time_t now, char* out, size_t size) {
    tm a{}, n{}; localtime_r(&at, &a); localtime_r(&now, &n);
    tm day = n; day.tm_hour = 12; day.tm_min = day.tm_sec = 0; day.tm_isdst = -1;
    tm target = a; target.tm_hour = 12; target.tm_min = target.tm_sec = 0; target.tm_isdst = -1;
    const long days = std::lround(difftime(mktime(&target), mktime(&day)) / 86400);
    static const char* const week[]{"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    if (days == 0) snprintf(out, size, "今天 %02d:%02d", a.tm_hour, a.tm_min);
    else if (days == 1) snprintf(out, size, "明天 %02d:%02d", a.tm_hour, a.tm_min);
    else if (days == -1) snprintf(out, size, "昨天 %02d:%02d", a.tm_hour, a.tm_min);
    else if (days > 1 && days < 7) snprintf(out, size, "%s %02d:%02d", week[a.tm_wday], a.tm_hour, a.tm_min);
    else snprintf(out, size, "%d月%d日 %02d:%02d", a.tm_mon + 1, a.tm_mday, a.tm_hour, a.tm_min);
}
inline TodoParse parseTodo(const char* heard, time_t now) {
    using namespace todo_time;
    std::string s(heard);
    std::string keep(s.size(), 1);  // 1: part of the todo's text, 0: part of a time
    const auto cut = [&](size_t from, size_t to) { for (size_t k = from; k < to && k < keep.size(); ++k) keep[k] = 0; };
    long relative = -1;                  // seconds from now
    int day = -1, weekday = -1, nextWeek = 0, month = -1, date = -1, hour = -1, minute = 0, period = -1;
    bool chineseHour = false; size_t hourFrom = 0, hourTo = 0;
    // Parts of the day: their default hour, and whether a clock time in them is afternoon.
    struct Period { const char* word; int hour; bool pm; };
    static const Period periods[]{{"凌晨", 6, false}, {"早上", 8, false}, {"早晨", 8, false}, {"上午", 9, false}, {"中午", 12, false},
        {"下午", 15, true}, {"傍晚", 18, true}, {"晚上", 20, true}, {"夜里", 22, true}};
    struct Day { const char* word; int offset; int period; };
    static const Day days[]{{"大后天", 3, -1}, {"后天", 2, -1}, {"明天", 1, -1}, {"明早", 1, 1}, {"明晚", 1, 7}, {"今天", 0, -1},
        {"今早", 0, 1}, {"今晚", 0, 7}};
    for (size_t i = 0; i < s.size();) {
        int value; bool arabic; size_t n;
        // N 分钟/小时/天 后, 半小时后, 一个半小时后, 一刻钟后.
        {
            size_t k = i; long seconds = -1;
            if (at(s, k, "一刻钟")) { seconds = 900; k += 9; }
            else if (at(s, k, "半个小时") || at(s, k, "半小时")) { seconds = 1800; k += at(s, k, "半个") ? 12 : 9; }
            else if ((n = number(s, k, value, arabic))) {
                k += n; if (at(s, k, "个")) k += 3;
                const bool half = at(s, k, "半"); if (half) k += 3;
                if (at(s, k, "分钟")) { seconds = value * 60L; k += 6; }
                else if (at(s, k, "小时") || at(s, k, "钟头")) { seconds = value * 3600L + (half ? 1800 : 0); k += 6; }
                else if (at(s, k, "天")) { seconds = value * 86400L; k += 3; }
            }
            if (seconds >= 0) for (const char* after : {"以后", "之后", "后"}) if (at(s, k, after)) {
                relative = seconds; cut(i, k + strlen(after)); i = k + strlen(after); goto next;
            }
        }
        for (const auto& d : days) if (at(s, i, d.word)) { day = d.offset; if (d.period >= 0) period = d.period; cut(i, i + strlen(d.word)); i += strlen(d.word); goto next; }
        for (int p = 0; p < int(std::size(periods)); ++p) if (at(s, i, periods[p].word)) { period = p; cut(i, i + strlen(periods[p].word)); i += strlen(periods[p].word); goto next; }
        // 周X / 星期X / 礼拜X, after an optional 下 (next week).
        {
            size_t k = i; int later = 0;
            for (const char* w : {"下个", "下"}) if (at(s, k, w) && (at(s, k + strlen(w), "周") || at(s, k + strlen(w), "星期") || at(s, k + strlen(w), "礼拜"))) { k += strlen(w); later = 1; break; }
            for (const char* w : {"星期", "礼拜", "周"}) if (at(s, k, w)) {
                k += strlen(w); int dayOf = -1;
                if (at(s, k, "日") || at(s, k, "天")) dayOf = 7;
                else if ((n = number(s, k, value, arabic)) && value >= 1 && value <= 7 && n <= 3) { dayOf = value; k += n - 3; }
                if (dayOf > 0) { weekday = dayOf % 7; nextWeek = later; cut(i, k + 3); i = k + 3; goto next; }
                break;
            }
        }
        if ((n = number(s, i, value, arabic))) {
            size_t k = i + n;
            // M 月 D 日/号, or D 号.
            int d2; bool a2; size_t m2;
            if (at(s, k, "月") && (m2 = number(s, k + 3, d2, a2)) && (at(s, k + 3 + m2, "日") || at(s, k + 3 + m2, "号")) && value >= 1 && value <= 12 && d2 >= 1 && d2 <= 31) {
                month = value; date = d2; cut(i, k + 6 + m2); i = k + 6 + m2; goto next;
            }
            if (at(s, k, "号") && value >= 1 && value <= 31) { date = value; cut(i, k + 3); i = k + 3; goto next; }
            // H 点 [半 / 一刻 / 三刻 / M 分], H 点钟, H:MM.
            if ((at(s, k, "点") || at(s, k, ":") || at(s, k, "：")) && value <= 24) {
                const bool colon = !at(s, k, "点"); k += at(s, k, ":") ? 1 : 3;
                int mins = 0;
                if (!colon && at(s, k, "钟")) k += 3;
                else if (!colon && at(s, k, "半")) { mins = 30; k += 3; }
                else if (!colon && at(s, k, "一刻")) { mins = 15; k += 6; }
                else if (!colon && at(s, k, "三刻")) { mins = 45; k += 6; }
                else if ((m2 = number(s, k, d2, a2)) && d2 < 60) { mins = d2; k += m2; if (at(s, k, "分")) k += 3; }
                else if (colon) goto plain;
                hour = value; minute = mins; chineseHour = !arabic; hourFrom = i; hourTo = k; cut(i, k); i = k; goto next;
            }
        plain:
            i += n; goto next;
        }
        i += (static_cast<unsigned char>(s[i]) < 0x80) ? 1 : (static_cast<unsigned char>(s[i]) < 0xe0 ? 2 : (static_cast<unsigned char>(s[i]) < 0xf0 ? 3 : 4));
    next:;
    }
    // A Chinese-numeral hour with no day or part of the day is probably not a time ("多喝一点水").
    if (hour >= 0 && chineseHour && day < 0 && period < 0 && weekday < 0 && date < 0) { for (size_t k = hourFrom; k < hourTo; ++k) keep[k] = 1; hour = -1; }

    TodoParse out;
    const bool timed = relative >= 0 || day >= 0 || weekday >= 0 || date >= 0 || hour >= 0 || period >= 0;
    if (relative >= 0) out.at = now + relative;  // to the second: rounding down would ring "1分钟后" seconds later
    else if (timed) {
        tm t{}; localtime_r(&now, &t);
        int h = hour, m = minute;
        if (h < 0) { h = period >= 0 ? periods[period].hour : 9; m = 0; }
        else if (period >= 0 && periods[period].pm && h < 12) h += 12;
        else if (period == 4 && h < 6) h += 12;  // 中午1点
        if (h == 24) h = 0;
        int offset = day >= 0 ? day : 0;
        if (weekday >= 0) {
            const int today = t.tm_wday;  // 0 = Sunday
            // This week's day (Monday first), or next week's after 下.
            const int mondayBased = (today + 6) % 7, target = (weekday + 6) % 7;
            offset = target - mondayBased + (nextWeek ? 7 : 0);
            if (!nextWeek && offset < 0) offset += 7;
        }
        tm when = t; when.tm_hour = h; when.tm_min = m; when.tm_sec = 0; when.tm_isdst = -1;
        if (date > 0) { when.tm_mday = date; if (month > 0) when.tm_mon = month - 1; }
        else when.tm_mday += offset;
        time_t result = mktime(&when);
        if (result <= now) {
            if (date > 0 && month > 0) { when.tm_year += 1; result = mktime(&when); }
            else if (date > 0) { when.tm_mon += 1; result = mktime(&when); }
            else if (weekday >= 0 && !nextWeek) { when.tm_mday += 7; result = mktime(&when); }
            else if (day < 0) {
                // "3点开会" said at 14:00 means 15:00 (up to 9 o'clock in the evening); "10点" means tomorrow morning.
                if (hour >= 0 && period < 0 && h >= 1 && h <= 9 && result + 12 * 3600 > now) result += 12 * 3600;
                else { when.tm_mday += 1; result = mktime(&when); }
            }
        }
        out.at = result;
    }
    // The todo: what is left, without the request words around the time.
    std::string text;
    for (size_t k = 0; k < s.size(); ++k) if (keep[k]) text += s[k];
    const auto trim = [&] {
        for (bool again = true; again;) {
            again = false;
            for (const char* w : {" ", "，", ",", "。", ".", "、", "！", "!", "？", "?", "：", ":", "提醒我", "叫我", "提醒", "记得", "的时候", "的"})
                if (text.compare(0, strlen(w), w) == 0) { text.erase(0, strlen(w)); again = true; }
            for (const char* w : {" ", "，", ",", "。", ".", "、", "！", "!", "～", "的时候"})
                if (text.size() >= strlen(w) && text.compare(text.size() - strlen(w), strlen(w), w) == 0) { text.erase(text.size() - strlen(w)); again = true; }
        }
    };
    trim();
    out.text = text;
    return out;
}
}  // namespace passport
