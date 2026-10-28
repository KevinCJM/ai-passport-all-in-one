#include "identity.hpp"
#include "esp_mac.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace passport {
namespace {
Identity loaded;
uint32_t loadedRevision = UINT32_MAX;
esp_partition_mmap_handle_t mapping;
bool mapped;
std::atomic<uint32_t> revision{0};
std::atomic<bool> busy{false};
void image(lv_image_dsc_t& dsc, const uint8_t* data, int width, int height, lv_color_format_t format, uint32_t size) {
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC; dsc.header.cf = format;
    dsc.header.w = width; dsc.header.h = height; dsc.header.stride = width * 2;
    dsc.data = data; dsc.data_size = size;
}
const esp_partition_t* region() {
    // First call comes from app_main before the HTTP server starts.
    static const esp_partition_t* part = [] {
#ifdef PASSPORT_DEVICE
        // Unallocated flash after the new NVS; the shipped partition table stays untouched.
        const esp_partition_t* found = nullptr;
        return esp_partition_register_external(nullptr, 0x630000, 0x40000, "identity", ESP_PARTITION_TYPE_DATA,
                                               static_cast<esp_partition_subtype_t>(0x40), &found) == ESP_OK ? found : nullptr;
#else
        return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "assets");
#endif
    }();
    return part && part->size >= kIdentityBytes ? part : nullptr;
}
bool headerValid(const IdentityHeader& h) {
    return !memcmp(h.magic, "PPID", 4) && h.version == 1 && memchr(h.name, 0, sizeof(h.name))
        && memchr(h.title, 0, sizeof(h.title)) && memchr(h.motto, 0, sizeof(h.motto));
}
// Reads through the flash API, so it is safe while the UI keeps its mapping.
bool storedValid(const esp_partition_t* part, IdentityHeader& header) {
    if (esp_partition_read(part, 0, &header, sizeof(header)) != ESP_OK || !headerValid(header)) return false;
    uint8_t chunk[256]; uint32_t crc = 0;
    for (uint32_t offset = 16; offset < kIdentityBytes; offset += sizeof(chunk)) {
        const size_t size = std::min<size_t>(sizeof(chunk), kIdentityBytes - offset);
        if (esp_partition_read(part, offset, chunk, size) != ESP_OK) return false;
        crc = esp_rom_crc32_le(crc, chunk, size);
    }
    return crc == header.crc;
}
}
uint32_t identityRevision() { return revision.load(); }
bool identityBusy() { return busy.load(); }
const Identity& identity() {
    const auto current = revision.load();
    if (current == loadedRevision) return loaded;
    if (mapped) { esp_partition_munmap(mapping); mapped = false; }
    loaded = {};
    uint8_t mac[6]{};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) snprintf(loaded.number, sizeof(loaded.number), "AP-%02X%02X%02X", mac[3], mac[4], mac[5]);
    // An upload in progress keeps the old revision: reload once it finishes.
    if (busy) { loaded.busy = true; loadedRevision = current; return loaded; }
    loadedRevision = current;
    const auto* part = region();
    const void* data = nullptr;
    if (!part || esp_partition_mmap(part, 0, kIdentityBytes, ESP_PARTITION_MMAP_DATA, &data, &mapping) != ESP_OK) return loaded;
    mapped = true;
    const auto* bytes = static_cast<const uint8_t*>(data);
    const auto& header = *static_cast<const IdentityHeader*>(data);
    if (!headerValid(header) || esp_rom_crc32_le(0, bytes + 16, kIdentityBytes - 16) != header.crc) return loaded;
    // Text is copied so labels never read flash that a later upload rewrites.
    memcpy(loaded.name, header.name, sizeof(loaded.name)); memcpy(loaded.title, header.title, sizeof(loaded.title));
    memcpy(loaded.motto, header.motto, sizeof(loaded.motto));
    loaded.valid = true; loaded.hasAvatar = header.flags & 1; loaded.hasPhoto = header.flags & 2;
    image(loaded.avatar, bytes + kIdentityAvatarOffset, kAvatarWidth, kAvatarHeight, LV_COLOR_FORMAT_RGB565A8, kAvatarWidth * kAvatarHeight * 3);
    image(loaded.photo, bytes + kIdentityPhotoOffset, kPhotoWidth, kPhotoHeight, LV_COLOR_FORMAT_RGB565, kPhotoWidth * kPhotoHeight * 2);
    return loaded;
}
bool identityWriteBegin() {
    const auto* part = region();
    if (!part || busy.exchange(true)) return false;
    ++revision;  // The UI drops its images before the erase reaches them.
    constexpr uint32_t erase = (kIdentityBytes + 0xfff) & ~0xfffU;
    if (esp_partition_erase_range(part, 0, erase) == ESP_OK) return true;
    busy = false; ++revision;
    return false;
}
bool identityWrite(uint32_t offset, const void* data, size_t size) {
    const auto* part = region();
    return busy && part && offset <= kIdentityBytes && size <= kIdentityBytes - offset
        && esp_partition_write(part, offset, data, size) == ESP_OK;
}
bool identityWriteFinish() {
    const auto* part = region();
    IdentityHeader header{};
    const bool valid = busy && part && storedValid(part, header);
    busy = false; ++revision;
    return valid;
}
bool identityStored() {
    const auto* part = region();
    IdentityHeader header{};
    return !busy && part && storedValid(part, header);
}
bool identityRead(uint32_t offset, void* data, size_t size) {
    const auto* part = region();
    IdentityHeader header{};
    return !busy && part && offset <= kIdentityBytes && size <= kIdentityBytes - offset
        && esp_partition_read(part, 0, &header, sizeof(header)) == ESP_OK && headerValid(header)
        && esp_partition_read(part, offset, data, size) == ESP_OK;
}
}
