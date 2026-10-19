// `CARSCN`: which of the circuit's own vehicles is going to be driven.
//
//
// What it shows: the choice itself, written back into the presentation screen; THE VEHICLE,
// turning on the spot (ui/vehicle_display.h); and its SHEET - the machine's own name, what kind
// of machine it is, and the six statistics.
//
// ⚠️ THE SIX STATISTICS ARE NOT COMPUTED, and reading `VehicleTuning` to derive them is the
// wrong instinct twice over: they are hand-written per vehicle in `TOURN.<LANG>` (`VHC_<NAME>`,
// six lines of two `@M` references apiece), and they are qualitative words their translators
// were told not to translate.
//
// The TEAM COLOUR is chosen here too, which is the user's own request of 2026-09-23 - the
// original asks for it on `CHARSCN`, a screen away from the vehicle it paints. Clicking the
// label cycles, exactly as `CHARSCN`'s `col` does, and the model repaints as it goes.
//
// ⚠️ THE COLOUR IS THE PROFILE'S, not this screen's, so it is written back only on OK: BACK
// leaves the player with the colour they arrived with, like the vehicle choice itself.
//
// The TEAM LOGO is chosen here too, from the thirteen the game ships (ui/team_logos.h), on the
// hand-drawn plate `LOGOSCN` shows them on. Same rule as the colour: cyclic, and written back
// to the profile only on OK.
//
// ⚠️ IT IS NOT PAINTED ON THE BODYWORK YET. The original reserves faces for it - the `tag=6`
// records, which carry no colour - and putting an image on them needs the vehicle renderer to
// grow a textured path it does not have.
//
// ⚠️ STILL MISSING: a CUSTOM logo (a PNG dropped in the player's own folder, as designed), and the split screen that shows four of this screen at once.
//
// ⚠️ ITS BACKGROUND IS `CARSEL.RAW`, one per environment family and living in the `.ENV`s, not
// in `CARSCN.SCN` - which ships no `.RAW` at all. See load_artwork() for what it is and why its
// palette is also the vehicle's. It is drawn BY HAND in draw() rather than by the document,
// because the vehicle has to land between the artwork and the text - an `<img>` in the document
// would be painted over the car. Same trap as the globe's.
//
// ⚠️ THE LIST IS THE LEVEL'S OWN. A circuit names the vehicles it allows in its own script and
// nothing here may add to them - see game/level_catalog.h, and the `FEHELI` note there for why
// the presentation helicopter is not on it.
#pragma once

#include <string>
#include <vector>

#include "app/player_profile.h"
#include "app/rml_screen.h"
#include "ui/scn_kit.h"
#include "formats/localised_text.h"
#include "formats/threedw.h"   // RGB
#include "game/level_catalog.h"
#include "ui/background.h"
#include "ui/team_logos.h"
#include "ui/vehicle_display.h"

namespace bigred {

class CarScreen : public RmlUiScreen {
public:
    // `selection` points INTO the screen below this one on the stack, which outlives it. That
    // is how OK gets its answer back to the presentation screen without a callback: the stack
    // already guarantees the lifetime (app/screen.h).
    CarScreen(LevelEntry level, int* selection)
        : level_(std::move(level)), selection_(selection),
          index_(selection && *selection >= 0 ? *selection : 0) {}

    bool enter(App& app) override;
    void exit(App& app) override;
    void update(App& app, double dt_ms) override;
    void draw(App& app) override;
    const char* name() const override { return "car"; }

protected:
    const char* document_source() const override;
    const char* context_name() const override { return "car"; }
    void on_ready(App& app) override;
    void on_activate(App& app, const std::string& id) override;

private:
    void load_artwork(App& app);   // CARSEL.RAW - the background AND the screen's palette
    void refresh(App& app);
    void show_vehicle(App& app);
    void register_logos(App& app);
    void refresh_logo();           // the plate's wibble, and nothing else
    VehicleTint tint() const;      // the chosen preset, as the ramp's brightest step

    LevelEntry level_;
    TextTable text_;
    // CARSCN through the environment's CARSEL.RAW: its sprites, its title's colours.
    ScnScreen scn_;
    std::string doc_;
    int* selection_ = nullptr;
    int index_ = 0;

    // The screen's palette and the screen's picture, both out of `CARSEL.RAW` - one palette for
    // everything drawn on it, which is what a 256-colour screen means.
    std::vector<RGB> palette_;
    Background background_;
    VehicleDisplay display_;
    float turn_ = 0.0f;   // where the vehicle has got to, in turns

    // Index into `color_presets()`. Seeded from the profile on entry and written back on OK.
    int colour_ = 0;
    // Index into the game's own logo sheet, same lifetime rule.
    int logo_ = 0;
    TeamLogos logos_;
    double anim_ms_ = 0.0;   // the plate is five hand-drawn frames, like every other wibble
    int plate_frame_ = 0;
};

} // namespace bigred
