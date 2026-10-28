#pragma once
#include "lunar.hpp"
#include "settings.hpp"
#include <ctime>

namespace passport {
// The first ring at or after `from` (local time), or 0 when the alarm is off.
inline time_t nextAlarm(const AlarmPrefs& a, time_t from) {
    if (!a.mode || a.mode > 3 || a.hour > 23 || a.minute > 59) return 0;
    tm day{}; localtime_r(&from, &day);
    for (int d = 0; d < 15; ++d) {
        tm t = day; t.tm_mday += d; t.tm_hour = a.hour; t.tm_min = a.minute; t.tm_sec = 0; t.tm_isdst = -1;
        const time_t at = mktime(&t);  // also fills in the date and weekday
        if (at < from) continue;
        if (a.mode == 2) {
            const int off = holiday({t.tm_year + 1900, t.tm_mon + 1, t.tm_mday});
            if (off == 1 || (off != 2 && (t.tm_wday == 0 || t.tm_wday == 6))) continue;
        }
        return at;
    }
    return 0;
}
// The first ring of any of them.
inline time_t nextAlarm(const AlarmList& list, time_t from) {
    time_t first = 0;
    for (unsigned i = 0; i < list.count && i < AlarmList::kMax; ++i) if (const time_t at = nextAlarm(list.items[i], from); at && (!first || at < first)) first = at;
    return first;
}
}
