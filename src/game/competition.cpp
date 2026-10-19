#include "competition.h"

#include <cstdio>
#include <utility>

namespace bigred {

void Competition::end() {
    kind_ = Kind::Single;
    champ_ = Championship();
    series_ = LocalSeries();
    pending_ = false;
    leaving_ = false;
    last_ = Taken();
}

void Competition::start_championship(Championship c) {
    end();
    kind_ = Kind::Championship;
    champ_ = std::move(c);
}

void Competition::start_series(int seats, int races, bool random_races) {
    end();
    kind_ = Kind::LocalSeries;
    series_.races = races;
    series_.random_races = random_races;
    series_.points.assign(static_cast<size_t>(seats), 0);
}

void Competition::race_over(const std::string& level, int place) {
    if (kind_ != Kind::Championship || place <= 0) return;
    last_ = Taken();
    last_.level = level;
    last_.place = place;
    last_.round_before = champ_.round();
    last_.outcome = champ_.record(level, place);
    pending_ = true;
    std::printf("[championship] %s, place %d -> round %d, wins %d/%d%s\n", level.c_str(), place,
                champ_.round() + 1, champ_.wins(), champ_.need(),
                champ_.over() ? (champ_.won() ? ", WON" : ", OVER") : "");
    std::fflush(stdout);
}

void Competition::replay_race() {
    if (kind_ != Kind::LocalSeries) return;
    for (size_t i = 0; i < series_.last_race.size() && i < series_.points.size(); ++i)
        series_.points[i] -= series_.last_race[i];
    series_.last_race.clear();
}

bool Competition::take_race(Taken& out) {
    if (!pending_) return false;
    pending_ = false;
    out = last_;
    return true;
}

std::string Competition::results_heading() const {
    if (kind_ != Kind::Championship || last_.place <= 0) return {};
    using O = Championship::Outcome;
    switch (last_.outcome) {
    case O::Champion: return "YOU ARE THE CHAMPION";
    case O::Eliminated: return "ELIMINATED - NOT ENOUGH WINS LEFT";
    case O::NextRound:
        return last_.round_before + 2 >= champ_.rounds()
                   ? std::string("ROUND ") + std::to_string(last_.round_before + 1) +
                         " COMPLETE - ON TO THE FINAL"
                   : std::string("ROUND ") + std::to_string(last_.round_before + 1) + " COMPLETE";
    case O::Continue: break;
    }
    const std::string round = champ_.final_round() ? std::string("THE FINAL")
                                                   : "ROUND " + std::to_string(champ_.round() + 1);
    return round + " - WINS " + std::to_string(champ_.wins()) + " OF " +
           std::to_string(champ_.need());
}

} // namespace bigred
