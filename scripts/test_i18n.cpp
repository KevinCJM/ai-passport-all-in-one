// Host check of tr() (firmware/passport_suite/main/i18n.hpp) against the generated English table:
//   clang++ -std=c++20 -I firmware/passport_suite/main scripts/test_i18n.cpp -o /tmp/test_i18n && /tmp/test_i18n
#include "i18n.hpp"
#include <cassert>
#include <cstdio>
using namespace passport;
static bool is(const char* zh, const char* en) {
    const char* got = tr(zh);
    if (strcmp(got, en)) { printf("FAIL %s -> %s (want %s)\n", zh, got, en); return false; }
    return true;
}
int main() {
    assert(!strcmp(tr("已保存"), "已保存"));  // Chinese: untouched
    uiLanguage = Language::English;
    bool ok = is("已保存", "Saved")                                   // exact
        && is("所在城市：杭州", "City: Hangzhou")                          // format; its %s piece has an entry too
        && is("收听地区：广东", "Region: Guangdong")
        && is("%lu 分钟", "%lu min")                                  // a format itself
        && is("还有 12 分钟", "In 12 min")                            // number piece
        && is("距杭州 120 km", "120 km from Hangzhou")                    // positional %2$ %1$
        && is("本地 · 64k", "Local · 64k")                           // " · " parts
        && is("温哥华  ·  24 小时 · 当地", "温哥华  ·  24 h · local")  // padded parts
        && is("没有这条翻译", "没有这条翻译") && is("所在城市：温哥华", "City: 温哥华")                          // no entry: as it is
        && is("正在播放", "Playing") && is("Listening…", "Listening…")
        && is("%s  %s：%s", "%s  %s: %s")
        && is("●  按 IP 定位", "●  Locate by IP")
        && is("你好小智唤醒  开", "你好小智唤醒  开");                  // no such format: as it is
    // Pieces: a number piece by its format, a spoken one left whole; 7+ pieces; %.*s; leading symbols; parts after a format.
    ok = ok && is("○  息屏时间：6 分钟", "○  Screen off after: 6 min") && is("没听懂：今天中午吃什么", "Didn't get: 今天中午吃什么")
        && is("2026-10-24 周六 · 九月十五", "2026-10-24 Sa · 九月十五") && is("除数不能为 0：10除以0", "Can't divide by 0: 10除以0")
        && is("●  停止", "●  Stop") && is("1551 分 · 243 评论 · 刚刚", "1551 pts · 243 comments · just now");
    // A %s piece with its own entry is translated: "%s已%s" -> "%s %s".
    ok = ok && is("语音唤醒已打开", "Wake word On");
    for (const auto& t : kEnglish) if (i18n::chinese(t.en)) { printf("Chinese in English: %s\n", t.en); ok = false; }
    puts(ok ? "I18N_OK" : "I18N_FAIL");
    return ok ? 0 : 1;
}
