// The championship screen: the round, the wins against the round's `need`, and the circuits
// still to race. Choosing one opens its presentation screen, and from there the usual chain -
// vehicle, race, results. The race records its result in the session's Competition
// (game/competition.h) and the screens in between step aside; this one takes it back in
// resumed(). The rule itself is game/championship.h.
//
// The original chooses a round's circuit on GLOBESCN, in its ROUND / FINAL mode: "CHOOSE ON THE
// GLOBE" does that (GlobeScreen's round constructor). The list stays beside it, for speed.
#pragma once

#include <string>
#include <vector>

#include "app/rml_screen.h"
#include "formats/localised_text.h"
#include "ui/scn_kit.h"
#include "game/championship.h"
#include "game/competition.h"
#include "game/level_catalog.h"

namespace bigred {

class ChampionshipScreen : public RmlUiScreen {
public:
    bool enter(App& app) override;
    // Leaving the screen, by BACK or by Escape, leaves the championship: it lives no longer.
    void exit(App& app) override;
    void resumed(App& app) override;
    const char* name() const override { return "championship"; }

protected:
    const char* document_source() const override;
    const char* context_name() const override { return "championship"; }
    void on_ready(App& app) override;
    void on_activate(App& app, const std::string& id) override;

private:
    // CONFIG4B.RAW: the title's and the BACK row's colours.
    ScnScreen scn_;
    TextTable text_;
    std::string doc_;
    void rebuild();
    // A fresh championship: drawn at random by difficulty (the default), or the shipped rounds.
    void new_championship();
    bool random_ = true;
    const LevelEntry* entry(const std::string& level) const;

    std::vector<LevelEntry> levels_;
    std::vector<TournRound> rounds_;
    // The session's competition (App::competition), which holds the championship itself.
    Competition* comp_ = nullptr;
    Championship& champ() { return comp_->championship(); }
    std::string message_;        // what the last race did
};

} // namespace bigred
