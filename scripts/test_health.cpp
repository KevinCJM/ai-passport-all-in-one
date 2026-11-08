// 健康记录 on the host: clang++ -std=c++20 -I firmware/passport_suite/main scripts/test_health.cpp -o /tmp/th && /tmp/th
#include "health.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
using namespace passport;
#define CHECK(c) do { if (!(c)) { printf("HEALTH_FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main() {
    setenv("TZ", "UTC", 1); tzset();
    const time_t morning = 1793080800 + 2 * 3600;  // Tuesday 2026-10-27 08:00 (UTC)
    const time_t noon = 1793080800 + 6 * 3600;     // 12:00
    auto p = parseHealth("62.5", HealthKind::Weight, noon);
    CHECK(p.ok && p.kind == HealthKind::Weight && p.v1 == 625 && !p.converted);
    p = parseHealth("体重一百二十五斤", HealthKind::Glucose, noon); CHECK(p.ok && p.kind == HealthKind::Weight && p.v1 == 625 && p.converted);
    p = parseHealth("125", HealthKind::Weight, noon); CHECK(p.ok && p.v1 == 625);
    p = parseHealth("105", HealthKind::Weight, noon, 1080); CHECK(p.ok && p.v1 == 1050);  // near a 108 kg last weight
    p = parseHealth("95", HealthKind::Weight, noon, 470); CHECK(p.ok && p.v1 == 475);     // near 47 kg: 95 斤
    p = parseHealth("六十二点五公斤", HealthKind::Weight, noon); CHECK(p.ok && p.v1 == 625);
    CHECK(!parseHealth("体重", HealthKind::Weight, noon).ok);
    p = parseHealth("血糖6.2 空腹", HealthKind::Weight, noon); CHECK(p.ok && p.kind == HealthKind::Glucose && p.v1 == 62 && p.tag == 1);
    p = parseHealth("早餐后两小时血糖8.1", HealthKind::Glucose, noon); CHECK(p.ok && p.v1 == 81 && p.tag == 2);
    p = parseHealth("五点八", HealthKind::Glucose, morning); CHECK(p.ok && p.v1 == 58 && p.tag == 1);
    p = parseHealth("饭后七点五", HealthKind::Glucose, noon); CHECK(p.ok && p.v1 == 75 && p.tag == 3);
    p = parseHealth("110", HealthKind::Glucose, noon); CHECK(p.ok && p.v1 == 61 && p.converted && p.tag == 0);
    p = parseHealth("三十七度五", HealthKind::Temp, noon); CHECK(p.ok && p.v1 == 375);
    p = parseHealth("体温36.6", HealthKind::Weight, noon); CHECK(p.ok && p.kind == HealthKind::Temp && p.v1 == 366);
    p = parseHealth("99.5", HealthKind::Temp, noon); CHECK(p.ok && p.v1 == 375 && p.converted);
    CHECK(!parseHealth("25", HealthKind::Temp, noon).ok);
    p = parseHealth("120/80 心率72", HealthKind::Pressure, noon); CHECK(p.ok && p.v1 == 120 && p.v2 == 80 && p.v3 == 72);
    p = parseHealth("高压一百二低压八十", HealthKind::Weight, noon); CHECK(p.ok && p.kind == HealthKind::Pressure && p.v1 == 120 && p.v2 == 80 && !p.v3);
    p = parseHealth("八十 一百三", HealthKind::Pressure, noon); CHECK(p.ok && p.v1 == 130 && p.v2 == 80);
    CHECK(!parseHealth("120", HealthKind::Pressure, noon).ok);
    p = parseHealth("130/85", HealthKind::Glucose, noon); CHECK(p.ok && p.kind == HealthKind::Pressure && p.v1 == 130);
    p = parseHealth("120 80 心率 70", HealthKind::Glucose, noon); CHECK(p.ok && p.kind == HealthKind::Pressure && p.v3 == 70);
    p = parseHealth("大姨妈来了，量多", HealthKind::Weight, noon); CHECK(p.ok && p.kind == HealthKind::Period && p.tag == 0 && p.v1 == 3 && !p.daysAgo);
    p = parseHealth("月经昨天结束了", HealthKind::Period, noon); CHECK(p.ok && p.tag == 1 && p.daysAgo == 1);
    p = parseHealth("10月20号来的月经", HealthKind::Period, noon); CHECK(p.ok && p.tag == 0 && p.daysAgo == 7);
    p = parseHealth("月经3天前开始", HealthKind::Period, noon); CHECK(p.ok && p.daysAgo == 3);
    p = parseHealth("30号来的大姨妈", HealthKind::Period, noon); CHECK(p.ok && p.daysAgo == 27);  // September 30
    CHECK(!parseHealth("月经", HealthKind::Period, noon).ok);
    p = parseHealth("昨天同房 有保护", HealthKind::Weight, noon); CHECK(p.ok && p.kind == HealthKind::Intimacy && p.tag == 1 && p.daysAgo == 1);
    p = parseHealth("亲密 没戴套", HealthKind::Period, noon); CHECK(p.ok && p.kind == HealthKind::Intimacy && p.tag == 2 && !p.daysAgo);
    p = parseHealth("同房", HealthKind::Weight, noon); CHECK(p.ok && !p.tag);
    CHECK(!parseHealth("一杯", HealthKind::Water, noon).ok);

    // Flags.
    CHECK(healthFlag({0, 0, uint8_t(HealthKind::Glucose), 1, 0, 0, 72, 0, 0}).level == 3);
    CHECK(healthFlag({0, 0, uint8_t(HealthKind::Glucose), 0, 0, 0, 72, 0, 0}).level == 0);
    CHECK(healthFlag({0, 0, uint8_t(HealthKind::Temp), 0, 0, 0, 376, 0, 0}).level == 2);
    CHECK(healthFlag({0, 0, uint8_t(HealthKind::Pressure), 0, 0, 0, 142, 85, 0}).level == 3);

    // The book: newest first, room from synced records, period records spared.
    HealthBook b;
    for (unsigned i = 0; i < HealthBook::kMax; ++i) CHECK(b.add({0, uint32_t(noon - 86400 * 200 + i * 3600), uint8_t(i < 10 ? HealthKind::Period : HealthKind::Weight), uint8_t(i & 1), 0, 0, int16_t(600 + i), 0, 0}));
    CHECK(!b.add({0, uint32_t(noon), uint8_t(HealthKind::Weight), 0, 0, 0, 700, 0, 0}) && b.pending() == HealthBook::kMax);
    for (unsigned i = 0; i < b.count; ++i) b.ack(b.items[i].id, b.items[i].rev);
    CHECK(b.add({0, uint32_t(noon), uint8_t(HealthKind::Weight), 0, 0, 0, 700, 0, 0}) && b.count == HealthBook::kMax);
    CHECK(b.items[0].kind == uint8_t(HealthKind::Period) && b.items[8].kind == uint8_t(HealthKind::Period) && b.items[9].kind == uint8_t(HealthKind::Weight));
    uint8_t order[HealthBook::kMax];
    CHECK(b.list(HealthKind::Weight, order, 3) == 3 && b.items[order[0]].v1 == 700 && b.items[order[1]].v1 == 600 + HealthBook::kMax - 1);
    char json[1600]; healthJson(b, json, sizeof(json), 6);
    CHECK(strstr(json, "\"kind\":\"weight\"") && strstr(json, "\"v\":[700,0,0]") && strstr(json, "\"left\":0}"));
    // Water: one record a day.
    HealthBook w; const int today = healthDay(noon);
    CHECK(w.cups(today) == 0 && w.add({0, uint32_t(noon), uint8_t(HealthKind::Water), 0, 0, 0, 1, 250, 0}) && w.cups(today) == 1 && w.cups(today + 1) == 0);

    // 经期: cycles 30 and 28 days, lengths 5 and 6.
    HealthBook c; const auto day = [&](int ago) { return uint32_t(healthNoon(noon, ago)); };
    c.add({0, day(70), 5, 0, 0, 0, 0, 0, 0}); c.add({0, day(66), 5, 1, 0, 0, 0, 0, 0});
    c.add({0, day(40), 5, 0, 0, 0, 0, 0, 0}); c.add({0, day(35), 5, 1, 0, 0, 0, 0, 0});
    c.add({0, day(12), 5, 0, 0, 0, 0, 0, 0});
    auto s = periodState(c, noon); CHECK(s.known && !s.active && s.cycle == 29 && s.length == 6 && s.nextIn == 17);
    CHECK(s.ovulationIn == 3 && s.fertile);
    c.add({0, day(1), 5, 0, 0, 0, 0, 0, 0}); s = periodState(c, noon); CHECK(s.active && s.day == 2 && !s.fertile);

    // 喝水提醒: 60 minutes inside 9-21.
    HealthPrefs prefs; CHECK(waterNext(prefs, morning - 3600) == morning + 3600);  // 07:00 -> 09:00
    CHECK(waterNext(prefs, noon) == noon + 3600);
    CHECK(waterNext(prefs, noon + 9 * 3600) == morning + 86400 + 3600);  // 21:00 -> tomorrow 09:00
    prefs.water = 0; CHECK(!waterNext(prefs, noon));
    puts("HEALTH_OK");
}
