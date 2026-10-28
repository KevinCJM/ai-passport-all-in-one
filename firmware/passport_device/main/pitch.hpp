#pragma once
#include <cstdint>
namespace passport {
// YIN pitch detection on 16 kHz samples: kPitchWindow + kPitchMaxLag of them, lag holds kPitchMaxLag + 1 floats.
// The first lag whose normalised difference dips under 0.15, refined by a parabola through its neighbours.
// 40 Hz to 1 kHz; returns 0 when there is no clear pitch. scripts/test_pitch.py checks it.
constexpr unsigned kPitchWindow = 480, kPitchMaxLag = 400;
inline float pitchHz(const int16_t* y, float* lag, unsigned rate) {
    int64_t sum = 0; lag[0] = 1;
    for (unsigned tau = 1; tau <= kPitchMaxLag; ++tau) {
        int64_t d = 0; for (unsigned j = 0; j < kPitchWindow; ++j) { const int64_t e = y[j] - y[j + tau]; d += e * e; }
        sum += d; lag[tau] = sum ? static_cast<float>(d) * tau / static_cast<float>(sum) : 1;
    }
    for (unsigned tau = rate / 1000; tau < kPitchMaxLag; ++tau) {
        if (lag[tau] >= 0.15f) continue;
        while (tau + 1 < kPitchMaxLag && lag[tau + 1] < lag[tau]) ++tau;
        const float a = lag[tau - 1], c = lag[tau], e = lag[tau + 1], bend = a - 2 * c + e;
        return rate / (tau + (bend > 0 ? (a - e) / (2 * bend) : 0));
    }
    return 0;
}
}
