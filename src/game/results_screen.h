// "Results": place, total time, one line per lap - what RESULSCN shows once every racer has
// finished. Pushed by race_screen.cpp the tick `RaceState::race_over()` turns true.
//
//
// It is handed DATA - every racer's place, time, state and points - and lays it out itself; the
// user's brief (2026-09-28): everyone's time, the active player's laps, the points so far.
//
// AFTER THE RACE: RESULSCN's OK goes on - back to the circuit, the
// next race of a series, the championship. Two buttons of our own may sit beside it: RACE AGAIN
// (a race on its own or a series, never a championship) and QUIT <the competition>.
//
// ⚠️ NOT WIRED: RESULSCN's "SAVE recording" button. Writing a `.REC` is not built yet: Load
// Recording is postponed, not abandoned.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/rml_screen.h"
#include "formats/localised_text.h"
#include "ui/scn_kit.h"

namespace bigred {

class ResultsScreen : public RmlUiScreen {
public:
    struct Setup {
        std::string level_title;          // "JAPAN" - LevelEntry::title, not the file name
        int laps = 0;
        int place = 1;                     // 1-based
        uint32_t finish_ticks = 0;          // total race time, 14 ms units (sim/race.h)
        std::vector<uint32_t> lap_ticks;    // the ACTIVE player's laps, one per completed lap
        // EVERYONE, as data: the screen sorts by place and lays
        // it out. A ghost is named "GHOST <vehicle>"; one still running has no time.
        struct Row {
            enum class Status { Finished, Running, Wrecked, Left };
            int place = 0;
            std::string name;
            uint32_t finish_ticks = 0;   // valid when Finished
            Status status = Status::Running;
            bool local = false;          // a player at this machine, shown highlighted
            // A series' points: this race's and the total after it. -1 = not a series.
            int race_points = -1, total_points = -1;
        };
        std::vector<Row> rows;
        // The competition's line - "RACE 2 OF 3" - or empty for a race on its own.
        std::string heading;
        // A test with no hand on the keyboard (openbigred -NETHOST/-NETJOIN): CONTINUE is
        // pressed by itself after that many milliseconds. 0 = wait for the player.
        double auto_continue_ms = 0.0;

        // AFTER THE RACE. `replay` builds the same race again - empty: no RACE AGAIN. It is
        // pushed in place of this screen and the race under it, and the competition strikes the
        // race just run out first (Competition::replay_race).
        std::function<std::unique_ptr<Screen>()> replay;
        // QUIT's words ("QUIT SERIES") - empty: no QUIT, OK is the only way on. It leaves the
        // competition (Competition::leave).
        std::string quit_label;
        // OK leaves the competition too: a series whose last race this was.
        bool ok_leaves = false;
        // With auto_continue_ms: RACE AGAIN is what gets pressed, not OK (-REPLAYS=N).
        bool auto_replay = false;
    };
    explicit ResultsScreen(Setup setup) : setup_(std::move(setup)) {}

    bool enter(App& app) override;
    void update(App& app, double dt_ms) override;
    const char* name() const override { return "results"; }

protected:
    const char* document_source() const override { return doc_source_.c_str(); }
    const char* context_name() const override { return "results"; }
    void on_ready(App& app) override;
    void on_activate(App& app, const std::string& id) override;
    // Escape does what CONTINUE does here - this is a summary to acknowledge, not a screen with
    // its own state to lose by leaving it.
    void on_back(App& app) override;

private:
    void leave(App& app);

    Setup setup_;
    std::string doc_source_;
    // RESULSCN: its picture, RES_RESULT wibbling at the top, its animated OK.
    ScnScreen scn_;
    TextTable text_;
    double shown_ms_ = 0.0;
    bool left_ = false;
};

} // namespace bigred
