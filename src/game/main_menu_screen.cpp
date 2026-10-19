#include "main_menu_screen.h"

#include <RmlUi/Core.h>

#include <memory>

#include "app/app.h"
#include "game/competition.h"
#include "formats/tourn.h"   // kFrontEndMusicTrack
#include "game/globe_screen.h"
#include "game/championship_screen.h"
#include "game/multiplayer_screen.h"
#include "game/options_screen.h"
#include "game/placeholder_screen.h"

namespace bigred {
namespace {

// CONFSCN as the original lays it out (CONFSCN.TXT): five boxes the width of the screen, each
// holding its line of TOURN.<LANG> at that line's own `@X`. The fourth line of the original is
// "Load Recording", which this game does not have; QUIT takes its box - the original quits on
// Escape, which this screen does too.
struct Row { const char* id; const char* key; int x, y, w, h; };
constexpr Row kRows[] = {
    {"quick_race", "CONFIG_SINGLE", 10, 40, 310, 24},
    {"championship", "CONFIG_TOURN", 10, 70, 310, 24},
    {"multiplayer", "CONFIG_2P", 10, 100, 310, 24},
    {"quit", nullptr, 10, 140, 310, 24},
    {"options", "CONFIG_OPT", 10, 170, 310, 24},
};


} // namespace

bool MainMenuScreen::enter(App& app) {
    scn_.load(app, "CONFSCN");
    text_.load(app.game_dir(), config_language(app.game_dir()));
    // The focused choice turns yellow - the one ramp every menu palette keeps yellow.
    doc_ = std::string("<rml><head><style>.hit:hover .st, .hit:focus .st { color: ") +
           scn_.ramp_colour(ScnScreen::kFocusRamp) + "; }</style></head><body>" +
           "<img id=\"bg\" src=\"" + scn_.background_src() + "\"/>";
    // The two wibble phases of CONFSCN (`anim 5,5,7, 2,3,4,5,6` and `4,5,6,2,3`), one row in
    // two, so neighbouring lines never wobble in step.
    int row = 0;
    for (const Row& r : kRows) {
        const std::string phase = (row++ % 2) ? "b" : "";
        doc_ += ScnScreen::box_open(r.id, r.x, r.y, r.w, r.h);
        doc_ += r.key ? scn_.text_rml(text_, r.key, r.x, r.y, phase)
                      : scn_.words_rml("QUIT", 120, r.y, -1, r.x, r.y, phase);
        doc_ += "</div>";
    }
    // No key hint: the original has none, and its fifth line runs to the bottom of the screen.
    doc_ += "</body></rml>";
    return RmlUiScreen::enter(app);
}

const char* MainMenuScreen::document_source() const { return doc_.c_str(); }

void MainMenuScreen::on_ready(App&) {
    set_focus_order({"quick_race", "championship", "multiplayer", "quit", "options"});
}

// Coming back from anywhere that changed the music (a race, eventually a championship round):
// the front end takes its own piece back. A no-op if it is already playing it.
// The split screen's seats go too: they belong to one trip through MULTIPLAYER, and a QUICK
// RACE afterwards is one player's again.
void MainMenuScreen::resumed(App& app) {
    app.locals().clear();
    app.competition().end();
    app.music().play_cd_track(kFrontEndMusicTrack);
}

void MainMenuScreen::on_activate(App& app, const std::string& id) {
    if (id == "quick_race") { app.push(std::make_unique<GlobeScreen>()); return; }
    if (id == "championship") {
        app.push(std::make_unique<ChampionshipScreen>());
        return;
    }
    if (id == "multiplayer") {
        app.push(std::make_unique<MultiplayerScreen>());
        return;
    }
    if (id == "options") { app.push(std::make_unique<OptionsScreen>()); return; }
    if (id == "quit") { app.quit(); return; }
}

} // namespace bigred
