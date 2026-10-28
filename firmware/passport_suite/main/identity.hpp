#pragma once
#include <cstddef>
#include <cstdint>
#include "lvgl.h"

namespace passport {
// Written by scripts/pack_passport_identity.py or the phone setup page; private data stays out of the app image.
// Header: "PPID", version, CRC32 of [16, kIdentityBytes), flags, then UTF-8 text.
constexpr uint32_t kIdentityAvatarOffset = 0x1000, kIdentityPhotoOffset = 0x10000;
constexpr int kAvatarWidth = 96, kAvatarHeight = 156, kPhotoWidth = 240, kPhotoHeight = 320;
constexpr uint32_t kIdentityBytes = kIdentityPhotoOffset + kPhotoWidth * kPhotoHeight * 2;
struct IdentityHeader { char magic[4]; uint32_t version, crc, flags; char name[64], title[64], motto[160]; };
struct Identity {
    bool valid{}, hasAvatar{}, hasPhoto{}, busy{};
    char name[64]{}, title[64]{}, motto[160]{}, number[16]{};
    lv_image_dsc_t avatar{}, photo{};  // Point into mapped flash.
};
// UI task only. Reloads after an upload; delete image objects first when identityRevision() changes.
const Identity& identity();
uint32_t identityRevision();
bool identityBusy();
// Upload path (any task): erase, write the exact kIdentityBytes image in order, then verify.
bool identityWriteBegin();
bool identityWrite(uint32_t offset, const void* data, size_t size);
bool identityWriteFinish();
// Full CRC check through the flash API; any task.
bool identityStored();
// Copies the stored image for the setup page; false when absent or being written.
bool identityRead(uint32_t offset, void* data, size_t size);
}
