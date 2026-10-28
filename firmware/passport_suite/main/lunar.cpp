#include "lunar.hpp"
#include "calendar_data.hpp"
#include <cstring>
#include <iterator>

namespace passport {
namespace {
// Lunar 1900-01-01 was solar 1900-01-31.
const int kLunarEpoch = dayNumber({1900, 1, 31});
int leapMonth(int year) { return kLunarYears[year - kLunarFirstYear] & 0xf; }
int yearDays(int year) {
    const auto info = kLunarYears[year - kLunarFirstYear];
    int days = 12 * 29 + (leapMonth(year) ? 29 + (info >> 16 & 1) : 0);
    for (int month = 1; month <= 12; ++month) days += info >> (16 - month) & 1;
    return days;
}
}
// Howard Hinnant's days_from_civil / civil_from_days.
int dayNumber(Date date) {
    const int y = date.year - (date.month <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    const int doy = (153 * (date.month + (date.month > 2 ? -3 : 9)) + 2) / 5 + date.day - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
Date fromDayNumber(int days) {
    days += 719468;
    const int era = (days >= 0 ? days : days - 146096) / 146097, doe = days - era * 146097;
    const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int mp = (5 * doy + 2) / 153, day = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9;
    return {yoe + era * 400 + (month <= 2), month, day};
}
int weekday(Date date) { return (dayNumber(date) % 7 + 11) % 7; }  // 1970-01-01 was a Thursday
int monthDays(int year, int month) {
    static const int days[]{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && year % 4 == 0 && (year % 100 || year % 400 == 0) ? 29 : days[month - 1];
}
int lunarMonthDays(int year, int month, bool leap) {
    const auto info = kLunarYears[year - kLunarFirstYear];
    return 29 + (leap ? info >> 16 & 1 : info >> (16 - month) & 1);
}
bool toLunar(Date date, LunarDate& out) {
    int offset = dayNumber(date) - kLunarEpoch, year = kLunarFirstYear;
    if (offset < 0) return false;
    for (; year <= kLunarLastYear && offset >= yearDays(year); ++year) offset -= yearDays(year);
    if (year > kLunarLastYear) return false;
    const int leap = leapMonth(year);
    for (int month = 1; month <= 12; ++month) {
        for (bool isLeap : {false, true}) {
            if (isLeap && month != leap) continue;
            const int days = lunarMonthDays(year, month, isLeap);
            if (offset < days) { out = {year, month, offset + 1, isLeap}; return true; }
            offset -= days;
        }
    }
    return false;
}
bool fromLunar(int year, int month, int day, Date& out) {
    if (year < kLunarFirstYear || year > kLunarLastYear || month < 1 || month > 12 || day < 1 || day > lunarMonthDays(year, month, false)) return false;
    int offset = 0;
    for (int y = kLunarFirstYear; y < year; ++y) offset += yearDays(y);
    for (int m = 1; m < month; ++m) offset += lunarMonthDays(year, m, false) + (m == leapMonth(year) ? lunarMonthDays(year, m, true) : 0);
    out = fromDayNumber(kLunarEpoch + offset + day - 1);
    return true;
}
int solarTermDay(int year, int index) {
    return year < kLunarFirstYear || year > kLunarLastYear || index < 0 || index > 23 ? 0 : kSolarTerms[year - kLunarFirstYear][index];
}
const char* const kSolarTermNames[24]{"小寒", "大寒", "立春", "雨水", "惊蛰", "春分", "清明", "谷雨", "立夏", "小满", "芒种", "夏至",
                                      "小暑", "大暑", "立秋", "处暑", "白露", "秋分", "寒露", "霜降", "立冬", "小雪", "大雪", "冬至"};
bool solarTermsAround(Date date, int& last, Date& lastDate, int& next, Date& nextDate) {
    // Terms numbered on from year 0: term n is index n % 24 of year n / 24, in month n % 24 / 2 + 1.
    const auto at = [](int n) { return Date{n / 24, n % 24 / 2 + 1, solarTermDay(n / 24, n % 24)}; };
    int n = date.year * 24 + (date.month - 1) * 2 + 1;
    while (at(n).day && dayNumber(at(n)) > dayNumber(date)) --n;
    if (!at(n).day || !at(n + 1).day) return false;
    last = n % 24; lastDate = at(n); next = (n + 1) % 24; nextDate = at(n + 1);
    return true;
}
const char* lunarMonthName(int month) {
    static const char* const names[]{"正月", "二月", "三月", "四月", "五月", "六月", "七月", "八月", "九月", "十月", "冬月", "腊月"};
    return month >= 1 && month <= 12 ? names[month - 1] : "";
}
const char* lunarDayName(int day) {
    static const char* const names[]{"初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
                                     "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
                                     "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十"};
    return day >= 1 && day <= 30 ? names[day - 1] : "";
}
void ganzhiName(int index, char out[7]) {
    static const char* const stems[]{"甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸"};
    static const char* const branches[]{"子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥"};
    strcpy(out, stems[index % 10]); strcat(out, branches[index % 12]);
}
void ganzhi(int lunarYear, char out[7]) { ganzhiName(((lunarYear - 4) % 60 + 60) % 60, out); }
const char* zodiacOfBranch(int branch) {
    static const char* const animals[]{"鼠", "牛", "虎", "兔", "龙", "蛇", "马", "羊", "猴", "鸡", "狗", "猪"};
    return animals[(branch % 12 + 12) % 12];
}
const char* zodiac(int lunarYear) { return zodiacOfBranch(lunarYear - 4); }
const char* almanacWord(int index) { return index >= 0 && index < static_cast<int>(std::size(kAlmanacWords)) ? kAlmanacWords[index] : ""; }
bool almanac(Date date, Almanac& out) {
    if (date.year <= kLunarFirstYear || date.year >= kLunarLastYear) return false;
    const auto cycle = [](int stem, int branch) { return ((6 * stem - 5 * branch) % 60 + 60) % 60; };
    // Whole days, as lunar_python's default: the 节 day already belongs to the new month (立春 to the new year).
    const int year = date.year - (date.month < 2 || (date.month == 2 && date.day < solarTermDay(date.year, 2)));  // 立春
    const int branch = (date.month - (date.day < solarTermDay(date.year, (date.month - 1) * 2))) % 12;          // this month's 节
    out.year = ((year - 4) % 60 + 60) % 60;
    out.month = cycle((out.year % 10 % 5 * 2 + 2 + (branch + 10) % 12) % 10, branch);  // 五虎遁: 寅 month stem from the year stem
    out.day = ((dayNumber(date) + 17) % 60 + 60) % 60;                                // 1970-01-01 was 辛巳
    out.chong = cycle((out.day % 10 + 4) % 10, (out.day % 12 + 6) % 12);
    static const char* const sha[]{"南", "东", "北", "西"};
    out.sha = sha[out.day % 12 % 4];
    const uint8_t* entry = kAlmanacData + kAlmanacOffsets[branch * 60 + out.day];
    out.yiCount = *entry++; memcpy(out.yi, entry, out.yiCount); entry += out.yiCount;
    out.jiCount = *entry++; memcpy(out.ji, entry, out.jiCount);
    return true;
}
const char* festival(Date date, const LunarDate& lunar) {
    struct Day { int month, day; const char* name; };
    static const Day solar[]{{1, 1, "元旦"}, {2, 14, "情人节"}, {3, 8, "妇女节"}, {5, 1, "劳动节"}, {5, 4, "青年节"},
                             {6, 1, "儿童节"}, {7, 1, "建党节"}, {8, 1, "建军节"}, {9, 10, "教师节"}, {10, 1, "国庆"}, {12, 25, "圣诞"}};
    static const Day lunarDays[]{{1, 1, "春节"}, {1, 15, "元宵"}, {5, 5, "端午"}, {7, 7, "七夕"}, {7, 15, "中元"},
                                 {8, 15, "中秋"}, {9, 9, "重阳"}, {12, 8, "腊八"}, {12, 23, "小年"}};
    if (!lunar.leap) {
        for (auto& d : lunarDays) if (d.month == lunar.month && d.day == lunar.day) return d.name;
        if (lunar.month == 12 && lunar.day == lunarMonthDays(lunar.year, 12, false)) return "除夕";
    }
    for (auto& d : solar) if (d.month == date.month && d.day == date.day) return d.name;
    for (int term : {(date.month - 1) * 2, (date.month - 1) * 2 + 1})
        if (date.day == solarTermDay(date.year, term)) return kSolarTermNames[term];
    return nullptr;
}
int holiday(Date date) {
    for (auto& h : kHolidays) if (h.year == date.year && h.month == date.month && h.day == date.day) return h.off ? 1 : 2;
    return 0;
}
bool holidayYearKnown(int year) {
    for (auto& h : kHolidays) if (h.year == year) return true;
    return false;
}
}
