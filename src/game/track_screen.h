// `TRAKSCN`: the circuit's presentation, and the three ways out of it.
//
//
// It is what the globe pushes once a circuit is chosen, and it is where the user wants the
// circuit's INTRO to be heard (2026-09-23): the jingle starts here, and RACE cuts it instead of
// letting it finish - `RaceScreen::Setup::skip_intro`.
//
//   CAR   -> game/car_screen.h, which writes its answer back into `vehicle_` below
//   VIEW  -> the same `RaceScreen` with `fly_by` set: the circuit flown on rails, nobody
//            driving. Escape comes back HERE, not to the menu, which is the user's own
//            requirement and what a screen stack gives for nothing.
//   RACE  -> the race
//
// ⚠️ ITS BACKGROUND IS THE LEVEL'S OWN `VIEW.RAW`, not one of its own, and that is why
// `TRAKSCN.SCN` ships no `.RAW`: the original loads the circuit's `VIEW.TXT` first (`raw
// VIEW.RAW`), then layers this script over it. Which also means `TRAKSCN.SPR`'s buttons are
// palettised by the LEVEL, not by a shared screen palette.
#pragma once

#include <string>
#include <vector>

#include "app/rml_screen.h"
#include "formats/localised_text.h"
#include "game/level_catalog.h"
#include "ui/scn_kit.h"

namespace bigred {

class TrackScreen : public RmlUiScreen {
public:
    // Once a race of a competition run from here is over, this screen steps aside by itself:
    // the result belongs to the competition's own screen (game/competition.h).
    // Set when a race is started from here, so that coming back knows it (a series moves on).
    bool raced_ = false;
    explicit TrackScreen(LevelEntry level) : level_(std::move(level)) {}

    bool enter(App& app) override;
    void resumed(App& app) override;
    const char* name() const override { return "track"; }

protected:
    const char* document_source() const override;
    const char* context_name() const override { return "track"; }
    void on_ready(App& app) override;
    void on_activate(App& app, const std::string& id) override;

private:
    std::string car_label() const;
    void start_intro(App& app);
    void start_race(App& app);

    LevelEntry level_;
    TextTable text_;
    // Which of the circuit's own cars is going to be driven. Index into `level_.cars`; the car
    // screen edits it through a pointer to this member, which is safe because this screen is
    // underneath it on the stack for the whole of its life. 0 is the level's own first `car`,
    // which is what `openbigred <LEVEL>` has always raced with.
    int vehicle_ = 0;

    // TRAKSCN with the circuit's VIEW.RAW behind it: the picture, the plates' palette, the
    // labels' colours. Built in enter(), the document with it.
    ScnScreen scn_;
    std::string doc_;
    bool intro_started_ = false;
    // A split screen: set by the split vehicle screen once everyone has chosen, and read when
    // this screen is back on top - that is when the race starts.
    bool split_go_ = false;
    // Entered on a split screen: no CAR plate, the vehicles are chosen in four after RACE.
    bool split_ = false;
};

} // namespace bigred
