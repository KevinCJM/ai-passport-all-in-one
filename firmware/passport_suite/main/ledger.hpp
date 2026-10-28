#pragma once
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
// 语音记账: one spoken entry ("午饭 35", "打车花了二十八块五", "收到红包200") becomes an amount, a category and a note,
// worked out on the badge. The badge keeps today's entries, and older ones only until the computer has them: its
// assistant takes every new or changed entry over USB or Bluetooth (ledgerExchange) into its CSV and acknowledges
// each by id and revision, so an entry changed while on its way is sent again.
namespace passport {
// Categories in the order Confirm steps through them; the last one is income.
inline constexpr const char* kLedgerCategories[]{"餐饮", "交通", "购物", "居家", "娱乐", "医疗", "人情", "学习", "其他", "收入"};
inline constexpr uint8_t kLedgerCount = 10, kLedgerOther = 8, kLedgerIncome = 9;
// at: epoch of the entry; cents: always positive (the category says income); rev: bumped by every change.
// flags: kSynced (the computer has this revision), kDeleted (removed here, the computer still to be told).
struct LedgerEntry { uint32_t id, at; int32_t cents; uint8_t category, flags, rev, pad; char note[28]; };
struct LedgerBook {
    static constexpr uint8_t kVersion = 1, kMax = 30, kNoteChars = 9, kSynced = 1, kDeleted = 2;
    uint8_t version{kVersion}, count{}; uint16_t pad{}; uint32_t lastId{};
    LedgerEntry items[kMax]{};  // oldest first
    bool valid() const {
        if (version != kVersion || count > kMax) return false;
        for (unsigned i = 0; i < count; ++i)
            if (!memchr(items[i].note, 0, sizeof(items[i].note)) || items[i].category >= kLedgerCount || items[i].cents <= 0) return false;
        return true;
    }
    int find(uint32_t id) const { for (unsigned i = 0; i < count; ++i) if (items[i].id == id) return int(i); return -1; }
    void erase(unsigned i) { memmove(items + i, items + i + 1, (count - i - 1) * sizeof(LedgerEntry)); items[--count] = {}; }
    unsigned pending() const { unsigned n = 0; for (unsigned i = 0; i < count; ++i) n += !(items[i].flags & kSynced); return n; }
    // Drops what is no longer needed: entries before today the computer has, deletions it has. True if any went.
    bool prune(time_t today) {
        const unsigned before = count;
        for (unsigned i = count; i-- > 0;) {
            const auto& e = items[i];
            if ((e.flags & kSynced) && ((e.flags & kDeleted) || time_t(e.at) < today)) erase(i);
        }
        return count != before;
    }
    // A new entry (false: full). Ids are unique and rising: the time, or one past the last.
    bool add(uint32_t at, int32_t cents, uint8_t category, const char* note) {
        if (count == kMax) return false;
        auto& e = items[count++]; e = {}; e.id = lastId = at > lastId ? at : lastId + 1; e.at = at; e.cents = cents; e.category = category;
        snprintf(e.note, sizeof(e.note), "%s", note);
        return true;
    }
    void touch(unsigned i) { items[i].flags &= ~kSynced; ++items[i].rev; }
    // Removed here: one the computer never had just goes; otherwise it is kept, marked, until the computer is told.
    void remove(unsigned i) { if (!(items[i].flags & kSynced) && !items[i].rev) erase(i); else { items[i].flags |= kDeleted; touch(i); } }
    // The computer has revision rev of entry id. True if that changed anything.
    bool ack(uint32_t id, uint8_t rev) {
        const int i = find(id);
        if (i < 0 || items[i].rev != rev || (items[i].flags & kSynced)) return false;
        if (items[i].flags & kDeleted) erase(unsigned(i)); else items[i].flags |= kSynced;
        return true;
    }
    // Today's entries on show, newest first (deleted ones left out); returns how many, their indexes in order.
    unsigned shown(time_t today, uint8_t* order) const {
        unsigned n = 0;
        for (unsigned i = count; i-- > 0;) if (!(items[i].flags & kDeleted) && time_t(items[i].at) >= today) order[n++] = uint8_t(i);
        return n;
    }
    // Today's spending and income, in cents.
    void totals(time_t today, int32_t& out, int32_t& in) const {
        out = in = 0;
        for (unsigned i = 0; i < count; ++i) {
            const auto& e = items[i];
            if ((e.flags & kDeleted) || time_t(e.at) < today) continue;
            (e.category == kLedgerIncome ? in : out) += e.cents;
        }
    }
    // Up to `most` entries the computer has not got yet, as JSON: {"entries":[{"id":..,"rev":..,"at":..,"cents":..,
    // "cat":"餐饮","note":"午饭"} or {"id":..,"rev":..,"del":1}],"left":N}. Notes never hold quotes or backslashes.
    void pendingJson(char* out, size_t size, unsigned most) const {
        size_t used = snprintf(out, size, "{\"entries\":[");unsigned sent = 0;
        for (unsigned i = 0; i < count && sent < most; ++i) {
            const auto& e = items[i];
            if (e.flags & kSynced) continue;
            char one[160];
            if (e.flags & kDeleted) snprintf(one, sizeof(one), "%s{\"id\":%lu,\"rev\":%u,\"del\":1}", sent ? "," : "", (unsigned long)e.id, e.rev);
            else snprintf(one, sizeof(one), "%s{\"id\":%lu,\"rev\":%u,\"at\":%lu,\"cents\":%ld,\"cat\":\"%s\",\"note\":\"%s\"}", sent ? "," : "",
                          (unsigned long)e.id, e.rev, (unsigned long)e.at, (long)e.cents, kLedgerCategories[e.category], e.note);
            if (used + strlen(one) + 24 >= size) break;
            used += snprintf(out + used, size - used, "%s", one); ++sent;
        }
        snprintf(out + used, size - used, "],\"left\":%u}", pending() - sent);
    }
};

struct LedgerParse { bool ok{}; int32_t cents{}; uint8_t category{kLedgerOther}; std::string note; };

namespace ledger_text {
inline bool at(const std::string& s, size_t i, const char* word) { return s.compare(i, strlen(word), word) == 0; }
inline int digit(const std::string& s, size_t i) {
    static const char* const digits[]{"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
    if (i < s.size() && s[i] >= '0' && s[i] <= '9') return s[i] - '0';
    if (at(s, i, "两")) return 2;
    for (int v = 0; v < 10; ++v) if (at(s, i, digits[v])) return v;
    return -1;
}
inline size_t width(const std::string& s, size_t i) {
    const auto c = static_cast<unsigned char>(s[i]); return c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
}
// A number at i: Arabic (with decimals, "1,200", "3万") or Chinese ("二十八", "一百二" = 120, "两千五" = 2500, "三点五").
// Returns its bytes (0: none) and its value in cents; chinese tells which, terms how many characters it took.
inline size_t number(const std::string& s, size_t i, int64_t& cents, bool& chinese, unsigned& terms) {
    size_t n = i; cents = 0; terms = 0;
    if (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        chinese = false; int64_t whole = 0;
        while (n < s.size() && ((s[n] >= '0' && s[n] <= '9') || (s[n] == ',' && n + 3 < s.size() && n > i))) { if (s[n] != ',') whole = whole * 10 + (s[n] - '0'); ++n; if (whole > 100000000) return 0; }
        cents = whole * 100;
        if (n + 1 < s.size() && s[n] == '.' && s[n + 1] >= '0' && s[n + 1] <= '9') {
            ++n; cents += (s[n++] - '0') * 10;
            if (n < s.size() && s[n] >= '0' && s[n] <= '9') cents += s[n++] - '0';
            while (n < s.size() && s[n] >= '0' && s[n] <= '9') ++n;
        }
        if (at(s, n, "万")) { cents *= 10000; n += 3; } else if (at(s, n, "千")) { cents *= 1000; n += 3; }
        terms = 1;
        return n - i;
    }
    chinese = true;
    // section: below 万; last: the unit just passed, so a bare digit after it is the next one down ("一百二" = 120)
    // unless a 零 came between ("一百零五" = 105).
    int64_t total = 0, section = 0; int pending = -1, last = 0; bool zero = false;
    while (n < s.size()) {
        const int d = (s[n] & 0x80) ? digit(s, n) : -1;
        if (d == 0) { zero = true; n += 3; ++terms; continue; }
        if (d > 0) { if (pending >= 0) break; pending = d; n += 3; ++terms; continue; }
        const int unit = at(s, n, "十") ? 10 : at(s, n, "百") ? 100 : at(s, n, "千") ? 1000 : at(s, n, "万") ? 10000 : 0;
        if (!unit) break;
        if (unit == 10000) { total = (total + section + (pending > 0 ? pending : 0)) * 10000; section = 0; }
        else section += (pending >= 0 ? pending : 1) * unit;
        pending = -1; last = unit; zero = false; n += 3; ++terms;
    }
    if (!terms || (!last && pending < 0)) return 0;
    if (pending >= 0) section += last >= 100 && !zero ? pending * (last / 10) : pending;
    cents = (total + section) * 100;
    // 点: decimals ("三十五点五").
    if (at(s, n, "点") && digit(s, n + 3) >= 0 && (s[n + 3] & 0x80)) { cents += digit(s, n + 3) * 10; n += 6; ++terms; if (digit(s, n) >= 0 && (s[n] & 0x80)) { cents += digit(s, n); n += 3; } }
    return n - i;
}
}

inline LedgerParse parseLedger(const char* heard) {
    using namespace ledger_text;
    const std::string s(heard ? heard : "");
    LedgerParse result;
    // Every number, with what follows it: a money word makes it the amount; else the last one that is not a count.
    struct Found { size_t from, to; int64_t cents; bool money; };
    Found best{}; bool found = false;
    static const char* const counts[]{"个", "杯", "份", "斤", "次", "张", "件", "瓶", "号", "点", "月", "日", "天", "人", "位", "顿", "碗", "盒", "包", "公里", "小时", "分钟", "岁", "楼"};
    for (size_t i = 0; i < s.size();) {
        int64_t cents; bool chinese; unsigned terms;
        size_t n = number(s, i, cents, chinese, terms);
        if (!n) { i += width(s, i); continue; }
        size_t k = i + n; bool money = false;
        if ((i >= 3 && at(s, i - 3, "￥")) || (i >= 2 && at(s, i - 2, "¥"))) money = true;
        // 块/元 (钱), then 毛/角 tenths and 分 hundredths, or a bare digit ("二十八块五", "3块5"), or 半.
        if (at(s, k, "块") || at(s, k, "元")) {
            money = true; k += 3; if (at(s, k, "钱")) k += 3;
            if (at(s, k, "半")) { cents += 50; k += 3; }
            else if (k < s.size() && s[k] >= '0' && s[k] <= '9') {
                // "3块5" is 3.50, "3块50" too.
                size_t j = k; int v = 0; while (j < s.size() && s[j] >= '0' && s[j] <= '9' && j - k < 3) v = v * 10 + (s[j++] - '0');
                if (j - k == 1) { cents += v * 10; k = j; if (at(s, k, "毛") || at(s, k, "角")) k += 3; }
                else if (j - k == 2) { cents += v; k = j; }
            } else if (const int d = digit(s, k); d > 0) {
                cents += d * 10; k += 3; if (at(s, k, "毛") || at(s, k, "角")) k += 3;
                if (const int f = digit(s, k); f > 0 && (s[k] & 0x80)) { cents += f; k += 3; if (at(s, k, "分")) k += 3; }
            }
        } else if (at(s, k, "毛") || at(s, k, "角")) {
            money = true; cents /= 10; k += 3;
            if (const int f = digit(s, k); f > 0) { cents += f; k += width(s, k); if (at(s, k, "分")) k += 3; }
        }
        bool counted = false;
        for (const char* c : counts) counted |= at(s, k, c);
        // A lone Chinese digit is not an amount without a money word ("三明治", "五香").
        const bool usable = money || (!counted && (!chinese || terms > 1));
        if (usable && cents > 0 && (money || !(found && best.money))) { best = {i, k, cents, money}; found = true; }
        i = k;
    }
    if (!found || best.cents > 10000000000LL) return result;
    result.ok = true; result.cents = int32_t(best.cents);
    // Income, then the categories (the first match wins: 购物's 买 last, so 买咖啡 is 餐饮).
    static const char* const income[]{"工资", "奖金", "收入", "收到", "进账", "报销", "退款", "退钱", "利息", "分红", "收红包", "赚了", "收了"};
    struct Kind { uint8_t category; const char* words[28]; };
    static const Kind kinds[]{
        {6, {"红包", "礼物", "份子", "请客", "送礼", "随礼", "礼金"}},
        {5, {"药", "医院", "看病", "挂号", "体检", "牙医", "看牙", "诊所"}},
        {1, {"打车", "滴滴", "出租", "地铁", "公交", "加油", "油费", "停车", "高铁", "火车", "动车", "机票", "飞机", "车费", "过路", "高速", "单车", "车票", "船票"}},
        {3, {"房租", "租金", "水费", "电费", "燃气", "煤气", "物业", "话费", "宽带", "网费", "充值", "维修", "水电"}},
        {4, {"电影", "游戏", "唱歌", "KTV", "演唱会", "门票", "旅游", "酒店", "健身", "会员", "按摩", "景点", "演出"}},
        {7, {"书", "课", "培训", "学费", "考试", "文具"}},
        {0, {"早饭", "早餐", "午饭", "午餐", "中饭", "晚饭", "晚餐", "夜宵", "宵夜", "饭", "餐", "吃", "外卖", "咖啡", "奶茶", "饮料", "茶", "酒", "水果",
             "零食", "面", "火锅", "烧烤", "食堂", "汉堡", "炸鸡", "包子"}},
        {2, {"买", "超市", "淘宝", "京东", "拼多多", "衣服", "裤子", "鞋", "网购", "商场", "日用", "化妆"}},
    };
    const auto has = [&](const char* w) { return s.find(w) != std::string::npos; };
    bool earned = false;
    for (const char* w : income) earned |= has(w);
    if (earned) result.category = kLedgerIncome;
    else for (const auto& kind : kinds) {
        bool hit = false;
        for (const char* w : kind.words) if (w) hit |= has(w);
        if (hit) { result.category = kind.category; break; }
    }
    // The note: what is left without the amount and filler words, punctuation and spaces.
    std::string note = s.substr(0, best.from) + " " + s.substr(best.to);
    static const char* const fillers[]{"花了", "用了", "花费", "一共", "总共", "共计", "支出", "记账", "记一笔", "块钱", "元钱", "人民币", "￥", "¥"};
    for (const char* f : fillers) for (size_t p; (p = note.find(f)) != std::string::npos;) note.erase(p, strlen(f));
    std::string clean; unsigned chars = 0; bool gap = false;
    static const char* const marks[]{"，", "。", "、", "！", "？", "；", "：", "　", "“", "”"};
    for (size_t i = 0; i < note.size() && chars < LedgerBook::kNoteChars;) {
        const auto c = static_cast<unsigned char>(note[i]); const size_t w = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        bool mark = c < 0x80 && !isalnum(c);
        for (const char* m : marks) mark |= at(note, i, m);
        if (mark) { gap = !clean.empty(); i += w; continue; }
        if (gap) { clean += ' '; ++chars; gap = false; if (chars >= LedgerBook::kNoteChars) break; }
        clean.append(note, i, w); ++chars; i += w;
    }
    // A trailing 了 or a leading 我 adds nothing.
    if (clean.size() >= 3 && at(clean, clean.size() - 3, "了")) clean.erase(clean.size() - 3);
    if (at(clean, 0, "我")) clean.erase(0, 3);
    while (!clean.empty() && clean.back() == ' ') clean.pop_back();
    result.note = clean.empty() ? kLedgerCategories[result.category] : clean;
    return result;
}
// An amount for the page: "35.00", "1,234.50".
inline void ledgerAmount(int32_t cents, char* out, size_t size) {
    const long whole = cents / 100; char digits[16]; snprintf(digits, sizeof(digits), "%ld", whole);
    std::string grouped; const size_t n = strlen(digits);
    for (size_t i = 0; i < n; ++i) { if (i && (n - i) % 3 == 0) grouped += ','; grouped += digits[i]; }
    snprintf(out, size, "%s.%02ld", grouped.c_str(), long(cents % 100));
}
}
