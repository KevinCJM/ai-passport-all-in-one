#pragma once
#include <cstddef>
#include <cstdint>

namespace passport {
enum class BleRole : uint8_t { Data };
enum class BleState : uint8_t { Off, Advertising, Unsecured, Secured };
// Simulator contract + real allocation of the BLE design budget.
// No radio/GAP/GATT/NimBLE functions are called by this model.
class BleSessionModel {
    void* reservation_{};
    BleRole role_{BleRole::Data};
    BleState state_{BleState::Off};
    uint32_t generation_{1}, session_{}, sequence_{};
public:
    constexpr static size_t kBudget = 48 * 1024;
    BleSessionModel() = default;
    ~BleSessionModel();
    BleSessionModel(const BleSessionModel&) = delete;
    BleSessionModel& operator=(const BleSessionModel&) = delete;
    bool start(BleRole role);
    bool onConnect(uint32_t session);
    bool onSecurityReady(bool authenticated, bool encrypted);
    bool accept(BleRole channel, uint32_t generation, uint32_t sequence, size_t length);
    void disconnect();
    void stop();
    uint32_t generation() const { return generation_; }
    BleState state() const { return state_; }
};
}
