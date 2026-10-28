#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
// 语音计算器: a spoken sum worked out on the badge, and how its result reads.
//   "128乘以37" -> 128 × 37 = 4,736; "一百除以七"; "2的10次方"; "根号2"; "三分之一加二分之一"; "1000打八五折";
//   "200的百分之十五"; "(3+5)×2"; "三万五减两千八"; a sum that starts with an operator ("再乘以3", "减100") goes on
//   from the last result.
namespace passport::calc {
// A worked sum: its value and the sum as the badge understood it ("2^10", "√(3 + 5)").
struct Row { double value; char shown[96]; };
struct Result { bool ok; double value; std::string shown; const char* error; };

enum class T : uint8_t { Num, Add, Sub, Mul, Div, Pow, Open, Close, Percent, PercentOf, Square, Cube, Root, RootAfter, CubeRootAfter,
                         Of, PowerEnd, Fold, Fraction, Discount, DiscountEnd, Ans, Skip };
// said: 减 spoken as a word, so a sum that starts with it goes on from the last result ("负" and "-" start a negative).
struct Token { T t; double v; bool said; };
struct Word { const char* text; T t; bool said = false; };
// The longest word at a place wins; asking words and fillers are skipped (before the numbers: 一下, 一共).
inline constexpr Word kWords[]{
    {"等于多少",T::Skip},{"等于几",T::Skip},{"是多少",T::Skip},{"得多少",T::Skip},{"多少",T::Skip},{"等于",T::Skip},{"请问",T::Skip},
    {"帮我",T::Skip},{"算一算",T::Skip},{"算一下",T::Skip},{"计算一下",T::Skip},{"计算",T::Skip},{"算算",T::Skip},{"一下",T::Skip},
    {"算",T::Skip},{"小智",T::Skip},{"一共",T::Skip},{"总共",T::Skip},{"然后",T::Skip},{"接着",T::Skip},{"再",T::Skip},{"是",T::Skip},
    {"=",T::Skip},{"＝",T::Skip},
    {"上一个结果",T::Ans},{"上个结果",T::Ans},{"刚才的结果",T::Ans},{"结果",T::Ans},{"得数",T::Ans},{"答案",T::Ans},
    {"加上",T::Add},{"加",T::Add},{"+",T::Add},{"＋",T::Add},
    {"减去",T::Sub,true},{"减",T::Sub,true},{"-",T::Sub},{"−",T::Sub},{"－",T::Sub},{"负",T::Sub},
    {"乘以",T::Mul},{"乘上",T::Mul},{"乘",T::Mul},{"×",T::Mul},{"*",T::Mul},{"x",T::Mul},{"X",T::Mul},
    {"除以",T::Div},{"除",T::Div},{"÷",T::Div},{"/",T::Div},
    {"^",T::Pow},{"(",T::Open},{"（",T::Open},{"左括号",T::Open},{")",T::Close},{"）",T::Close},{"右括号",T::Close},
    {"%",T::Percent},{"％",T::Percent},{"百分之",T::PercentOf},
    {"的平方根",T::RootAfter},{"平方根",T::RootAfter},{"开平方",T::RootAfter},{"开根号",T::RootAfter},{"开方",T::RootAfter},
    {"的立方根",T::CubeRootAfter},{"立方根",T::CubeRootAfter},{"开立方",T::CubeRootAfter},
    {"的平方",T::Square},{"平方",T::Square},{"的立方",T::Cube},{"立方",T::Cube},{"根号下",T::Root},{"根号",T::Root},{"√",T::Root},
    {"的",T::Of},{"次方",T::PowerEnd},{"次幂",T::PowerEnd},{"倍",T::Fold},{"分之",T::Fraction},{"打",T::Discount},{"折",T::DiscountEnd}};

// The code point at s (moving s past it).
inline char32_t next(const char*& s) {
    const auto c = static_cast<unsigned char>(*s);
    const int n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    char32_t cp = n == 1 ? c : n == 2 ? c & 31u : n == 3 ? c & 15u : c & 7u;
    for (int i = 1; i < n && (s[i] & 0xc0) == 0x80; ++i) cp = cp << 6 | (s[i] & 63u);
    for (int i = 0; i < n && *s; ++i) ++s;
    return cp;
}
inline int digitOf(char32_t c) {
    if (c >= '0' && c <= '9') return int(c - '0');
    switch (c) {
    case U'零': case U'〇': return 0; case U'一': case U'幺': return 1; case U'二': case U'两': return 2; case U'三': return 3;
    case U'四': return 4; case U'五': return 5; case U'六': return 6; case U'七': return 7; case U'八': return 8; case U'九': return 9;
    default: return -1;
    }
}
inline double unitOf(char32_t c) { return c == U'十' ? 10 : c == U'百' ? 100 : c == U'千' ? 1e3 : c == U'万' ? 1e4 : c == U'亿' ? 1e8 : 0; }
inline bool numeral(char32_t c) { return digitOf(c) >= 0 || unitOf(c) || c == U'点' || c == '.' || c == ','; }

// A spoken or written number: "128", "1,000", "3.14", "一百零五", "二零二六", "三万五" (35000), "1.5万", "两千五".
// NAN when it is not one.
inline double number(const std::u32string& run) {
    double big = 0, section = 0, n = 0, unit = 0;
    bool have = false, chained = false, zero = false, point = false; unsigned since = 0;  // digits since the last unit
    for (size_t i = 0; i < run.size();) {
        const char32_t c = run[i];
        if (c >= '0' && c <= '9') {
            std::string a;
            for (; i < run.size() && ((run[i] >= '0' && run[i] <= '9') || run[i] == ',' || run[i] == '.'); ++i) if (run[i] != ',') a += char(run[i]);
            if (std::count(a.begin(), a.end(), '.') > 1) return NAN;
            point = a.find('.') != std::string::npos; n = strtod(a.c_str(), nullptr); have = true; chained = false; since += unsigned(a.size());
            continue;
        }
        if (const int d = digitOf(c); d >= 0) {
            if (!d && !chained && unit) zero = true;
            n = chained ? n * 10 + d : d; have = chained = true; ++since; ++i;
            continue;
        }
        if (c == U'点' || c == '.') {
            double scale = .1;
            for (++i; i < run.size() && digitOf(run[i]) >= 0; ++i, scale /= 10) n += digitOf(run[i]) * scale;
            have = point = true; chained = false;
            continue;
        }
        const double u = unitOf(c); ++i;
        if (!u) return NAN;
        if (u < 1e4) section += (have ? n : 1) * u;
        else if (u == 1e4) { big += (section + (have || section ? n : 1)) * 1e4; section = 0; }
        else { big = (big + section + (have || section || big ? n : 1)) * 1e8; section = 0; }
        n = 0; have = chained = zero = point = false; since = 0; unit = u;
    }
    // 三万五, 一百五, 两千五: one digit after a unit counts in the next unit down.
    if (since == 1 && unit >= 100 && !zero && !point) n *= unit == 1e4 ? 1e3 : unit == 1e8 ? 1e7 : unit / 10;
    return big + section + n;
}

// What was heard as tokens; false when a number in it is not one.
inline bool lex(const char* heard, std::vector<Token>& out) {
    for (const char* s = heard; *s;) {
        const Word* best = nullptr; size_t length = 0;
        for (const auto& w : kWords) if (const size_t n = strlen(w.text); n > length && !strncmp(s, w.text, n)) { best = &w; length = n; }
        if (best) { if (best->t != T::Skip) out.push_back({best->t, 0, best->said}); s += length; continue; }
        const char* at = s; const char32_t c = next(at);
        // A number starts with a digit, 十, or a point before a digit; units, points and commas carry it on.
        const auto digitAt = [](const char* p) { return digitOf(next(p)) >= 0; };
        if (digitOf(c) >= 0 || c == U'十' || ((c == '.' || c == U'点') && digitAt(at))) {
            std::u32string run(1, c); s = at;
            for (const char* p = s; *p;) {
                // Stop at a filler that starts with a numeral (一下, 一共). 三百分之一 stays 三百 分之 一.
                bool word = false;
                for (const auto& w : kWords) if (w.t == T::Skip && strlen(w.text) > 3 && !strncmp(p, w.text, strlen(w.text))) { word = true; break; }
                if (word) break;
                const char* q = p; const char32_t d = next(q);
                if (!numeral(d) || ((d == ',' || d == '.' || d == U'点') && !digitAt(q))) break;
                run += d; p = s = q;
            }
            const double v = number(run);
            if (std::isnan(v)) return false;
            out.push_back({T::Num, v, false});
            continue;
        }
        s = at;  // anything else (个, 块, spaces) is left out
    }
    return true;
}

inline void trimZeros(char* s) {
    if (!strchr(s, '.')) return;
    char* end = s + strlen(s);
    while (end[-1] == '0') *--end = 0;
    if (end[-1] == '.') end[-1] = 0;
}
// A result as the badge shows it: up to 10 significant digits, thousands commas; very large or small in e-notation.
inline std::string format(double v) {
    char out[48]; const double a = std::fabs(v);
    if (a != 0 && (a >= 1e15 || a < 1e-6)) { snprintf(out, sizeof(out), "%.6g", v); return out; }
    const int digits = a > 0 ? int(std::floor(std::log10(a))) + 1 : 1;
    snprintf(out, sizeof(out), "%.*f", std::max(0, 10 - digits), v); trimZeros(out);
    std::string s = strcmp(out, "-0") ? out : "0";
    const size_t start = s[0] == '-', dot = std::min(s.find('.'), s.size());
    for (size_t i = dot; i > start + 3; i -= 3) s.insert(i - 3, ",");
    return s;
}
// A large result in 万 / 亿 / 万亿 ("4.736 万", "约 1.235 亿"); empty below 10,000.
inline std::string reading(double v) {
    const double a = std::fabs(v);
    if (a < 1e4 || a >= 1e16) return {};
    const double unit = a >= 1e12 ? 1e12 : a >= 1e8 ? 1e8 : 1e4;
    const double q = v / unit; char out[32];
    snprintf(out, sizeof(out), "%.*f", std::max(0, 4 - (int(std::floor(std::log10(std::fabs(q)))) + 1)), q); trimZeros(out);
    const bool exact = std::fabs(strtod(out, nullptr) * unit - v) <= 1e-9 * a;
    return std::string(exact ? "" : "约 ") + out + (unit == 1e12 ? " 万亿" : unit == 1e8 ? " 亿" : " 万");
}

struct Value { double v; std::string s; bool atom; bool percent; };
// Recursive descent: sum := term (+|- term)*; term := unary (×|÷ unary)*; unary := -unary | √unary | power;
// power := postfix (^ unary)?; postfix := primary (% | ² | ³ | 开方 | 分之 x | 打 x 折 | 的 x 次方 / 倍 / 百分之x)*.
struct Parser {
    const std::vector<Token>& t; const double* ans; size_t i = 0; const char* error = nullptr;
    T peek() const { return i < t.size() ? t[i].t : T::Skip; }
    Value fail(const char* e) { if (!error) error = e; return {0, {}, true, false}; }
    static std::string paren(const Value& v) { return v.atom ? v.s : "(" + v.s + ")"; }
    Value root(const Value& u) { return u.v < 0 ? fail("负数不能开平方") : Value{std::sqrt(u.v), "√" + paren(u), true, false}; }
    Value primary() {
        if (error) return fail(error);
        switch (peek()) {
        case T::Num: { const double v = t[i++].v; return {v, format(v), true, false}; }
        case T::Ans: ++i; return ans ? Value{*ans, format(*ans), true, false} : fail("还没有上一个结果");
        case T::Open: { ++i; Value e = sum(); if (peek() == T::Close) ++i; return {e.v, "(" + e.s + ")", true, false}; }
        case T::PercentOf: { ++i; const Value p = primary(); return {p.v / 100, paren(p) + "%", true, true}; }
        default: return fail("没听懂算式");
        }
    }
    Value postfix() {
        Value v = primary();
        while (!error) {
            const T k = peek();
            if (k == T::Percent) { ++i; v = {v.v / 100, paren(v) + "%", true, true}; }
            else if (k == T::Square || k == T::Cube) { ++i; v = {std::pow(v.v, k == T::Square ? 2 : 3), paren(v) + (k == T::Square ? "²" : "³"), true, false}; }
            else if (k == T::RootAfter) { ++i; v = root(v); }
            else if (k == T::CubeRootAfter) { ++i; v = {std::cbrt(v.v), "∛" + paren(v), true, false}; }
            else if (k == T::Fraction) {
                ++i; const Value top = primary();
                v = v.v == 0 ? fail("除数不能为 0") : Value{top.v / v.v, paren(top) + "/" + paren(v), false, false};
            } else if (k == T::Discount) {
                ++i; const Value d = primary(); if (peek() == T::DiscountEnd) ++i;
                const double f = d.v > 0 && d.v < 10 ? d.v / 10 : d.v >= 10 && d.v < 100 ? d.v / 100 : -1;
                v = f < 0 ? fail("折扣要说几折") : Value{v.v * f, paren(v) + " × " + format(f), false, false};
            } else if (k == T::Of) {
                ++i; const Value p = postfix();
                if (peek() == T::PowerEnd) { ++i; v = {std::pow(v.v, p.v), paren(v) + "^" + paren(p), true, false}; }
                else if (peek() == T::Fold) { ++i; v = {v.v * p.v, paren(v) + " × " + paren(p), false, false}; }
                else if (p.percent) v = {v.v * p.v, paren(v) + " × " + p.s, false, false};
                else return fail("没听懂算式");
            } else break;
        }
        return v;
    }
    Value power() {
        const Value b = postfix();
        if (peek() != T::Pow) return b;
        ++i; const Value e = unary();
        return {std::pow(b.v, e.v), paren(b) + "^" + paren(e), true, false};
    }
    Value unary() {
        if (peek() == T::Sub) { ++i; const Value u = unary(); return {-u.v, "-" + paren(u), false, false}; }
        if (peek() == T::Add) { ++i; return unary(); }
        if (peek() == T::Root) { ++i; return root(unary()); }
        return power();
    }
    Value term() {
        Value l = unary();
        // 2(3+4): a bracket straight after multiplies.
        for (T k; !error && ((k = peek()) == T::Mul || k == T::Div || k == T::Open);) {
            if (k != T::Open) ++i;
            const Value r = unary();
            if (k == T::Div && r.v == 0) return fail("除数不能为 0");
            l = {k == T::Div ? l.v / r.v : l.v * r.v, l.s + (k == T::Div ? " ÷ " : " × ") + r.s, false, false};
        }
        return l;
    }
    Value sum() {
        Value l = term();
        for (T k; !error && ((k = peek()) == T::Add || k == T::Sub);) {
            ++i; const Value r = term();
            l = {k == T::Add ? l.v + r.v : l.v - r.v, l.s + (k == T::Add ? " + " : " − ") + r.s, false, false};
        }
        return l;
    }
};

// ans: the last result (nullptr: none yet).
inline Result evaluate(const char* heard, const double* ans) {
    std::vector<Token> t;
    if (!lex(heard, t)) return {false, 0, {}, "没听懂数字"};
    // Nothing to work on: no number, and no last result to go on from.
    if (t.empty() || (!ans && std::none_of(t.begin(), t.end(), [](const Token& k) { return k.t == T::Num; }))) return {false, 0, {}, "没听到算式"};
    const T first = t[0].t;
    if (first != T::Num && first != T::Open && first != T::Root && first != T::PercentOf && first != T::Ans && !(first == T::Sub && !t[0].said))
        t.insert(t.begin(), Token{T::Ans, 0, false});
    Parser p{t, ans};
    const Value v = p.sum();
    if (!p.error && p.i != t.size()) p.error = "没听懂算式";
    if (!p.error && std::isnan(v.v)) p.error = "无法计算";
    if (!p.error && !std::isfinite(v.v)) p.error = "结果太大";
    if (p.error) return {false, 0, {}, p.error};
    return {true, v.v == 0 ? 0 : v.v, v.s, nullptr};
}
}
