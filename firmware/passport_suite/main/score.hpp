#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
namespace passport {
// 记分牌: a match is only the order in which the points were won (a set bit: 蓝方), replayed under its rule. So undo
// drops the last point, a game or match that ended is reopened by it, and the record stays small enough to save on
// every point (NVS "score_v1"), which keeps a match through sleep and power loss.
struct ScoreRule {
    const char* name; const char* note;
    uint8_t points;    // a game is won at this many with a two-point lead (0: no games, a plain count)
    uint8_t cap;       // ... or outright at this many (羽毛球 30; 0: none)
    uint8_t deciding;  // points to win the last possible game (排球 15)
    uint8_t games;     // games to win the match
    uint8_t serve;     // 0 none; 1 two serves each, one each from deuce, first server alternates by game (乒乓球);
                       // 2 the rally winner serves, a game's winner starts the next (羽毛球); 3 as 2, but the first server alternates (排球)
};
inline constexpr ScoreRule kScoreRules[]{
    {"自由计分","不限分数",0,0,0,0,0},
    {"乒乓球","5局3胜",11,0,11,3,1},
    {"羽毛球","3局2胜",21,30,21,2,2},
    {"排球","5局3胜",25,0,15,3,3},
};
struct ScoreBoard {
    static constexpr uint8_t kVersion = 1;
    static constexpr unsigned kPoints = 512, kMax = 999;  // points replayed; 自由计分 folds older ones into base
    uint8_t version{kVersion}, rule{};
    uint16_t count{}, base[2]{};
    uint8_t bits[kPoints / 8]{};
    bool valid() const { return version == kVersion && rule < std::size(kScoreRules) && count <= kPoints && (rule == 0 || (!base[0] && !base[1])) && base[0] <= kMax && base[1] <= kMax; }
    bool fresh() const { return !count && !base[0] && !base[1]; }
    unsigned side(unsigned i) const { return bits[i / 8] >> (i % 8) & 1; }
};
struct ScoreState {
    static constexpr uint8_t kGames = 7, kNone = 2;
    uint16_t points[2]{};                 // in the game being played (自由计分: the totals)
    uint8_t games[2]{}, done{};           // games won; games finished, their scores in history
    uint8_t history[kGames][2]{};
    uint8_t server{kNone}, winner{kNone}; // 0 红方, 1 蓝方
    uint8_t chance{kNone};                // the side one point from the game
    bool matchPoint{};                    // ... and that game would win the match
    uint8_t ended{kNone};                 // the last point ended a game: its winner (the match, if winner is set)
};
inline bool scoreGameWon(unsigned a, unsigned b, unsigned target, unsigned cap) {
    const unsigned hi = std::max(a, b), lo = std::min(a, b);
    return (hi >= target && hi - lo >= 2) || (cap && hi >= cap);
}
inline ScoreState scoreReplay(const ScoreBoard& s) {
    ScoreState t; const auto& r = kScoreRules[s.rule < std::size(kScoreRules) ? s.rule : 0];
    t.points[0] = s.base[0]; t.points[1] = s.base[1];
    uint8_t first = 0, last = 0;  // who served first in this game, who won the last point
    const auto target = [&] { return t.games[0] + t.games[1] + 1u == 2u * r.games - 1 ? r.deciding : r.points; };
    for (unsigned i = 0; i < s.count && t.winner == ScoreState::kNone; ++i) {
        const unsigned side = s.side(i); ++t.points[side]; last = static_cast<uint8_t>(side); t.ended = ScoreState::kNone;
        if (!r.points || !scoreGameWon(t.points[0], t.points[1], target(), r.cap)) continue;
        if (t.done < ScoreState::kGames) { t.history[t.done][0] = static_cast<uint8_t>(std::min<unsigned>(t.points[0], 255)); t.history[t.done][1] = static_cast<uint8_t>(std::min<unsigned>(t.points[1], 255)); }
        ++t.done; ++t.games[side]; t.points[0] = t.points[1] = 0; t.ended = static_cast<uint8_t>(side);
        if (t.games[side] == r.games) t.winner = static_cast<uint8_t>(side);
        first = r.serve == 2 ? static_cast<uint8_t>(side) : static_cast<uint8_t>(t.done % 2);
    }
    if (!r.points || t.winner != ScoreState::kNone) return t;
    const unsigned a = t.points[0], b = t.points[1], p = a + b, deuce = target() - 1u;
    if (r.serve == 1) t.server = static_cast<uint8_t>(first ^ ((a >= deuce && b >= deuce ? p - deuce : p / 2) & 1));
    else if (r.serve) t.server = p ? last : first;
    for (uint8_t side = 0; side < 2; ++side)
        if (scoreGameWon(a + (side == 0), b + (side == 1), target(), r.cap)) { t.chance = side; t.matchPoint = t.games[side] + 1u == r.games; }
    return t;
}
// A point for side 0/1; false when the match is over or the record is full (自由计分 never fills: it folds).
inline bool scoreAdd(ScoreBoard& s, unsigned side) {
    if (scoreReplay(s).winner != ScoreState::kNone) return false;
    if (s.count == ScoreBoard::kPoints) {
        if (s.rule) return false;
        unsigned blue = 0; for (unsigned i = 0; i < 8; ++i) blue += s.side(i);
        s.base[0] = static_cast<uint16_t>(s.base[0] + 8 - blue); s.base[1] = static_cast<uint16_t>(s.base[1] + blue);
        std::memmove(s.bits, s.bits + 1, sizeof(s.bits) - 1); s.bits[sizeof(s.bits) - 1] = 0; s.count -= 8;
    }
    if (!s.rule && scoreReplay(s).points[side] >= ScoreBoard::kMax) return false;
    if (side) s.bits[s.count / 8] |= 1u << (s.count % 8); else s.bits[s.count / 8] &= ~(1u << (s.count % 8));
    ++s.count; return true;
}
inline bool scoreUndo(ScoreBoard& s) { if (!s.count) return false; --s.count; s.bits[s.count / 8] &= ~(1u << (s.count % 8)); return true; }
}  // namespace passport
