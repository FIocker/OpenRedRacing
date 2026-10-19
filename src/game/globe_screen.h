// `GLOBESCN`: choosing a circuit on the turning globe.
//
//
// This is the "Quick Race" destination the main menu pushes, and it replaces the flat list
// `quick_race_screen` used to show. The behaviour is the user's own description of the
// original, 2026-09-23:
//
//   * nothing is written while nothing is chosen, and the globe turns on itself;
//   * left/right choose a circuit - THE FIRST CHOICE IS THE FIRST CIRCUIT, Kenya, which is
//     `TOURN.TXT`'s own `single` order and what the catalogue already sorts by;
//   * the globe then swings round to the country, its hand-written name appears over it, and
//     the circuit's one-line description sits under it;
//   * OK - the passport stamp, bottom right - goes on to the presentation screen.
//
// ⚠️ LEFT AND RIGHT DO NOT MOVE THE FOCUS HERE, which is the one place this screen departs
// from `RmlUiScreen`'s shared input map: they turn the globe - the pad's left stick too.
// ⚠️ AND VALIDATING IS ALWAYS OK (Enter, the pad's bottom button, Start), whatever holds the
// focus: the stick or a resting mouse pointer used to move it onto an arrow, and the bottom
// button then chose the next circuit instead of validating (user, 2026-09-30). The arrows are
// still clickable, and left/right do what they do.
#pragma once

#include <string>
#include <vector>

#include "app/rml_screen.h"
#include "formats/localised_text.h"
#include "game/level_catalog.h"
#include "ui/background.h"
#include "ui/globe.h"
#include "ui/scn_kit.h"

namespace bigred {

class GlobeScreen : public RmlUiScreen {
public:
    GlobeScreen() = default;
    // A CHAMPIONSHIP ROUND (GLOBESCN's ROUND / FINAL mode in the original): only `only`'s
    // circuits can be chosen, and once a race of the competition has come back the globe
    // closes itself - the result belongs to the championship screen underneath.
    explicit GlobeScreen(std::vector<std::string> only) : only_(std::move(only)) {}
    bool enter(App& app) override;
    void exit(App& app) override;
    void resumed(App& app) override;
    bool handle_event(App& app, const SDL_Event& e) override;
    void update(App& app, double dt_ms) override;
    void draw(App& app) override;
    const char* name() const override { return "globe"; }

protected:
    const char* document_source() const override;
    const char* context_name() const override { return "globe"; }
    void on_ready(App& app) override;
    void on_activate(App& app, const std::string& id) override;
    // Choosing a series: Escape takes back the last race chosen, and only leaves once none is.
    void on_back(App& app) override;

private:
    bool picking_series(App& app) const;
    void choose(App& app, int delta);
    void refresh(App& app);       // writes the selection into the document
    void refresh_widget_frames(); // the wibble, and nothing else
    void register_widgets(App& app);
    const LevelEntry* current() const;

    std::vector<LevelEntry> levels_;
    std::vector<std::string> only_;
    TextTable text_;
    ScnScreen scn_;   // GLOBESCN through THIRD3.RAW: its wibbly title's colours
    Globe globe_;
    Background background_;

    int level_ = 0;
    // ⚠️ Nothing is chosen until the player says so, and that is not the same as "index 0 is
    // chosen": the original writes nothing at all on the globe until then.
    bool chosen_ = false;
    // The left stick's two directions, re-armed once it comes back towards the centre.
    bool stick_armed_neg_ = true;
    bool stick_armed_pos_ = true;

    float yaw_ = 0.0f, pitch_ = 0.0f;         // where the globe IS, in turns
    float yaw_target_ = 0.0f, pitch_target_ = 0.0f;
    double anim_ms_ = 0.0;                    // drives the arrows' and the stamp's wibble
    int widget_frame_ = 0;
};

} // namespace bigred
