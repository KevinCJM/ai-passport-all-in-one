#pragma once
#include <cstdint>

namespace passport::board {
enum class ButtonEvent : uint8_t { None, Up, Down, Confirm, Back, Wake, AccessPoint, Settings };

class ButtonInput {
    int candidate_{-1}, stable_{-1};
    uint64_t candidateAt_{}, pressedAt_{};
    uint32_t context_{UINT32_MAX};
    bool sent_{}, suppressed_{};
public:
    static constexpr uint32_t Volume = 1, Awake = 2, HomeScreen = 4;
    void invalidSample() { candidate_ = -2; }
    ButtonEvent sample(int millivolts, uint64_t now, uint32_t context) {
        const int current = millivolts < 150 ? 0 : millivolts < 447 ? 1 : millivolts < 1900 ? 2 : -1;
        if (context != context_) {
            context_ = context;
            // A held key belongs to the page or wake action that received its press.
            suppressed_ = stable_ >= 0 || candidate_ >= 0 || current >= 0;
        }
        if (current != candidate_) {
            candidate_ = current;
            candidateAt_ = now;
            return ButtonEvent::None;
        }
        if (now - candidateAt_ < 20) return ButtonEvent::None;
        if (current < 0 && stable_ < 0) suppressed_ = false;
        if (current != stable_) {
            const int previous = stable_;
            stable_ = current;
            if (current >= 0) {
                pressedAt_ = now;
                sent_ = false;
                if (!suppressed_ && !(context & Awake)) {
                    sent_ = suppressed_ = true;
                    return ButtonEvent::Wake;
                }
            } else {
                const bool click = previous >= 0 && !suppressed_ && !sent_;
                suppressed_ = false;
                if (click) {
                    return previous == 0 ? ButtonEvent::Up : previous == 1 ? ButtonEvent::Down : ButtonEvent::Confirm;
                }
            }
        }
        if (stable_ >= 0 && !sent_ && !suppressed_ && now - pressedAt_ >= 1000) {
            // Home: long Up/Down do nothing (no hotspot/settings shortcuts); long Confirm turns the screen off.
            if((context&HomeScreen)&&stable_<2){sent_=true;return ButtonEvent::None;}
            // Chat volume changes once on release, including a long press.
            if ((context & Volume) && stable_ < 2) return ButtonEvent::None;
            sent_ = true;
            return stable_ == 2 ? ButtonEvent::Back : stable_ == 0 ? ButtonEvent::AccessPoint : ButtonEvent::Settings;
        }
        return ButtonEvent::None;
    }
};
} // namespace passport::board
