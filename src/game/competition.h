// WHAT IS BEING PLAYED this session, and what comes next - the code review's A5.
//
// Before it, a championship's result travelled back through the screens as an `int*` handed
// from the championship to the globe, the presentation screen and the race, and each screen
// guessed in resumed() whether it was coming back from one. Now the RACE records its result
// here the moment it is over, the results screen reads the standing of the competition from
// here, and the screens in between only ask "has a race of it just finished?".
//
// It lives in the App for the session only: the user's rule, and the original's (no
// championship is ever saved).
#pragma once

#include <string>
#include <vector>

#include "game/championship.h"

namespace bigred {

class Competition {
public:
    enum class Kind {
        Single,         // a race on its own - nothing to carry from one race to the next
        Championship,   // the original's seven rounds (game/championship.h)
        LocalSeries,    // a split screen's series of races
    };
    Kind kind() const { return kind_; }

    // Back to a race on its own: the competition is left (BACK from its screen).
    void end();

    // --- the championship ------------------------------------------------------------------
    // Starts one; it replaces whatever was being played.
    void start_championship(Championship c);
    Championship& championship() { return champ_; }
    const Championship& championship() const { return champ_; }

    // --- a split screen's series ---------------------------------------------------------------
    // Player one picks up to three circuits on the globe, or has them drawn; the points of each
    // race (10 / 8 / 6 / 4 / 2 / 0) add up per seat. Out of LocalPlayers since 2026-09-28: the
    // seats are WHO plays, the series is WHAT is played.
    struct LocalSeries {
        int races = 1;
        bool random_races = false;
        std::vector<std::string> circuits;   // in order
        size_t next = 0;                     // the one being raced now
        std::vector<int> points;             // per seat, the series so far
        std::vector<int> last_race;          // per seat, what the race just run added to it
        bool over() const { return next + 1 >= circuits.size(); }
        // A new series on the same seats: no circuit chosen, no point scored.
        void restart() {
            circuits.clear();
            next = 0;
            points.assign(points.size(), 0);
            last_race.clear();
        }
    };
    // Starts one for `seats` players; it replaces whatever was being played. `races` 1 is an
    // ordinary split-screen race.
    void start_series(int seats, int races, bool random_races);
    LocalSeries& series() { return series_; }
    const LocalSeries& series() const { return series_; }

    // --- one race of the competition -------------------------------------------------------
    // Called by the race when it is over and its results are about to show - never for a race
    // left with ESC, which is not a race. Records it (a championship counts it at once).
    void race_over(const std::string& level, int place);
    // A race of the competition has finished and its organiser (the championship screen) has
    // not taken it yet: the screens between the two step aside.
    bool race_pending() const { return pending_; }
    // The organiser takes the race just run: which, where it finished, and what it did.
    struct Taken {
        std::string level;
        int place = 0;
        Championship::Outcome outcome = Championship::Outcome::Continue;
        int round_before = 0;   // the round it was raced in (0-based)
    };
    bool take_race(Taken& out);

    // --- after the race (the results screen's buttons) --------------------------------------
    // RACE AGAIN: the race just run is struck out - a series takes back the points it gave.
    // Never offered in a championship, where every race counts (user, 2026-09-28).
    void replay_race();
    // QUIT: the player leaves the competition from the results. The screens underneath see it
    // on their way back (resumed) and step aside down to the one that started it, which
    // ends it. Cleared by end() and by the series' globe once it has started over.
    void leave() { leaving_ = true; }
    bool leaving() const { return leaving_; }
    void left() { leaving_ = false; }

    // The competition's line on the results screen, after the race just recorded: "ROUND 2 -
    // WINS 1 OF 2", "ROUND 1 COMPLETE", "YOU ARE THE CHAMPION"... Empty for a race on its own.
    std::string results_heading() const;

private:
    Kind kind_ = Kind::Single;
    Championship champ_;
    LocalSeries series_;
    bool pending_ = false;
    bool leaving_ = false;
    Taken last_;
};

} // namespace bigred
