#pragma once
#include <cstdint>
#include <cstring>

namespace passport {
// Spaced repetition in Leitner boxes. Box 0 is a word never graded; recalling a word from the last box
// masters it and it leaves the deck, so the deck only holds the words still being learned.
struct WordCard { uint16_t word, due; uint8_t box; };
struct WordDeck {
    static constexpr uint8_t kVersion = 1, kMastered = 6, kNewPerDay = 10;
    static constexpr unsigned kCards = 160;
    uint8_t version{kVersion}, added{}, quota{kNewPerDay};  // new words added today / allowed today
    uint16_t day{}, next{}, mastered{}, count{};
    WordCard cards[kCards]{};
};
// Today's due cards; Up/Down move the cursor through them.
struct WordSession { uint8_t queue[WordDeck::kCards]{}; uint8_t size{}, cursor{}; bool revealed{}; };
inline constexpr uint16_t kWordDays[WordDeck::kMastered]{0, 1, 2, 4, 7, 15};  // box -> days until its review

// Local day number in China time, 0 while the clock is unset.
inline uint16_t wordDay(int64_t epoch) { return epoch > 1700000000 ? (epoch + 8 * 3600) / 86400 : 0; }

// Starts the day (0 keeps the deck's day), drops mastered cards, adds today's new words and queues every due card.
inline void wordsBegin(WordDeck& d, WordSession& s, uint16_t today, uint16_t total) {
    if (today > d.day) { d.day = today; d.added = 0; d.quota = WordDeck::kNewPerDay; }
    unsigned kept = 0;
    for (unsigned i = 0; i < d.count; ++i) if (d.cards[i].box < WordDeck::kMastered) d.cards[kept++] = d.cards[i];
    d.count = kept;
    while (d.added < d.quota && d.count < WordDeck::kCards && d.next < total) { d.cards[d.count++] = {d.next++, d.day, 0}; ++d.added; }
    s = {};
    for (unsigned i = 0; i < d.count; ++i) if (d.cards[i].due <= d.day) s.queue[s.size++] = i;
}
// Ten more new words once today's queue is done.
inline void wordsMore(WordDeck& d, WordSession& s, uint16_t total) { d.quota = d.added + WordDeck::kNewPerDay; wordsBegin(d, s, 0, total); }
inline const WordCard* wordsCurrent(const WordDeck& d, const WordSession& s) { return s.size ? &d.cards[s.queue[s.cursor]] : nullptr; }
inline void wordsMove(WordSession& s, int delta) { if (s.size) s.cursor = (s.cursor + s.size + delta) % s.size; s.revealed = false; }
// Known moves the card up a box (a new word known at first sight is mastered at once).
// Unknown sends it back to box 1 and to the end of today's queue.
inline void wordsGrade(WordDeck& d, WordSession& s, bool known) {
    if (!s.size) return;
    const uint8_t at = s.queue[s.cursor];
    auto& card = d.cards[at];
    memmove(s.queue + s.cursor, s.queue + s.cursor + 1, s.size - s.cursor - 1); --s.size;
    if (!known) { card.box = 1; card.due = d.day; s.queue[s.size++] = at; }
    else if (!card.box || card.box + 1 >= WordDeck::kMastered) { card.box = WordDeck::kMastered; ++d.mastered; }
    else { ++card.box; card.due = d.day + kWordDays[card.box]; }
    if (s.cursor >= s.size) s.cursor = 0;
    s.revealed = false;
}
}
