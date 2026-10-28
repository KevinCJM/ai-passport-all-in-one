#pragma once
#include "stock_catalog.hpp"
#include "../../passport_suite/main/apps.hpp"
#include "../../passport_suite/main/weather.hpp"

namespace passport {
// 百度热搜's board (top.baidu.com/api/board?platform=pc&tab=realtime) as it arrives: 50 items of about 900 bytes,
// fields in alphabetical order (desc, hotScore, hotTag ... word), then "topContent" with the pinned one. The first
// kHotRows are kept, the pinned one first (tag kHotPinned).
struct HotItem { char title[72]; char desc[240]; uint32_t score; uint8_t tag; };
class HotBoardScan {
public:
    explicit HotBoardScan(HotItem* items) : items_(items) { start(); }
    // Starts over (a retried download).
    void start() {
        count_ = 0; pinned_ = false; next_ = {};
        for (auto* s : {&desc_, &score_, &tag_, &word_, &top_}) s->reset(this);
    }
    // A byte at a time to every field, so each value lands on its own item.
    void feed(const char* data, size_t size) { for (size_t i = 0; i < size; ++i) for (auto* s : {&desc_, &score_, &tag_, &word_, &top_}) s->feed(data + i, 1); }
    unsigned count() const { return count_; }
private:
    HotItem* items_; unsigned count_{}; bool pinned_{}; HotItem next_{};
    static HotBoardScan& self(void* context) { return *static_cast<HotBoardScan*>(context); }
    FieldScan desc_{"\"desc\":\"", [](void* c, const char* s) { auto& d = self(c).next_.desc; fitText(d, sizeof(d), s, strlen(s)); }, nullptr};
    FieldScan score_{"\"hotScore\":\"", [](void* c, const char* s) { self(c).next_.score = strtoul(s, nullptr, 10); }, nullptr};
    FieldScan tag_{"\"hotTag\":\"", [](void* c, const char* s) { self(c).next_.tag = uint8_t(atoi(s)); }, nullptr};
    FieldScan word_{"\"word\":\"", [](void* c, const char* s) { self(c).add(s); }, nullptr};
    FieldScan top_{"\"topContent\":", [](void* c, const char*) { self(c).pinned_ = true; }, nullptr};
    void add(const char* title) {
        auto item = next_; next_ = {};
        if (!pinned_ && count_ == kHotRows) return;
        fitText(item.title, sizeof(item.title), title, strlen(title));
        // FieldScan cuts a long summary at its buffer (inside a character) or at an escaped quote.
        size_t n = strlen(item.desc); while (n && item.desc[n - 1] == '\\') item.desc[--n] = 0;
        whole(item.desc); whole(item.title);
        if (pinned_) {
            // First; the last one drops out of a full board.
            item.tag = kHotPinned; const unsigned kept = std::min(count_, kHotRows - 1);
            std::move_backward(items_, items_ + kept, items_ + kept + 1); items_[0] = item; count_ = kept + 1;
        } else items_[count_++] = item;
    }
    // Drops a character cut short at the end of text.
    static void whole(char* text) {
        size_t n = strlen(text), start = n; while (start && (static_cast<unsigned char>(text[start - 1]) & 0xc0) == 0x80) --start;
        if (!start) return;
        const auto lead = static_cast<unsigned char>(text[start - 1]);
        const size_t want = lead < 0x80 ? 1 : lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
        if (n - (start - 1) < want) text[start - 1] = 0;
    }
};
}
