// The championship: seven rounds of `TOURN.TXT`, won one race at a time.
//
//
// The rule is the original's, in its championship mode:
//
//   * a round offers the circuits of its `tourn N` section that have NOT BEEN RACED yet in this
//     championship (the original skips a circuit whose "raced" flag is set);
//   * after each race the circuit is marked raced, won or lost, and the race
//     counts as a WIN only if the player finished FIRST;
//   * as soon as the wins reach the round's `need`, the round is over (the next-round
//     routine: wins and races back to zero, round + 1) - or, if it was the
//     last round, the championship is WON (the original's result code 0x12);
//   * as soon as the circuits left in the round could no longer bring the wins up to `need`,
//     the championship is LOST (code 0x13).
//
// No state is saved between sessions: the original keeps it in memory only.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "formats/tourn.h"

namespace bigred {

class Championship {
public:
    enum class Outcome {
        Continue,     // another race in this round
        NextRound,    // `need` reached: the next round starts
        Champion,     // `need` reached in the last round
        Eliminated,   // `need` can no longer be reached
    };

    // Starts afresh, as the original does. `playable` filters out the circuits that cannot be loaded
    // (RUF1/2/3 ship without their terrain): a round that names one simply offers one fewer,
    // and its `need` is capped so that the round can still be won.
    void start(const std::vector<TournRound>& rounds, const std::vector<std::string>& playable);

    // ⚠️ OURS, on the user's request (2026-09-25): the same championship, DRAWN AT RANDOM. The
    // shipped rounds go easy -> hard: rounds 1-2 easy, 3-4 medium (one easy quarry in round
    // 3), round 5 half medium half hard, round 6 and the final hard. So the circuits the
    // shipped rounds use (and that load) are shuffled WITHIN their difficulty, laid end to end
    // easy, medium, hard, and cut into rounds of the shipped sizes with the shipped `need`s.
    // With the 21 playable circuits (5 / 9 / 7) that reproduces the original's spread exactly,
    // with other circuits in it. `difficulty` gives each circuit's 0/1/2, -1 = unknown (kept in
    // the shipped round's place). `seed` makes it repeatable.
    void start_random(const std::vector<TournRound>& rounds,
                      const std::vector<std::pair<std::string, int>>& playable_difficulty,
                      uint32_t seed);
    // The rounds as they stand, for the screen and the bench.
    const std::vector<TournRound>& round_list() const { return rounds_; }

    bool valid() const { return !rounds_.empty(); }
    bool over() const { return over_; }
    bool won() const { return won_; }
    int round() const { return round_; }                 // 0-based
    int rounds() const { return static_cast<int>(rounds_.size()); }
    bool final_round() const { return round_ + 1 >= rounds(); }
    int wins() const { return wins_; }
    int need() const;
    int raced() const { return raced_; }
    int round_size() const;                              // circuits in the round
    // The circuits the player may choose from now, in the file's order.
    std::vector<std::string> available() const;

    // One race of `level`, finished in `place` (1 = first). Anything else - a race abandoned
    // with ESC - is not a race and must not be recorded.
    Outcome record(const std::string& level, int place);

private:
    std::vector<TournRound> rounds_;
    std::vector<std::string> done_;   // the circuits already raced, as names
    int round_ = 0;                   // 0-based
    int wins_ = 0;                    // in this round
    int raced_ = 0;                   // in this round
    bool over_ = false, won_ = false;
};

} // namespace bigred
