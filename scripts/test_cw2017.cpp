// Battery gauge host check: c++ -std=c++20 -Wall -Wextra -Werror -Ifirmware/passport_device/main scripts/test_cw2017.cpp -o /tmp/test_cw2017 && /tmp/test_cw2017
// After deep sleep the gauge reads 0% for a while: show "--%" (unknown), not 0%, while the cell is clearly not empty.
#include "cw2017.hpp"
#include <cassert>
#include <cstdio>
struct Bus {
    uint8_t regs[256]{};
    bool read(uint8_t reg, uint8_t* data, size_t size) { for (size_t i = 0; i < size; ++i) data[i] = regs[reg + i]; return true; }
    bool write(uint8_t reg, uint8_t value) { regs[reg] = value; return true; }
    void delayMs(unsigned) {}
    void set(unsigned percent, unsigned mv) { regs[4] = percent; const unsigned raw = mv * 10000 / 3125; regs[2] = raw >> 8; regs[3] = raw & 0xff; }
};
int main() {
    Bus bus;
    bus.regs[0x0b] = 0x80; for (size_t i = 0; i < sizeof(passport::Cw2017<Bus>::profile); ++i) bus.regs[0x10 + i] = passport::Cw2017<Bus>::profile[i];
    bus.regs[0x08] = 0xf0;  // asleep, as left before deep sleep
    passport::Cw2017<Bus> gauge(bus);
    assert(gauge.initialize(0));
    bus.set(0, 3900);
    for (uint64_t t = 100; t < 3000; t += 100) { gauge.poll(t); assert(!gauge.reading().known); }
    bus.set(76, 3900); gauge.poll(3100);
    assert(gauge.reading().known && gauge.reading().percent == 76);
    // A really empty cell still reads 0%.
    passport::Cw2017<Bus> empty(bus); assert(empty.initialize(0)); bus.set(0, 3300); empty.poll(100);
    assert(empty.reading().known && empty.reading().percent == 0);
    // A gauge that never moves off 0% is believed after the 20 s grace.
    passport::Cw2017<Bus> stuck(bus); assert(stuck.initialize(0)); bus.set(0, 3900);
    for (uint64_t t = 100; t <= 20000; t += 100) stuck.poll(t);
    assert(stuck.reading().known && stuck.reading().percent == 0);
    puts("PASS cw2017 waits out the 0% after wake");
}
