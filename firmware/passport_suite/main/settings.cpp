#include "settings.hpp"
#include "menu_layout.hpp"
#include "word_study.hpp"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_partition.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace passport {
namespace {
bool valid(const SystemSettings& settings) {
    return settings.version == 1 && settings.idleSeconds <= 86400 && settings.brightness <= 100 && settings.bluetoothEnabled <= 1
        && memchr(settings.wifiSsid, 0, sizeof(settings.wifiSsid))
        && memchr(settings.wifiPassword, 0, sizeof(settings.wifiPassword))
        && memchr(settings.timezone, 0, sizeof(settings.timezone));
}
class Handle {
public:
    nvs_handle_t value{};
    ~Handle() { if (value) nvs_close(value); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
}
bool SettingsStore::load(SystemSettings& settings) {
    ++reads;
    Handle handle;
    if (nvs_open("suite_system", NVS_READONLY, &handle.value) != ESP_OK) return false;
    SystemSettings candidate{};
    size_t size = sizeof(candidate);
    if (nvs_get_blob(handle.value, "config_v1", &candidate, &size) != ESP_OK
          || size != sizeof(candidate) || !valid(candidate)) return false;
    settings = candidate;
    return true;
}
bool SettingsStore::save(const SystemSettings& settings) {
    if (!valid(settings)) return false;
    Handle handle;
    if (nvs_open("suite_system", NVS_READWRITE, &handle.value) != ESP_OK) return false;
    if (nvs_set_blob(handle.value, "config_v1", &settings, sizeof(settings)) != ESP_OK
           || nvs_commit(handle.value) != ESP_OK) return false;
    ++writes;
    return true;
}
uint8_t SettingsStore::loadByte(const char* key) {
    Handle handle; uint8_t value = 0;
    if (nvs_open("suite_system", NVS_READONLY, &handle.value) == ESP_OK) nvs_get_u8(handle.value, key, &value);
    return value;
}
bool SettingsStore::loadMenu(MenuLayout& layout) {
    Handle handle;MenuLayout candidate;size_t size=sizeof(candidate);
    if(nvs_open("suite_system",NVS_READONLY,&handle.value)!=ESP_OK||nvs_get_blob(handle.value,"menu_v1",&candidate,&size)!=ESP_OK||size!=sizeof(candidate)||!candidate.upgrade()){
        layout=defaultMenuLayout();return false;
    }
    layout=candidate;return true;
}
bool SettingsStore::saveMenu(const MenuLayout& layout) {
    Handle handle;
    return layout.valid()&&nvs_open("suite_system",NVS_READWRITE,&handle.value)==ESP_OK
        &&nvs_set_blob(handle.value,"menu_v1",&layout,sizeof(layout))==ESP_OK&&nvs_commit(handle.value)==ESP_OK;
}
bool SettingsStore::saveByte(const char* key, uint8_t value) {
    Handle handle;
    return nvs_open("suite_system", NVS_READWRITE, &handle.value) == ESP_OK
        && nvs_set_u8(handle.value, key, value) == ESP_OK && nvs_commit(handle.value) == ESP_OK;
}
// The deck is one blob: about 1 KiB rewritten after each graded word.
bool SettingsStore::loadWords(WordDeck& deck) {
    Handle handle; WordDeck candidate; size_t size = sizeof(candidate);
    if (nvs_open("suite_words", NVS_READONLY, &handle.value) != ESP_OK
        || nvs_get_blob(handle.value, "deck_v1", &candidate, &size) != ESP_OK || size != sizeof(candidate)
        || candidate.version != WordDeck::kVersion || candidate.count > WordDeck::kCards) return false;
    for (unsigned i = 0; i < candidate.count; ++i)
        if (candidate.cards[i].box > WordDeck::kMastered || candidate.cards[i].word >= candidate.next) return false;
    deck = candidate;
    return true;
}
bool SettingsStore::saveWords(const WordDeck& deck) {
    Handle handle;
    return nvs_open("suite_words", NVS_READWRITE, &handle.value) == ESP_OK
        && nvs_set_blob(handle.value, "deck_v1", &deck, sizeof(deck)) == ESP_OK && nvs_commit(handle.value) == ESP_OK;
}
bool SettingsStore::loadStatus(StatusBoard& board) {
    Handle handle; StatusBoard candidate; size_t size = sizeof(candidate);
    if (nvs_open("suite_system", NVS_READONLY, &handle.value) != ESP_OK
        || nvs_get_blob(handle.value, "status_v1", &candidate, &size) != ESP_OK || (size != sizeof(candidate) && size != kStatusV1Size)
        || candidate.version != StatusBoard::kVersion || candidate.awake > 1) return false;
    for (auto& text : candidate.custom) if (!memchr(text, 0, sizeof(text))) return false;
    board = candidate;
    return true;
}
bool SettingsStore::saveStatus(const StatusBoard& board) {
    Handle handle;
    return nvs_open("suite_system", NVS_READWRITE, &handle.value) == ESP_OK
        && nvs_set_blob(handle.value, "status_v1", &board, sizeof(board)) == ESP_OK && nvs_commit(handle.value) == ESP_OK;
}
bool SettingsStore::loadRates(RateBoard& board) {
    Handle handle; RateBoard candidate; size_t size = sizeof(candidate);
    if (nvs_open("suite_system", NVS_READONLY, &handle.value) != ESP_OK
        || nvs_get_blob(handle.value, "rates_v1", &candidate, &size) != ESP_OK || size != sizeof(candidate)
        || candidate.version != RateBoard::kVersion || !memchr(candidate.base, 0, 4)) return false;
    for (auto& code : candidate.targets) if (!memchr(code, 0, 4)) return false;
    board = candidate;
    return true;
}
bool SettingsStore::saveRates(const RateBoard& board) {
    Handle handle;
    return nvs_open("suite_system", NVS_READWRITE, &handle.value) == ESP_OK
        && nvs_set_blob(handle.value, "rates_v1", &board, sizeof(board)) == ESP_OK && nvs_commit(handle.value) == ESP_OK;
}
bool SettingsStore::loadCountdown(CountdownBoard& board) {
    Handle handle; CountdownBoard candidate; size_t size = sizeof(candidate);
    if (nvs_open("suite_system", NVS_READONLY, &handle.value) != ESP_OK
        || nvs_get_blob(handle.value, "countdown_v1", &candidate, &size) != ESP_OK || size != sizeof(candidate)
        || candidate.version != CountdownBoard::kVersion || candidate.offwork >= 24 * 60) return false;
    for (auto& event : candidate.events) if (!memchr(event.name, 0, sizeof(event.name))) return false;
    board = candidate;
    return true;
}
bool SettingsStore::saveCountdown(const CountdownBoard& board) {
    Handle handle;
    return nvs_open("suite_system", NVS_READWRITE, &handle.value) == ESP_OK
        && nvs_set_blob(handle.value, "countdown_v1", &board, sizeof(board)) == ESP_OK && nvs_commit(handle.value) == ESP_OK;
}
bool SettingsStore::loadQr(unsigned index, QrEntry& entry) {
    Handle handle; QrEntry candidate; size_t size = sizeof(candidate); char key[8]; snprintf(key, sizeof(key), "qr%u", index);
    if (index >= QrEntry::kCount || nvs_open("suite_system", NVS_READONLY, &handle.value) != ESP_OK
        || nvs_get_blob(handle.value, key, &candidate, &size) != ESP_OK || size != sizeof(candidate)
        || !memchr(candidate.title, 0, sizeof(candidate.title)) || !memchr(candidate.text, 0, sizeof(candidate.text))) return false;
    entry = candidate;
    return true;
}
bool SettingsStore::saveQr(unsigned index, const QrEntry& entry) {
    Handle handle; char key[8]; snprintf(key, sizeof(key), "qr%u", index);
    return index < QrEntry::kCount && nvs_open("suite_system", NVS_READWRITE, &handle.value) == ESP_OK
        && nvs_set_blob(handle.value, key, &entry, sizeof(entry)) == ESP_OK && nvs_commit(handle.value) == ESP_OK;
}
bool SettingsStore::loadTools(ToolPrefs& prefs) {
    Handle handle; ToolPrefs candidate; size_t size = sizeof(candidate);
    if (nvs_open("suite_system", NVS_READONLY, &handle.value) != ESP_OK
        || nvs_get_blob(handle.value, "tools_v1", &candidate, &size) != ESP_OK || (size != sizeof(candidate) && size != kToolPrefsV1Size)
        || candidate.version != ToolPrefs::kVersion) return false;
    prefs = candidate;
    return true;
}
bool SettingsStore::saveTools(const ToolPrefs& prefs) {
    Handle handle;
    return nvs_open("suite_system", NVS_READWRITE, &handle.value) == ESP_OK
        && nvs_set_blob(handle.value, "tools_v1", &prefs, sizeof(prefs)) == ESP_OK && nvs_commit(handle.value) == ESP_OK;
}
bool AlarmList::valid() const {
    if (version != kVersion || count > kMax) return false;
    for (unsigned i = 0; i < count; ++i) if (items[i].hour > 23 || items[i].minute > 59 || items[i].mode > 3) return false;
    return true;
}
bool SettingsStore::loadAlarms(AlarmList& list) {
    Handle handle; AlarmList candidate; size_t size = sizeof(candidate);
    if (nvs_open("suite_system", NVS_READONLY, &handle.value) != ESP_OK) return false;
    if (nvs_get_blob(handle.value, "alarm_v2", &candidate, &size) == ESP_OK && size == sizeof(candidate) && candidate.valid()) { list = candidate; return true; }
    // The one alarm from before.
    AlarmPrefs old; size = sizeof(old); candidate = {};
    if (nvs_get_blob(handle.value, "alarm_v1", &old, &size) == ESP_OK && size == sizeof(old) && old.version == AlarmPrefs::kVersion
        && old.hour <= 23 && old.minute <= 59 && old.mode <= 3) { candidate.items[0] = old; candidate.count = 1; }
    list = candidate;
    return true;
}
bool SettingsStore::saveAlarms(const AlarmList& list) {
    Handle handle;
    return nvs_open("suite_system", NVS_READWRITE, &handle.value) == ESP_OK
        && nvs_set_blob(handle.value, "alarm_v2", &list, sizeof(list)) == ESP_OK && nvs_commit(handle.value) == ESP_OK;
}
void TodoList::sort() {
    const auto rank = [](const TodoItem& t) { return (t.flags & kDone) ? 2u : t.at ? 0u : 1u; };
    std::stable_sort(items, items + count, [&](const TodoItem& a, const TodoItem& b) {
        return rank(a) != rank(b) ? rank(a) < rank(b) : rank(a) == 0 && a.at < b.at;
    });
}
bool TodoList::valid() const {
    if (version != kVersion || count > kMax) return false;
    for (unsigned i = 0; i < count; ++i) if (!memchr(items[i].text, 0, sizeof(items[i].text)) || !*items[i].text) return false;
    return true;
}
const TodoItem* TodoList::next() const {
    const TodoItem* best = nullptr;
    for (unsigned i = 0; i < count; ++i) {
        const auto& t = items[i];
        if (t.at && !(t.flags & (kDone | kRung)) && (!best || t.at < best->at)) best = &t;
    }
    return best;
}
bool SettingsStore::loadTodo(TodoList& list) {
    Handle handle; size_t size = sizeof(list);
    if (nvs_open("suite_system", NVS_READONLY, &handle.value) != ESP_OK || nvs_get_blob(handle.value, "todo_v1", &list, &size) != ESP_OK
        || size != sizeof(list) || !list.valid()) { list = {}; return false; }
    return true;
}
bool SettingsStore::saveTodo(const TodoList& list) {
    Handle handle;
    return list.valid() && nvs_open("suite_system", NVS_READWRITE, &handle.value) == ESP_OK
        && nvs_set_blob(handle.value, "todo_v1", &list, sizeof(list)) == ESP_OK && nvs_commit(handle.value) == ESP_OK;
}
bool SettingsStore::loadLedger(LedgerBook& book) {
    Handle handle; size_t size = sizeof(book);
    if (nvs_open("suite_system", NVS_READONLY, &handle.value) != ESP_OK || nvs_get_blob(handle.value, "ledger_v1", &book, &size) != ESP_OK
        || size != sizeof(book) || !book.valid()) { book = {}; return false; }
    return true;
}
bool SettingsStore::saveLedger(const LedgerBook& book) {
    Handle handle;
    return book.valid() && nvs_open("suite_system", NVS_READWRITE, &handle.value) == ESP_OK
        && nvs_set_blob(handle.value, "ledger_v1", &book, sizeof(book)) == ESP_OK && nvs_commit(handle.value) == ESP_OK;
}
bool SettingsStore::loadGuard(GuardPrefs& prefs) {
    Handle handle; GuardPrefs candidate; size_t size = sizeof(candidate);
    if (nvs_open("suite_system", NVS_READONLY, &handle.value) != ESP_OK || nvs_get_blob(handle.value, "guard_v1", &candidate, &size) != ESP_OK
        || size != sizeof(candidate) || !candidate.valid()) { prefs = {}; return false; }
    prefs = candidate;
    return true;
}
bool SettingsStore::saveGuard(const GuardPrefs& prefs) {
    Handle handle;
    return prefs.valid() && nvs_open("suite_system", NVS_READWRITE, &handle.value) == ESP_OK
        && nvs_set_blob(handle.value, "guard_v1", &prefs, sizeof(prefs)) == ESP_OK && nvs_commit(handle.value) == ESP_OK;
}
bool WheelBoard::validText(const char* text, size_t bytes) {
    unsigned chars = 0;
    for (size_t i = 0; i < bytes; ++i) {
        if (static_cast<unsigned char>(text[i]) < 0x20) return false;
        chars += (text[i] & 0xC0) != 0x80;
    }
    return bytes && bytes < kText && chars <= kChars;
}
bool WheelBoard::valid() const {
    if (version != kVersion || count > kModes || (count && current >= count)) return false;
    const auto text = [](const char (&s)[kText]) { const void* end = memchr(s, 0, kText); return end && validText(s, static_cast<const char*>(end) - s); };
    for (unsigned i = 0; i < count; ++i) {
        if (!text(modes[i].name) || modes[i].count > kOptions) return false;
        for (unsigned j = 0; j < modes[i].count; ++j) if (!text(modes[i].options[j])) return false;
    }
    return true;
}
int WheelBoard::find(const char* name) const {
    for (unsigned i = 0; i < count; ++i) if (!strcmp(modes[i].name, name)) return i;
    return -1;
}
WheelBoard defaultWheel() {
    WheelBoard board{};
    const auto add = [&](const char* name, std::initializer_list<const char*> options) {
        auto& mode = board.modes[board.count++]; strcpy(mode.name, name);
        for (const char* option : options) strcpy(mode.options[mode.count++], option);
    };
    add("吃饭", {"烧烤", "汉堡", "炸鸡", "面条", "快餐"});
    add("喝什么", {"奶茶", "咖啡", "果汁", "茶", "白开水"});
    add("掷骰子", {"1", "2", "3", "4", "5", "6"});
    return board;
}
bool SettingsStore::loadScore(ScoreBoard& board) {
    Handle handle; size_t size = sizeof(board);
    if (nvs_open("suite_system", NVS_READONLY, &handle.value) != ESP_OK || nvs_get_blob(handle.value, "score_v1", &board, &size) != ESP_OK
        || size != sizeof(board) || !board.valid()) { board = {}; return false; }
    return true;
}
bool SettingsStore::saveScore(const ScoreBoard& board) {
    Handle handle;
    return board.valid() && nvs_open("suite_system", NVS_READWRITE, &handle.value) == ESP_OK
        && nvs_set_blob(handle.value, "score_v1", &board, sizeof(board)) == ESP_OK && nvs_commit(handle.value) == ESP_OK;
}
bool SettingsStore::loadWheel(WheelBoard& board) {
    Handle handle; size_t size = sizeof(board);
    if (nvs_open("suite_system", NVS_READONLY, &handle.value) != ESP_OK || nvs_get_blob(handle.value, "wheel_v1", &board, &size) != ESP_OK
        || size != sizeof(board) || !board.valid()) { board = defaultWheel(); return false; }
    return true;
}
bool SettingsStore::saveWheel(const WheelBoard& board) {
    Handle handle;
    return board.valid() && nvs_open("suite_system", NVS_READWRITE, &handle.value) == ESP_OK
        && nvs_set_blob(handle.value, "wheel_v1", &board, sizeof(board)) == ESP_OK && nvs_commit(handle.value) == ESP_OK;
}
int WifiList::find(const char* ssid) const {
    for (unsigned i = 0; i < count; ++i) if (!strcmp(items[i].ssid, ssid)) return i;
    return -1;
}
bool WifiList::remember(const char* ssid, const char* password) {
    if (!*ssid || strlen(ssid) >= sizeof(items[0].ssid) || strlen(password) >= sizeof(items[0].password)) return false;
    const int at = find(ssid);
    if (at == 0 && !strcmp(items[0].password, password)) return false;
    const unsigned last = at >= 0 ? at : std::min<unsigned>(count, kMax - 1);
    memmove(&items[1], &items[0], last * sizeof(items[0]));
    items[0] = {}; strcpy(items[0].ssid, ssid); strcpy(items[0].password, password);
    if (at < 0 && count < kMax) ++count;
    return true;
}
void WifiList::remove(unsigned index) {
    if (index >= count) return;
    memmove(&items[index], &items[index + 1], (count - index - 1) * sizeof(items[0]));
    items[--count] = {};
}
bool SettingsStore::loadWifi(WifiList& list) {
    Handle handle; WifiList candidate; size_t size = sizeof(candidate);
    if (nvs_open("suite_system", NVS_READONLY, &handle.value) != ESP_OK || nvs_get_blob(handle.value, "wifi_v1", &candidate, &size) != ESP_OK
        || size != sizeof(candidate) || candidate.version != WifiList::kVersion || candidate.count > WifiList::kMax) return false;
    for (auto& n : candidate.items) if (!memchr(n.ssid, 0, sizeof(n.ssid)) || !memchr(n.password, 0, sizeof(n.password))) return false;
    list = candidate;
    return true;
}
bool SettingsStore::saveWifi(const WifiList& list) {
    Handle handle;
    return nvs_open("suite_system", NVS_READWRITE, &handle.value) == ESP_OK
        && nvs_set_blob(handle.value, "wifi_v1", &list, sizeof(list)) == ESP_OK && nvs_commit(handle.value) == ESP_OK;
}
namespace {
// The order of the notes, newest first; each note is the blob "n<id>": its epoch, then the text and its NUL.
struct MemoIndex { uint16_t next; uint16_t count; uint16_t ids[MemoNote::kMax]; };
#ifdef PASSPORT_DEVICE
constexpr const char* kMemoPart = "memo";
#else
constexpr const char* kMemoPart = NVS_DEFAULT_PART_NAME;
#endif
bool memoIndex(Handle& handle, MemoIndex& index) {
    index = {}; size_t size = sizeof(index);
    const auto result = nvs_get_blob(handle.value, "index", &index, &size);
    if (result == ESP_ERR_NVS_NOT_FOUND) { index = {}; return true; }
    return result == ESP_OK && size == sizeof(index) && index.count <= MemoNote::kMax;
}
bool memoOpen(Handle& handle, nvs_open_mode_t mode) { return nvs_open_from_partition(kMemoPart, "memo", mode, &handle.value) == ESP_OK; }
}
bool MemoStore::mount() {
#ifdef PASSPORT_DEVICE
    // Registered once; the shipped partition table stays as it is. A region that was never NVS is formatted.
    static const bool registered = [] {
        const esp_partition_t* part = nullptr;
        return esp_partition_register_external(nullptr, 0x6B0000, 0x20000, kMemoPart, ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, &part) == ESP_OK;
    }();
    if (!registered) return false;
    auto result = nvs_flash_init_partition(kMemoPart);
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (nvs_flash_erase_partition(kMemoPart) != ESP_OK) return false;
        result = nvs_flash_init_partition(kMemoPart);
    }
    return result == ESP_OK;
#else
    return true;
#endif
}
void MemoStore::unmount() {
#ifdef PASSPORT_DEVICE
    nvs_flash_deinit_partition(kMemoPart);
#endif
}
unsigned MemoStore::count() {
    Handle handle; MemoIndex index;
    return memoOpen(handle, NVS_READONLY) && memoIndex(handle, index) ? index.count : 0;
}
bool MemoStore::load(unsigned position, MemoNote& note) {
    Handle handle; MemoIndex index; char key[8];
    if (!memoOpen(handle, NVS_READONLY) || !memoIndex(handle, index) || position >= index.count) return false;
    snprintf(key, sizeof(key), "n%u", index.ids[position]); size_t size = sizeof(note);
    return nvs_get_blob(handle.value, key, &note, &size) == ESP_OK && size > sizeof(note.at) && !note.text[size - sizeof(note.at) - 1];
}
bool MemoStore::add(const char* text, uint32_t at) {
    Handle handle; MemoIndex index; MemoNote note; char key[8];
    if (!memoOpen(handle, NVS_READWRITE) || !memoIndex(handle, index) || index.count >= MemoNote::kMax) return false;
    // Cut at a character boundary, never halfway through a UTF-8 sequence.
    size_t length = std::min(strlen(text), sizeof(note.text) - 1);
    while (length && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) --length;
    note.at = at; memcpy(note.text, text, length); note.text[length] = 0;
    const uint16_t id = index.next++;
    snprintf(key, sizeof(key), "n%u", id);
    // The note first: a power cut before the index is written only leaves an unlisted blob.
    if (nvs_set_blob(handle.value, key, &note, sizeof(note.at) + length + 1) != ESP_OK) return false;
    memmove(index.ids + 1, index.ids, index.count * sizeof(index.ids[0])); index.ids[0] = id; ++index.count;
    if (nvs_set_blob(handle.value, "index", &index, sizeof(index)) != ESP_OK || nvs_commit(handle.value) != ESP_OK) {
        nvs_erase_key(handle.value, key); nvs_commit(handle.value); return false;
    }
    return true;
}
bool MemoStore::remove(unsigned position) {
    Handle handle; MemoIndex index; char key[8];
    if (!memoOpen(handle, NVS_READWRITE) || !memoIndex(handle, index) || position >= index.count) return false;
    snprintf(key, sizeof(key), "n%u", index.ids[position]);
    memmove(index.ids + position, index.ids + position + 1, (index.count - position - 1) * sizeof(index.ids[0])); --index.count;
    if (nvs_set_blob(handle.value, "index", &index, sizeof(index)) != ESP_OK) return false;
    nvs_erase_key(handle.value, key);
    return nvs_commit(handle.value) == ESP_OK;
}
bool SettingsStore::loadOrCreate(SystemSettings& settings, bool& created) {
    created = !load(settings);
    return !created || save(settings);
}
// 电子书 (BookStore in settings.hpp). Nothing is cached, to spare RAM: the meta is read from NVS each time. Only the
// computer's requests write it (one at a time, over USB on the httpd task or Bluetooth on the main loop); the place read
// has a key of its own, written by the page. A read racing a write sees a stale window at worst, and each chunk's header
// names its book and number, so a sector reused meanwhile reads as missing, never as the wrong text.
namespace {
std::atomic<unsigned> bookRevision;
const esp_partition_t* bookPartition() {
#ifdef PASSPORT_DEVICE
    static const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
#else
    static const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "userdata");
#endif
    return part;
}
void bookRead(BookMeta& meta, uint32_t& place) {
    meta = {}; place = 0; Handle handle; size_t size = sizeof(meta);
    if (nvs_open("book", NVS_READONLY, &handle.value) != ESP_OK) return;
    if (nvs_get_blob(handle.value, "meta", &meta, &size) != ESP_OK || size != sizeof(meta) || meta.version != BookMeta::kVersion
        || meta.slots != BookStore::slots() || meta.lo > meta.hi || meta.hi - meta.lo > meta.slots || meta.hi > meta.chunks) meta = {};
    meta.title[sizeof(meta.title) - 1] = 0;
    nvs_get_u32(handle.value, "place", &place);
}
bool bookWrite(const BookMeta* meta, const uint32_t* place) {
    Handle handle;
    return nvs_open("book", NVS_READWRITE, &handle.value) == ESP_OK
        && (!meta || nvs_set_blob(handle.value, "meta", meta, sizeof(*meta)) == ESP_OK)
        && (!place || nvs_set_u32(handle.value, "place", *place) == ESP_OK) && nvs_commit(handle.value) == ESP_OK;
}
// Chunk c's header, when its sector still holds it.
bool bookHead(const esp_partition_t* part, const BookMeta& m, unsigned c, BookChunk& head) {
    return esp_partition_read(part, (c % m.slots) * BookStore::kSector, &head, sizeof(head)) == ESP_OK
        && head.magic == BookChunk::kMagic && head.id == m.id && head.chunk == c && head.count <= BookStore::kSector - sizeof(head);
}
}
uint16_t BookStore::slots() { const auto* part = bookPartition(); return part ? static_cast<uint16_t>(std::min<size_t>(kMaxSlots, part->size / kSector)) : 0; }
void BookStore::load(BookMeta& meta, uint32_t& place) { bookRead(meta, place); }
unsigned BookStore::revision() { return bookRevision.load(); }
bool BookStore::savePlace(uint32_t place) { return bookWrite(nullptr, &place); }
uint32_t BookStore::start() {
    BookMeta m; uint32_t place; bookRead(m, place); BookChunk head{};
    const auto* part = bookPartition();
    return part && m.id && m.lo < m.hi && bookHead(part, m, m.lo, head) ? head.first : UINT32_MAX;
}
size_t BookStore::text(uint32_t at, char* out, size_t size) {
    BookMeta m; uint32_t place; bookRead(m, place); *out = 0;
    const auto* part = bookPartition();
    if (!part || !m.id || size < 2) return 0;
    // The chunk holding byte `at`: chunks are in order, so halve the window.
    unsigned lo = m.lo, hi = m.hi, c = 0; BookChunk head{};
    for (;;) {
        if (lo >= hi) return 0;
        c = (lo + hi) / 2;
        if (!bookHead(part, m, c, head)) return 0;
        if (at < head.first) hi = c; else if (at >= head.first + head.count) lo = c + 1; else break;
    }
    // Then on into the chunks after it while there is room.
    size_t used = 0;
    for (uint32_t from = at - head.first;;) {
        const size_t take = std::min<size_t>(head.count - from, size - 1 - used);
        if (esp_partition_read(part, (c % m.slots) * kSector + sizeof(head) + from, out + used, take) != ESP_OK) break;
        used += take;
        if (used + 1 == size || ++c >= m.hi || !bookHead(part, m, c, head)) break;
        from = 0;
    }
    // Whole characters only: a sequence cut at the end goes.
    size_t lead = used;
    while (lead && (static_cast<unsigned char>(out[lead - 1]) & 0xc0) == 0x80) --lead;
    if (lead) { const auto b = static_cast<unsigned char>(out[lead - 1]); const size_t need = b < 0x80 ? 1 : b >= 0xf0 ? 4 : b >= 0xe0 ? 3 : 2; if (used - (lead - 1) < need) used = lead - 1; }
    out[used] = 0; return used;
}
bool BookStore::open(uint32_t id, const char* title, uint32_t size, uint16_t chunks, uint16_t at, uint32_t place) {
    if (!id || !size || !chunks || at >= chunks || place >= size || !slots()) return false;
    BookMeta m; m.id = id; m.size = size; m.chunks = chunks; m.lo = m.hi = at; m.slots = slots();
    size_t length = std::min(strlen(title), sizeof(m.title) - 1);
    while (length && (static_cast<unsigned char>(title[length]) & 0xc0) == 0x80) --length;
    memcpy(m.title, title, length);
    const bool saved = bookWrite(&m, &place); ++bookRevision; return saved;
}
bool BookStore::drop(uint16_t lo) {
    BookMeta m; uint32_t place; bookRead(m, place);
    if (!m.id || lo <= m.lo) return true;
    m.lo = std::min(lo, m.hi); return bookWrite(&m, nullptr);
}
bool BookStore::write(uint16_t chunk, uint16_t at, const char* text, size_t size) {
    BookMeta m; uint32_t place; bookRead(m, place); const auto* part = bookPartition();
    // Only the next chunk, into the sector of one already dropped.
    if (!part || !m.id || chunk != m.hi || chunk >= m.chunks || m.hi - m.lo >= m.slots || sizeof(BookChunk) + at + size > kSector) return false;
    const size_t base = (chunk % m.slots) * kSector;
    if (!at && esp_partition_erase_range(part, base, kSector) != ESP_OK) return false;
    return !size || esp_partition_write(part, base + sizeof(BookChunk) + at, text, size) == ESP_OK;
}
bool BookStore::finish(uint16_t chunk, uint32_t first, uint16_t length) {
    BookMeta m; uint32_t place; bookRead(m, place); const auto* part = bookPartition();
    if (!part || !m.id || chunk != m.hi || chunk >= m.chunks || m.hi - m.lo >= m.slots || !length || length > kSector - sizeof(BookChunk) || first + length > m.size) return false;
    const BookChunk head{BookChunk::kMagic, m.id, chunk, length, first};
    if (esp_partition_write(part, (chunk % m.slots) * kSector, &head, sizeof(head)) != ESP_OK) return false;
    ++m.hi; const bool saved = bookWrite(&m, nullptr); ++bookRevision; return saved;
}
} // namespace passport
