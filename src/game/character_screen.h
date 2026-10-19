// The choice of driver - `CHARSCN`, "SELECT CHARACTER": the driver turning on the left, their
// fact sheet on the right, PERSON and COLOUR cycling underneath.
//
//
// ⚠️ IT EDITS A PROFILE IT DOES NOT OWN: `target` points into the screen underneath (the driver
// profile, whose SAVE is the one that writes to disk), the same arrangement as the vehicle
// screen's `selection`. OK writes the two fields back; Escape leaves them as they were.
#pragma once

#include <string>
#include <vector>

#include "app/player_profile.h"
#include "app/rml_screen.h"
#include "formats/localised_text.h"
#include "formats/threedw.h"   // RGB
#include "ui/background.h"
#include "ui/scn_kit.h"
#include "ui/vehicle_display.h"

namespace bigred {

class CharacterScreen : public RmlUiScreen {
public:
    explicit CharacterScreen(PlayerProfile* target) : target_(target) {}

    bool enter(App& app) override;
    void exit(App& app) override;
    void update(App& app, double dt_ms) override;
    void draw(App& app) override;
    const char* name() const override { return "character"; }

protected:
    const char* document_source() const override;
    const char* context_name() const override { return "character"; }
    void on_ready(App& app) override;
    void on_activate(App& app, const std::string& id) override;

private:
    void refresh(App& app);

    PlayerProfile* target_ = nullptr;
    int character_ = 0;
    int colour_ = 0;
    TextTable text_;
    // CHARSCN through DRIVER.RAW: its OK, its wibbly title and buttons.
    ScnScreen scn_;
    std::string doc_;
    // DRIVER.RAW: the screen's picture and, as on every 256-colour screen, its palette - the
    // driver is drawn through it.
    std::vector<RGB> palette_;
    Background background_;
    VehicleDisplay display_;
    float turn_ = 0.0f;
};

} // namespace bigred
