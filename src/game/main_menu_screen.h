// THE main menu: the five entries CONFSCN itself names - Quick Race, Championship, Multiplayer,
// Options, Quit - each a push onto the screen stack. This is the ROOT screen `openbigred`
// pushes once a profile exists (game/profile_screen.h handles the case where one does not).
//
//
// ⚠️ THIS USED TO BE `quick_race_screen.h`'s job (as `MenuScreen`): drawing straight on
// `CONFSCN.RAW` with a circuit list. That screen is now the "Quick Race" DESTINATION this one
// pushes; this file is the graph's actual root, matching the original's own `CONFSCN` - "1P
// Single Race / 1P Tournament / Multi Player / Load Recording / Options" - at one entry's
// remove.
#pragma once

#include <string>

#include "app/rml_screen.h"
#include "formats/localised_text.h"
#include "ui/scn_kit.h"

namespace bigred {

class MainMenuScreen : public RmlUiScreen {
public:
    void resumed(App& app) override;
    const char* name() const override { return "main-menu"; }
    // Builds the document from CONFSCN and the player's language before the base class loads it.
    bool enter(App& app) override;

protected:
    const char* document_source() const override;
    const char* context_name() const override { return "main-menu"; }
    void on_ready(App& app) override;
    void on_activate(App& app, const std::string& id) override;
    // The root of the stack: Escape here quits (rule 1 of enchainement-des-menus.md), which the
    // base class's default on_back() (app.pop()) already does correctly once this is the only
    // screen left - popping the last screen empties the stack and App::run() exits. No override
    // needed.

private:
    ScnScreen scn_;
    TextTable text_;
    std::string doc_;
};

} // namespace bigred
