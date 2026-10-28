#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace passport {
// 自选股: Tencent's public quote service (plain HTTP, no key). A security is kept as "<market>.<code>": 1 Shanghai,
// 0 Shenzhen or Beijing, 116 Hong Kong, 105 NASDAQ, 106 NYSE, 107 NYSE American/Arca ('.' in a US code is '_'),
// or one of kStockExtras below.
struct StockList {
    static constexpr uint32_t kVersion = 1; static constexpr unsigned kMax = 12;
    struct Item { char id[16]; char name[24]; };
    uint32_t version{kVersion}; uint8_t count{}; Item items[kMax]{};
};
inline StockList defaultStocks() {
    return {StockList::kVersion, 7, {{"1.000001", "上证指数"}, {"1.510300", "沪深300ETF"}, {"1.513100", "纳指ETF"},
                                     {"1.600519", "贵州茅台"}, {"116.00700", "腾讯控股"}, {"105.AAPL", "苹果"},
                                     {"101.GC00Y", "纽约黄金"}}};
}
// A security found by searchStocks (wifi_portal.cpp): Eastmoney's search box over plain HTTP, its markets limited to
// those the quotes cover. word is URL-encoded; returns the hits, or -1 when the search failed.
struct StockHit { char id[16], name[32], type[16], code[12]; };
int searchStocks(const char* word, StockHit* hits, unsigned capacity);
inline bool stockUs(unsigned market) { return market >= 105 && market <= 107; }
// Gold, oil and currencies, kept by Eastmoney's id (what the phone search returns). Tencent quotes them (hf_ futures
// and spot metals, fx currencies) but draws no chart, so charts come from Eastmoney's history service.
struct StockExtra { const char* id; const char* symbol; const char* code; const char* tag; };
inline constexpr StockExtra kStockExtras[]{
    {"101.GC00Y", "hf_GC", "GC", "CMX"}, {"101.SI00Y", "hf_SI", "SI", "CMX"}, {"101.HG00Y", "hf_HG", "HG", "CMX"},
    {"102.CL00Y", "hf_CL", "CL", "NYM"}, {"102.NG00Y", "hf_NG", "NG", "NYM"}, {"112.B00Y", "hf_OIL", "BRENT", "ICE"},
    {"122.XAU", "hf_XAU", "XAU", "LDN"}, {"122.XAG", "hf_XAG", "XAG", "LDN"},
    {"100.UDI", "fxDINIW", "USDX", "FX"}, {"119.EURUSD", "fxEURUSD", "EURUSD", "FX"},
    {"133.USDCNH", "fxUSDCNY", "USDCNY", "FX"},  // Tencent quotes onshore yuan only; the chart is offshore (CNH)
};
inline const StockExtra* stockExtra(const char* id) {
    for (const auto& x : kStockExtras) if (!strcmp(x.id, id)) return &x;
    return nullptr;
}
inline bool stockIdValid(const char* id) {
    if (stockExtra(id)) return true;
    const char* dot = strchr(id, '.');
    if (!dot || dot == id || dot - id > 3 || !dot[1] || strlen(id) >= sizeof(StockList::Item::id)) return false;
    for (const char* c = id; c < dot; ++c) if (!isdigit(static_cast<unsigned char>(*c))) return false;
    for (const char* c = dot + 1; *c; ++c) if (!isalnum(static_cast<unsigned char>(*c)) && *c != '_') return false;
    const unsigned market = atoi(id);
    return market <= 1 || market == 116 || stockUs(market);
}
// Points in one trading day: A shares 09:30-11:30 and 13:00-15:00, Hong Kong to 12:00 and 16:00, US 6.5 hours,
// the extras round the clock.
inline unsigned stockSession(unsigned market) { return market <= 1 ? 241 : market == 116 ? 331 : stockUs(market) ? 391 : 1440; }
inline const char* stockMarketTag(unsigned market, const char* code) {
    if (market == 1) return "SH";
    if (market == 0) return *code == '4' || *code == '8' || *code == '9' ? "BJ" : "SZ";
    return market == 116 ? "HK" : stockUs(market) ? "US" : "";
}
// Tencent's symbol: sh600519, sz159915, bj830799, hk00700, usAAPL; charts want the US exchange too (usAAPL.OQ).
inline void stockSymbol(char* out, size_t size, const char* id, bool chart) {
    if (const auto* x = stockExtra(id)) { snprintf(out, size, "%s", x->symbol); return; }
    const unsigned market = atoi(id); const char* code = strchr(id, '.') + 1;
    const char* prefix = market == 1 ? "sh" : market == 116 ? "hk" : stockUs(market) ? "us" : *code == '4' || *code == '8' || *code == '9' ? "bj" : "sz";
    const char* suffix = !chart || !stockUs(market) ? "" : market == 105 ? ".OQ" : market == 106 ? ".N" : ".AM";
    int used = snprintf(out, size, "%s%s%s", prefix, code, suffix);
    for (char* c = out + 2; used > 0 && *c; ++c) if (*c == '_') *c = '.';
}
inline void stockQuoteUrl(char* out, size_t size, const StockList& list) {
    int used = snprintf(out, size, "http://qt.gtimg.cn/q=");
    for (unsigned i = 0; i < list.count && used > 0 && size_t(used) < size; ++i) {
        char symbol[24]; stockSymbol(symbol, sizeof(symbol), list.items[i].id, false);
        used += snprintf(out + used, size - used, "%s%s", i ? "," : "", symbol);
    }
}
// Today's minutes ("0930 1311.84 ...", A and HK from minute/query, US from UsMinute) or the last 30 days.
// HTTPS only: plain HTTP is redirected, and esp_http_client would follow it on port 80.
// The extras: Eastmoney's "2026-10-26 06:00,4012.1" minutes or "2026-10-23,open,close,high,low" days, plain HTTP.
inline void stockChartUrl(char* out, size_t size, const char* id, bool days) {
    if (stockExtra(id)) {
        if (days) snprintf(out, size, "http://push2his.eastmoney.com/api/qt/stock/kline/get?secid=%s&fields1=f1&fields2=f51,f52,f53,f54,f55&klt=101&fqt=0&lmt=30&end=20500101", id);
        else snprintf(out, size, "http://push2his.eastmoney.com/api/qt/stock/trends2/get?secid=%s&fields1=f1&fields2=f51,f53&ndays=1", id);
        return;
    }
    char symbol[24]; stockSymbol(symbol, sizeof(symbol), id, true);
    if (days) snprintf(out, size, "https://web.ifzq.gtimg.cn/appstock/app/kline/kline?param=%s,day,,,30", symbol);
    else snprintf(out, size, "https://web.ifzq.gtimg.cn/appstock/app/%s/query?code=%s", stockUs(atoi(id)) ? "UsMinute" : "minute", symbol);
}
// One quote line, v_sh600519="1~name~600519~price~...";: its fields split in place at '~'. Fields used:
// 3 price, 4 previous close, 5 open, 31 change, 32 change %, 33 high, 34 low, 37 turnover (10k yuan for A shares).
inline unsigned stockFields(char* quote, char** fields, unsigned capacity, char separator = '~') {
    unsigned count = 0;
    for (char* c = quote; count < capacity; ) { fields[count++] = c; c = strchr(c, separator); if (!c) break; *c++ = 0; }
    return count;
}
// Digits after the point of a price as sent ("386.000" -> 3).
inline unsigned stockDecimals(const char* price) { const char* dot = strchr(price, '.'); return dot ? std::min<size_t>(strspn(dot + 1, "0123456789"), 4) : 0; }
// One quote (split in place) as price, change, change %, open, high, low, previous close, turnover; false when short.
// Futures (hf_GC="4007.60,-0.01,bid,ask,high,low,time,prev,open,...") split at ','; currencies (fxUSDCNY="310~name~
// code~price~0~time~prev~open~high~low~~~change~pct~...") and stocks at '~'.
inline bool stockQuote(char* quote, const char* symbol, float* v, unsigned& decimals) {
    const auto real = [](const char* text) { char* end; const float x = strtof(text, &end); return end == text ? NAN : x; };
    char* f[40];
    const bool futures = !strncmp(symbol, "hf_", 3), currency = !strncmp(symbol, "fx", 2);
    const unsigned count = stockFields(quote, f, 40, futures ? ',' : '~');
    if (futures) {
        if (count < 9) return false;
        const float price = real(f[0]), prev = real(f[7]);
        const float x[]{price, price - prev, real(f[1]), real(f[8]), real(f[4]), real(f[5]), prev, NAN};
        std::copy(x, x + 8, v); decimals = stockDecimals(f[0]); return true;
    }
    if (count < (currency ? 14u : 38u)) return false;
    static constexpr int stock[]{3, 31, 32, 5, 33, 34, 4, 37}, rate[]{3, 12, 13, 7, 8, 9, 6};
    for (unsigned k = 0; k < 8; ++k) v[k] = currency && k == 7 ? NAN : real(f[(currency ? rate : stock)[k]]);
    if (!strncmp(symbol, "sh", 2) || !strncmp(symbol, "sz", 2) || !strncmp(symbol, "bj", 2)) v[7] *= 1e4f;  // A shares: 10k yuan
    decimals = stockDecimals(f[3]); return true;
}
// The strings of the JSON array that follows `after` (e.g. "\"data\":["), one at a time as the reply arrives.
class StringArrayScan {
public:
    using Item = void (*)(void* context, const char* text);
    StringArrayScan(const char* after, Item item, void* context) : after_(after), item_(item), context_(context) {}
    void feed(const char* data, size_t size) { for (size_t i = 0; i < size; ++i) put(data[i]); }
private:
    const char* after_; Item item_; void* context_;
    unsigned matched_{}, used_{}; bool started_{}, string_{}, done_{}; char text_[64]{};
    void put(char c) {
        if (done_) return;
        if (!started_) { matched_ = c == after_[matched_] ? matched_ + 1 : c == after_[0]; started_ = !after_[matched_]; return; }
        if (string_) { if (c == '"') { string_ = false; text_[used_] = 0; item_(context_, text_); } else if (used_ < sizeof(text_) - 1) text_[used_++] = c; return; }
        if (c == '"') { string_ = true; used_ = 0; } else if (c == ']') done_ = true;
    }
};
// Every string value after `key` (e.g. "\"title\":\""), wherever it appears, one at a time as the reply arrives.
// Escapes are kept as they are; longer values are cut.
class FieldScan {
public:
    using Item = void (*)(void* context, const char* text);
    FieldScan(const char* key, Item item, void* context) : key_(key), item_(item), context_(context) {}
    void feed(const char* data, size_t size) { for (size_t i = 0; i < size; ++i) put(data[i]); }
    void reset(void* context) { *this = FieldScan(key_, item_, context); }
private:
    const char* key_; Item item_; void* context_;
    unsigned matched_{}, used_{}; bool inside_{}; char text_[256]{};
    void put(char c) {
        if (inside_) { if (c == '"') { inside_ = false; text_[used_] = 0; item_(context_, text_); } else if (used_ < sizeof(text_) - 1) text_[used_++] = c; return; }
        matched_ = c == key_[matched_] ? matched_ + 1 : c == key_[0];
        if (!key_[matched_]) { inside_ = true; used_ = matched_ = 0; }
    }
};
// Each JSON value `depth` brackets deep (counting [ and {) whole, as the reply arrives: with depth 3 the rows of
// {"list":[[...],[...]]}. One longer than the buffer is skipped.
class JsonSlices {
public:
    using Item = void (*)(void* context, char* text);
    JsonSlices(unsigned depth, char* buffer, size_t capacity, Item item, void* context) : depth_(depth), buffer_(buffer), capacity_(capacity), item_(item), context_(context) {}
    void feed(const char* data, size_t size) { for (size_t i = 0; i < size; ++i) put(data[i]); }
private:
    unsigned depth_, level_{}; char* buffer_; size_t capacity_, used_{}; Item item_; void* context_; bool string_{}, escape_{}, over_{};
    void put(char c) {
        if (level_ >= depth_) { if (used_ + 1 < capacity_) buffer_[used_++] = c; else over_ = true; }
        if (string_) { if (escape_) escape_ = false; else if (c == '\\') escape_ = true; else if (c == '"') string_ = false; return; }
        if (c == '"') string_ = true;
        else if (c == '[' || c == '{') { if (++level_ == depth_) { buffer_[0] = c; used_ = 1; over_ = false; } }
        else if ((c == ']' || c == '}') && level_ && level_-- == depth_) { buffer_[used_] = 0; if (!over_) item_(context_, buffer_); }
    }
};
// The text after `marker` up to the next "</div", tags and line breaks dropped: one block of a page's prose
// (nmc.cn's warning text) as it arrives. Longer text is cut, possibly inside a character.
class HtmlTextScan {
public:
    HtmlTextScan(const char* marker, char* out, size_t capacity) : marker_(marker), out_(out), capacity_(capacity) { *out = 0; }
    void feed(const char* data, size_t size) { for (size_t i = 0; i < size; ++i) put(data[i]); }
    bool done() const { return done_; }
private:
    const char* marker_; char* out_; size_t capacity_, used_{};
    unsigned matched_{}, tagUsed_{}; bool started_{}, tag_{}, done_{}; char name_[4]{};
    void put(char c) {
        if (done_) return;
        if (!started_) { matched_ = c == marker_[matched_] ? matched_ + 1 : c == marker_[0]; started_ = !marker_[matched_]; return; }
        if (tag_) { if (c == '>') { tag_ = false; done_ = !strncmp(name_, "/div", 4); } else if (tagUsed_ < 4) name_[tagUsed_++] = c; return; }
        if (c == '<') { tag_ = true; tagUsed_ = 0; memset(name_, 0, sizeof(name_)); return; }
        if (c != '\n' && c != '\r' && used_ + 1 < capacity_) { out_[used_++] = c; out_[used_] = 0; }
    }
};
// The rows of a JSON array of arrays after `after` ("\"day\":["): each row's first kFields strings, objects in a
// row skipped (Hong Kong's dividend notes).
class RowArrayScan {
public:
    static constexpr unsigned kFields = 5;
    using Row = void (*)(void* context, char (*fields)[16], unsigned count);
    RowArrayScan(const char* after, Row row, void* context) : after_(after), row_(row), context_(context) {}
    void feed(const char* data, size_t size) { for (size_t i = 0; i < size; ++i) put(data[i]); }
private:
    const char* after_; Row row_; void* context_;
    unsigned matched_{}, used_{}, depth_{}, braces_{}, count_{}; bool started_{}, string_{}, escape_{}, done_{};
    char fields_[kFields][16]{};
    void put(char c) {
        if (done_) return;
        if (!started_) { matched_ = c == after_[matched_] ? matched_ + 1 : c == after_[0]; started_ = !after_[matched_]; depth_ = 1; return; }
        if (string_) {
            if (escape_) { escape_ = false; return; }
            if (c == '\\') { escape_ = true; return; }
            if (c == '"') { string_ = false; if (!braces_ && depth_ == 2 && count_ < kFields) fields_[count_++][std::min<unsigned>(used_, 15)] = 0; return; }
            if (!braces_ && depth_ == 2 && count_ < kFields && used_ < 15) fields_[count_][used_++] = c;
            return;
        }
        if (c == '"') { string_ = true; used_ = 0; }
        else if (c == '{') ++braces_;
        else if (c == '}' && braces_) --braces_;
        else if (braces_) return;
        else if (c == '[') { if (++depth_ == 2) count_ = 0; }
        else if (c == ']') { if (--depth_ == 1) row_(context_, fields_, count_); else if (!depth_) done_ = true; }
    }
};
// At most kMax values in arrival order: once full, every other one is dropped and the spacing (stride) doubles,
// so a 391-minute US day still fits evenly.
struct ChartPoints {
    static constexpr unsigned kMax = 200;
    float v[kMax]; unsigned count{}, stride{1}, skip{};
    void add(float x) {
        if (skip) { --skip; return; }
        if (count == kMax) { for (unsigned i = 0; i < kMax / 2; ++i) v[i] = v[2 * i]; count = kMax / 2; stride *= 2; }
        v[count++] = x; skip = stride - 1;
    }
};
}
