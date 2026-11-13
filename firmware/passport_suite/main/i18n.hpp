#pragma once
// 语言 / Language (系统设置): the UI shows text through tr() when it is drawn; models, NVS and JSON keep Chinese,
// so code comparing Chinese text keeps working. The Chinese text is the key, looked up in three ways:
//  1. exactly ("已保存" -> "Saved");
//  2. against the printf formats that build it ("所在城市：%s" -> "City: %s"), the pieces moved into the English
//     format (in order, or by "%2$s"); a %s piece is itself looked up exactly;
//  3. split at " · " and each part looked up as above.
// Text without an entry stays as it is. kEnglish and kEnglishFormats are generated from
// firmware/passport_suite/i18n/en.json by scripts/gen_i18n.py.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <iterator>
namespace passport {
enum class Language : uint8_t { Chinese, English };
inline Language uiLanguage = Language::Chinese;  // Runtime::init loads it ("language" in suite_system)
struct Translation { const char* zh; const char* en; };
}
#include "i18n_en.hpp"
namespace passport {
constexpr bool byteLess(const char* a, const char* b) {
    for (; *a && *a == *b; ++a, ++b) {}
    return static_cast<unsigned char>(*a) < static_cast<unsigned char>(*b);
}
constexpr bool translationsSorted() {
    for (size_t i = 1; i < std::size(kEnglish); ++i) if (!byteLess(kEnglish[i - 1].zh, kEnglish[i].zh)) return false;
    return true;
}
static_assert(translationsSorted(), "run scripts/gen_i18n.py");

namespace i18n {
inline const char* exact(const char* zh) {
    const auto* at = std::lower_bound(std::begin(kEnglish), std::end(kEnglish), zh, [](const Translation& t, const char* s) { return strcmp(t.zh, s) < 0; });
    return at != std::end(kEnglish) && !strcmp(at->zh, zh) ? at->en : nullptr;
}
struct Piece { const char* at; size_t size; };
constexpr unsigned kPieces = 10;
// A printf conversion at f ("%s", "%02u", "%.1f", "%lu", "%.*s"): its length, and whether it prints a number.
inline size_t spec(const char* f, bool& number) {
    size_t n = 1;
    while (f[n] && strchr("-+ 0#123456789.*$lhz", f[n])) ++n;
    number = f[n] != 's' && f[n] != 'c';
    return f[n] ? n + 1 : 0;
}
inline bool numberChar(char c) { return (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == ' '; }
// Does s read as format f? Shortest pieces first, backtracking.
inline bool match(const char* f, const char* s, Piece* pieces, unsigned n) {
    for (;;) {
        if (!*f) return !*s;
        if (f[0] == '%' && f[1] == '%') { if (*s != '%') return false; f += 2; ++s; continue; }
        if (*f != '%') { if (*f != *s) return false; ++f; ++s; continue; }
        bool number = false; const size_t length = spec(f, number);
        if (!length || n == kPieces) return false;
        for (size_t size = number ? 1 : 0;; ++size) {
            if (size > strlen(s) || (number && size && !numberChar(s[size - 1]))) return false;
            pieces[n] = {s, size};
            if (match(f + length, s + size, pieces, n + 1)) return true;
        }
    }
}
// CJK text (U+3000-U+9FFF and the full-width forms), not symbols such as … ○ →.
inline bool chinese(const char* s) {
    for (; *s; ++s) { const auto b = static_cast<unsigned char>(*s); if ((b >= 0xe3 && b <= 0xe9) || (b == 0xef && (static_cast<unsigned char>(s[1]) | 1) == 0xbd)) return true; }
    return false;
}
inline const char* part(const char* zh, char* out, size_t capacity, bool pieces = true);
// Whether format f has only number conversions ("%lu 分钟"): the only formats a piece is read with, so a spoken or
// typed piece ("今天中午吃什么") is never taken apart.
inline bool numbersOnly(const char* f) {
    bool any = false;
    for (; *f; ++f) if (*f == '%') { if (f[1] == '%') { ++f; continue; } bool number = false; const size_t n = spec(f, number); if (!n || !number) return false; any = true; f += n - 1; }
    return any;
}
// en with the pieces put in (a %s piece translated too: exactly, or by a number format one level down); false when out is too small.
inline bool fill(const char* en, const Piece* pieces, char* out, size_t capacity, bool translatePieces) {
    size_t used = 0; unsigned next = 0;
    const auto put = [&](const char* text, size_t size) { if (used + size >= capacity) return false; memcpy(out + used, text, size); used += size; return true; };
    for (const char* f = en; *f;) {
        if (f[0] == '%' && f[1] == '%') { if (!put("%", 1)) return false; f += 2; continue; }
        if (*f != '%') { if (!put(f, 1)) return false; ++f; continue; }
        bool number = false; const size_t length = spec(f, number);
        if (!length) return false;
        unsigned index = next++;
        if (f[1] >= '1' && f[1] <= '9' && f[2] == '$') index = f[1] - '1';
        if (index >= kPieces) return false;
        const Piece& p = pieces[index]; char piece[64], sub[96];
        const char* text = p.at; size_t size = p.size;
        if (!number && size < sizeof(piece)) {
            memcpy(piece, p.at, size); piece[size] = 0;
            if (const char* e = chinese(piece) ? (translatePieces ? part(piece, sub, sizeof(sub), false) : exact(piece)) : nullptr) { text = e; size = strlen(e); }
        }
        if (!put(text, size)) return false;
        f += length;
    }
    out[used] = 0; return true;
}
// One part: exactly, else by a format. ponytail: formats are tried in turn (a few hundred, only for text with no
// exact entry, while English is on); cache by label if a render ever shows it.
inline const char* part(const char* zh, char* out, size_t capacity, bool pieces) {
    if (const char* e = exact(zh)) return e;
    Piece found[kPieces];
    for (const auto index : kEnglishFormats) {
        const auto& t = kEnglish[index];
        if (!pieces && !numbersOnly(t.zh)) continue;
        if (match(t.zh, zh, found, 0) && fill(t.en, found, out, capacity, pieces)) return out;
    }
    return nullptr;
}
// A symbol such as ● ○ ■ ▶ (U+25A0-U+25FF) at s: its length, else 0.
inline size_t symbol(const char* s) { return static_cast<unsigned char>(s[0]) == 0xe2 && (static_cast<unsigned char>(s[1]) | 1) == 0x97 ? 3 : 0; }
// text split at " · ", each part translated without the spaces and symbols around it ("  ·  " pads with two);
// nullptr when nothing changed or out is too small.
inline const char* parts(const char* text, char* out, size_t capacity) {
    size_t used = 0; bool changed = false; char core[192], one[192];
    for (const char* at = text;;) {
        const char* end = strstr(at, " · "); const size_t size = end ? size_t(end - at) : strlen(at);
        size_t lead = 0, trail = 0;
        while (lead < size) { if (at[lead] == ' ') ++lead; else if (const size_t n = symbol(at + lead)) lead += n; else break; }
        while (trail < size - lead && at[size - 1 - trail] == ' ') ++trail;
        const size_t n = size - lead - trail;
        if (n >= sizeof(core)) return nullptr;
        memcpy(core, at + lead, n); core[n] = 0;
        const char* text = chinese(core) ? part(core, one, sizeof(one)) : nullptr;
        if (text) changed = true; else text = core;
        const size_t length = strlen(text);
        if (used + lead + length + trail + (end ? 4 : 0) >= capacity) return nullptr;
        memcpy(out + used, at, lead); used += lead; memcpy(out + used, text, length); used += length; memset(out + used, ' ', trail); used += trail;
        if (!end) break;
        memcpy(out + used, " · ", 4); used += 4; at = end + 4;  // " · " is 4 bytes (· is 2)
    }
    out[used] = 0;
    return changed ? out : nullptr;
}
}

#ifdef PASSPORT_SIM_ENGLISH
// The English simulator logs each text still shown in Chinese once ("I18N_MISS ...").
inline void reportChinese(const char* shown) {
    if (uiLanguage != Language::English || !shown || !i18n::chinese(shown)) return;
    static uint8_t seen[128]; uint32_t h = 2166136261u; for (const char* c = shown; *c; ++c) h = (h ^ uint8_t(*c)) * 16777619u;
    if (seen[h % 1024 / 8] & (1 << (h % 8))) return;  // ponytail: 1024-bit filter, a clash only hides a repeat
    seen[h % 1024 / 8] |= 1 << (h % 8);
    // Percent-encoded ASCII: the emulator's output reader can split a line inside a UTF-8 character and panic.
    char line[400]; size_t used = 0;
    for (const char* c = shown; *c && used + 4 < sizeof(line); ++c) {
        const auto b = static_cast<unsigned char>(*c);
        if (b >= 0x80 || b == '%' || b < 0x20) used += snprintf(line + used, sizeof(line) - used, "%%%02X", b); else line[used++] = char(b);
    }
    line[used] = 0;
    printf("I18N_MISS %s\n", line);  // one call: frame rows print from the same task
}
#else
inline void reportChinese(const char*) {}
#endif
// The text in the language picked. A text built from a format lives in one static buffer (valid until the next tr();
// the chat page has little heap to spare), so copy it if more than one is needed at once.
inline const char* tr(const char* zh) {
    if (uiLanguage == Language::Chinese || !zh || !*zh || !i18n::chinese(zh)) return zh;
    static char out[192];
    const char* whole = i18n::part(zh, out, sizeof(out));
    if (whole && !i18n::chinese(whole)) return whole;
    // Still Chinese: its " · " parts (or the text without a leading ● ○) one by one.
    char text[192]; snprintf(text, sizeof(text), "%s", whole ? whole : zh);
    if (const char* split = i18n::parts(text, out, sizeof(out))) return split;
    return whole ? whole : zh;
}
}
