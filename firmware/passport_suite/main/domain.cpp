#include "suite.hpp"
#include <cstdio>
#include <cstring>

namespace passport {
void Model::notify(const char* text) {
    ++notificationsReceived;
    if (noticeCount == notices.size()) ++notificationsDropped;
    else ++noticeCount;
    snprintf(notices[noticeHead].data(), notices[noticeHead].size(), "%s", text);
    noticeHead = (noticeHead + 1) % notices.size();
    snprintf(lastNotice, sizeof(lastNotice), "%s", text);
}
void Model::tick(App active, uint64_t now) {
    if (active == App::Focus && focusRunning) {
        focusRemaining = now < focusDeadline ? focusDeadline - now : 0;
        if (!focusRemaining) {
            focusRunning = false;
            focusFinished = true;
            if(focusPhase==0)++focusRounds;
            notify(focusPhase==0?"专注完成，休息一下":"休息结束，开始下一轮");
        }
    }
}
void Model::action(App app, uint64_t now) {
    tick(app, now);
    switch (app) {
    case App::Focus:
        if (focusRunning) focusRunning = false;
        else {
            if(focusFinished){focusPhase=focusPhase==0?(focusRounds%4==0?2:1):0;focusDuration=focusPeriods[focusPhase];focusRemaining=focusDuration;}
            if (!focusRemaining) focusRemaining = focusDuration;
            focusDeadline = now + focusRemaining;
            focusRunning = true;
            focusFinished = false;
        }
        break;
    case App::Codex: if (!live) notify("Codex：等待你确认"); break;
    default: break;
    }
}
bool Model::selectFocus(int delta) {
    if(focusRunning)return false;
    focusPhase=(int(focusPhase)+delta+3)%3;focusDuration=focusPeriods[focusPhase];focusRemaining=focusDuration;focusFinished=false;return true;
}
} // namespace passport
