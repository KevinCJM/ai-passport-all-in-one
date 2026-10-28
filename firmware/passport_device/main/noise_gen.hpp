#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace passport {
// 白噪音: every sound is made as it plays (16 kHz mono, integer maths, a few hundred bytes of state), so nothing
// is stored in flash. Kinds: 0 雨声, 1 海浪, 2 粉红噪音, 3 棕噪音, 4 白噪音; all about -15 dBFS RMS.
class NoiseGen {
public:
    static constexpr unsigned kRate = 16000, kKinds = 5;
    void fill(unsigned kind, int16_t* out, unsigned count) {
        // The ocean's swell changes once per block (10 ms at 160 samples): one cosine per block.
        float swell = 1;
        if (kind == 1) {
            if (wave_ >= waveLength_) { wave_ = 0; waveLength_ = kRate * (6 + next() % 5); }
            const float s = 0.5f - 0.5f * cosf(2 * float(M_PI) * wave_ / waveLength_);
            swell = 0.12f + 0.88f * s * s;
            wave_ += count;
        }
        const int32_t level = static_cast<int32_t>(swell * 32768);
        for (unsigned i = 0; i < count; ++i) {
            const int32_t w = white();
            int32_t v = 0;
            if (kind == 0) {
                // Steady hiss (pink above ~500 Hz) and quick decaying drops at random.
                const int32_t p = pink(w), high = p - (low_ += (p - low_) >> 3);
                if (next() % 500 == 0) drop_ = 4000 + next() % 16000;
                v = high * 2 / 3 + static_cast<int32_t>((int64_t(w) * drop_) >> 15);
                drop_ -= drop_ >> 5;
            } else if (kind == 1) {
                // Deep rumble plus foam, both swelling with each wave.
                const int32_t p = pink(w), foam = p - (low_ += (p - low_) >> 4);
                v = static_cast<int32_t>((int64_t(brown(w) + foam / 3) * level) >> 15);
            } else if (kind == 2) v = pink(w) / 2;
            else if (kind == 3) v = brown(w);
            else v = w * 5 / 16;
            out[i] = static_cast<int16_t>(std::clamp<int32_t>(static_cast<int32_t>((int64_t(v) * gain_) >> 15), -32767, 32767));
            // Fade in over 1.5 s, out over 0.5 s.
            if (gain_ < target_) gain_ = std::min(target_, gain_ + 1);
            else if (gain_ > target_) gain_ = std::max(target_, gain_ - 4);
        }
    }
    void start() { target_ = 24000; }
    void stop() { target_ = 0; }
    bool silent() const { return !gain_ && !target_; }
private:
    uint32_t seed_ = 0x2545f491;
    int64_t b0_{}, b1_{}, b2_{};
    int32_t brown_{}, low_{}, drop_{}, gain_{}, target_{};
    unsigned wave_{}, waveLength_{};
    uint32_t next() { seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5; return seed_; }
    int32_t white() { return static_cast<int32_t>(next()) >> 16; }
    // Paul Kellet's economy pink filter in Q15.
    int32_t pink(int32_t w) {
        b0_ = (b0_ * 32691 + w * 3246) >> 15;
        b1_ = (b1_ * 31556 + w * 9716) >> 15;
        b2_ = (b2_ * 18678 + w * 34494) >> 15;
        return static_cast<int32_t>((b0_ + b1_ + b2_ + ((w * 6056) >> 15)) / 4);
    }
    // White noise through a leaky integrator (corner ~20 Hz).
    int32_t brown(int32_t w) { brown_ += (w * 11 >> 8) - (brown_ >> 7); return brown_; }
};
// 闹钟: four short beeps, then a pause, getting louder over the first 20 s.
class AlarmTone {
public:
    void fill(int16_t* out, unsigned count) {
        static constexpr int16_t sine[16]{0, 12539, 23170, 30273, 32767, 30273, 23170, 12539, 0, -12539, -23170, -30273, -32767, -30273, -23170, -12539};
        for (unsigned i = 0; i < count; ++i, ++at_) {
            const unsigned beat = at_ % 19200;  // 1.2 s
            const bool on = beat < 12800 && beat % 3200 < 1920;  // 120 ms beeps, 80 ms apart
            const int32_t loud = 9000 + static_cast<int32_t>(int64_t(std::min<uint32_t>(at_, 320000)) * 21000 / 320000);
            // 2 kHz (8 samples a cycle) for the first two beeps, 1 kHz after: a simple two-tone pattern.
            const int32_t s = beat < 6400 ? sine[(at_ * 2) % 16] : sine[at_ % 16];
            out[i] = on ? static_cast<int16_t>(s * loud >> 15) : 0;
        }
    }
    void reset() { at_ = 0; }
private:
    uint32_t at_{};
};
}
