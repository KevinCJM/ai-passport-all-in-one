// Double-Confirm host check: c++ -std=c++20 -Wall -Wextra -Werror -Ifirmware/passport_device/main scripts/test_button_input.cpp -o /tmp/test_button_input && /tmp/test_button_input
#include "button_input.hpp"
#include <cassert>
#include <vector>
using namespace passport::board;
// Feeds level (-1 none, 0 Up, 1 Down, 2 Confirm) in 10 ms steps from t until `until`; collects events.
struct Rig {
    ButtonInput in; uint64_t t = 1000; std::vector<ButtonEvent> got;
    Rig() { hold(-1, 50); }  // keys up when it starts (a key held at boot belongs to the boot)
    void hold(int key, unsigned ms) {
        const int mv = key == 0 ? 0 : key == 1 ? 300 : key == 2 ? 1000 : 3000;
        for (uint64_t end = t + ms; t < end; t += 10) if (auto e = in.sample(mv, t, ButtonInput::Awake); e != ButtonEvent::None) got.push_back(e);
    }
};
int main() {
    { Rig r; r.hold(2, 80); r.hold(-1, 50); assert(r.got.empty()); r.hold(-1, 400); assert((r.got == std::vector{ButtonEvent::Confirm})); }  // single, held back
    { Rig r; r.hold(2, 80); r.hold(-1, 90); r.hold(2, 80); r.hold(-1, 500); assert((r.got == std::vector{ButtonEvent::Voice})); }  // double: no Confirm at all
    { Rig r; r.hold(2, 80); r.hold(-1, 500); r.hold(2, 80); r.hold(-1, 500); assert((r.got == std::vector{ButtonEvent::Confirm, ButtonEvent::Confirm})); }  // two slow clicks
    { Rig r; r.hold(2, 80); r.hold(-1, 100); r.hold(0, 80); r.hold(-1, 100); assert((r.got == std::vector{ButtonEvent::Confirm, ButtonEvent::Up})); }  // Confirm then Up: order kept
    { Rig r; r.hold(0, 80); r.hold(-1, 100); assert((r.got == std::vector{ButtonEvent::Up})); }  // Up is not delayed
    { Rig r; r.hold(2, 1200); r.hold(-1, 500); assert((r.got == std::vector{ButtonEvent::Back})); }  // long Confirm unchanged
}
