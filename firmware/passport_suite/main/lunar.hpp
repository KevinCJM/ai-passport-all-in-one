#pragma once
#include <cstdint>
// Offline Chinese calendar for 万年历 and 倒数日: lunar dates, solar terms, festivals, official holidays.
// Tables come from calendar_data.hpp (scripts/generate_calendar_data.py); lunar years 1900-2100.
namespace passport {
inline constexpr int kLunarFirstYear = 1900, kLunarLastYear = 2100;
struct Date { int year, month, day; };
struct LunarDate { int year, month, day; bool leap; };
// Days since 1970-01-01, and back.
int dayNumber(Date date);
Date fromDayNumber(int days);
int weekday(Date date);  // 0 = Sunday
int monthDays(int year, int month);
bool toLunar(Date date, LunarDate& out);
// The solar date of a lunar date (not a leap month); false when that month has no such day or is out of range.
bool fromLunar(int year, int month, int day, Date& out);
int lunarMonthDays(int year, int month, bool leap);
// Day of month of solar term index (0 = 小寒 ... 23 = 冬至; term i falls in month i/2+1), 0 out of range.
int solarTermDay(int year, int index);
extern const char* const kSolarTermNames[24];
// The latest solar term on or before date and the one after it (indices into kSolarTermNames, with their dates);
// false out of range.
bool solarTermsAround(Date date, int& last, Date& lastDate, int& next, Date& nextDate);
const char* lunarMonthName(int month);  // 正月 ... 腊月
const char* lunarDayName(int day);      // 初一 ... 三十
// "丙午" and "马" for a lunar year.
void ganzhi(int lunarYear, char out[7]);
const char* zodiac(int lunarYear);
// Festival on this day (春节, 国庆, 除夕 ...), or nullptr.
const char* festival(Date date, const LunarDate& lunar);
// 1 = official day off, 2 = make-up working day, 0 = neither or no data for that year.
int holiday(Date date);
bool holidayYearKnown(int year);
// 黄历 for one day. 干支 indices 0-59 (甲子 = 0): the year turns on the 立春 day and the month on each 节 day.
struct Almanac { int year, month, day, chong; const char* sha; uint8_t yi[40], ji[40], yiCount, jiCount; };
bool almanac(Date date, Almanac& out);
void ganzhiName(int index, char out[7]);
const char* zodiacOfBranch(int branch);
const char* almanacWord(int index);
}
