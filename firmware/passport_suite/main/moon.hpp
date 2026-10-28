#pragma once
#include <cmath>
#include <cstdint>
// 万年历's 月相: computed on the badge. The Moon's elongation from the Sun comes from the larger periodic terms of
// Meeus, Astronomical Algorithms ch. 47 (about 0.02°, so phase times land within a few minutes).
namespace passport {
// Elongation in degrees, 0-360: 0 new moon, 90 first quarter, 180 full, 270 last quarter. at: Unix seconds.
inline double moonElongation(double at) {
    constexpr double rad = 3.14159265358979323846 / 180;
    // Terrestrial time runs about 69 s ahead of UTC.
    const double t = ((at + 69) / 86400 + 2440587.5 - 2451545) / 36525;
    const double d = (297.8501921 + 445267.1114034 * t) * rad, m = (357.5291092 + 35999.0502909 * t) * rad;
    const double mm = (134.9633964 + 477198.8675055 * t) * rad, f = (93.2720950 + 483202.0175233 * t) * rad;
    // The Moon's longitude terms (micro-degrees), then the Sun's equation of centre.
    const double moon = 6288774 * sin(mm) + 1274027 * sin(2 * d - mm) + 658314 * sin(2 * d) + 213618 * sin(2 * mm)
        - 185116 * sin(m) - 114332 * sin(2 * f) + 58793 * sin(2 * d - 2 * mm) + 57066 * sin(2 * d - m - mm)
        + 53322 * sin(2 * d + mm) + 45758 * sin(2 * d - m) - 40923 * sin(m - mm) - 34720 * sin(d) - 30383 * sin(m + mm)
        + 15327 * sin(2 * d - 2 * f) - 12528 * sin(mm + 2 * f) + 10980 * sin(mm - 2 * f) + 10675 * sin(4 * d - mm)
        + 10034 * sin(3 * mm) + 8548 * sin(4 * d - 2 * mm) - 7888 * sin(2 * d + m - mm) - 6766 * sin(2 * d + m)
        - 5163 * sin(d - mm) + 4987 * sin(d + m) + 4036 * sin(2 * d - m + mm);
    const double sun = (1.914602 - 0.004817 * t) * sin(m) + 0.019993 * sin(2 * m) + 0.000289 * sin(3 * m);
    const double e = fmod(d / rad + moon / 1e6 - sun, 360);
    return e < 0 ? e + 360 : e;
}
// The lit share of the disc, 0-1.
inline double moonLit(double elongation) { return (1 - cos(elongation * 3.14159265358979323846 / 180)) / 2; }
// The first time after `from` that the elongation reaches quarter * 90° (0 新月, 1 上弦, 2 满月, 3 下弦), to the second.
inline double moonPhaseAfter(double from, int quarter) {
    // How far the Moon is past the target, -180 to 180; it gains about 12° a day, so a day's step never skips it.
    const auto past = [&](double at) { const double e = fmod(moonElongation(at) - quarter * 90 + 540, 360); return e - 180; };
    double low = from, high = from + 86400;
    for (int days = 0; days < 32 && !(past(low) < 0 && past(high) >= 0); ++days) { low = high; high += 86400; }
    while (high - low > 1) { const double mid = (low + high) / 2; (past(mid) < 0 ? low : high) = mid; }
    return high;
}
inline const char* moonQuarterName(int quarter) {
    static const char* const names[]{"新月", "上弦月", "满月", "下弦月"};
    return names[quarter & 3];
}
// A day's phase: the quarter that falls within it (0-3), otherwise (-1) the part of the cycle the Moon is in at noon.
inline const char* moonPhaseName(int quarterToday, double elongationAtNoon) {
    if (quarterToday >= 0) return moonQuarterName(quarterToday);
    const double e = elongationAtNoon;
    return e < 90 ? "蛾眉月" : e < 180 ? "盈凸月" : e < 270 ? "亏凸月" : "残月";
}
}  // namespace passport
