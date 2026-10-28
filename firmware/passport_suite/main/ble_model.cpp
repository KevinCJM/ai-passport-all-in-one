#include "ble_model.hpp"
#include "esp_heap_caps.h"
#include <cstring>

namespace passport {
BleSessionModel::~BleSessionModel() { stop(); }
bool BleSessionModel::start(BleRole role) {
    if (state_ == BleState::Unsecured || state_ == BleState::Secured) return false;
    if (!reservation_) {
        constexpr unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        if (heap_caps_get_free_size(caps) < kBudget + 48 * 1024
             || heap_caps_get_largest_free_block(caps) < kBudget) return false;
        reservation_ = heap_caps_malloc(kBudget, caps);
        if (!reservation_) return false;
        memset(reservation_, 0, kBudget);
    }
    role_ = role; state_ = BleState::Advertising;
    ++generation_; session_ = sequence_ = 0;
    return true;
}
bool BleSessionModel::onConnect(uint32_t session) {
    if (!session || state_ != BleState::Advertising) return false;
    session_ = session; state_ = BleState::Unsecured;
    return true;
}
bool BleSessionModel::onSecurityReady(bool authenticated, bool encrypted) {
    if (state_ != BleState::Unsecured || !authenticated || !encrypted) return false;
    state_ = BleState::Secured;
    return true;
}
bool BleSessionModel::accept(BleRole channel, uint32_t generation, uint32_t sequence, size_t length) {
    if (state_ != BleState::Secured || channel != role_ || generation != generation_
           || sequence <= sequence_ || !length || length > 160) return false;
    sequence_ = sequence;
    return true;
}
void BleSessionModel::disconnect() {
    ++generation_; session_ = sequence_ = 0;
    state_ = reservation_ ? BleState::Advertising : BleState::Off;
}
void BleSessionModel::stop() {
    disconnect();
    if (reservation_) heap_caps_free(reservation_);
    reservation_ = nullptr; state_ = BleState::Off;
}
}
