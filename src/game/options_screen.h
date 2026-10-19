// "Options": the hub (OPTSCN's own five - Joysticks, Controls, Display, Sound, Damage, folded
// to what this engine actually has knobs for today) plus the two children that are REAL
// screens, not mock-ups - Display and Sound, because "le moteur a deja les reglages" (the
// menus' construction order, point 3).
//
//
// Controls and Race (damage) are still a PlaceholderScreen
// - key rebinding needs a capture flow this session does not build, and per-player damage needs
// the multiplayer simulation that does not exist yet. Driver pushes game/profile_screen.h in
// Edit mode, which IS real.
#pragma once

#include <string>

#include "app/rml_screen.h"
#include "formats/localised_text.h"
#include "ui/scn_kit.h"

namespace bigred {

class OptionsScreen : public RmlUiScreen {
public:
    bool enter(App& app) override;
    const char* name() const override { return "options"; }

protected:
    const char* document_source() const override;
    const char* context_name() const override { return "options"; }
    void on_ready(App& app) override;
    void on_activate(App& app, const std::string& id) override;

private:
    void refresh(App& app);

    ScnScreen scn_;
    TextTable text_;
    std::string doc_;
};

// Options -> Controls: KEYSCN. The six driving keys of the first player's keyboard, each on
// KEYSCN's own row; choosing one waits for the next key pressed. Escape gives up, Delete puts
// the default back, a key another row already had is swapped with it. Saved on the way out.
class OptionsControlsScreen : public RmlUiScreen {
public:
    bool enter(App& app) override;
    void exit(App& app) override;
    bool handle_event(App& app, const SDL_Event& e) override;
    const char* name() const override { return "options-controls"; }

protected:
    const char* document_source() const override;
    const char* context_name() const override { return "options-controls"; }
    void on_ready(App& app) override;
    void on_activate(App& app, const std::string& id) override;

private:
    void refresh(App& app);
    ScnScreen scn_;
    TextTable text_;
    std::string doc_;
    int waiting_ = -1;   // the row waiting for its key, or -1
};

// Options -> Display: the +/- resolution ladder (app/presentation.h), as buttons - the keys
// already work everywhere, this is just a screen a mouse/pad can reach the same setting from.
class OptionsDisplayScreen : public RmlUiScreen {
public:
    bool enter(App& app) override;
    void exit(App& app) override;
    const char* name() const override { return "options-display"; }

protected:
    const char* document_source() const override;
    const char* context_name() const override { return "options-display"; }
    void on_ready(App& app) override;
    void on_activate(App& app, const std::string& id) override;

private:
    void refresh(App& app);
    ScnScreen scn_;
    TextTable text_;
    std::string doc_;
};

// Options -> Sound: CD (music) and FX volume, 8 notches each - the same shape as the original's
// SOUNDSCN, adapted to this engine's 0..256 range rather than its 0..127.
class OptionsSoundScreen : public RmlUiScreen {
public:
    bool enter(App& app) override;
    void exit(App& app) override;
    const char* name() const override { return "options-sound"; }

protected:
    const char* document_source() const override;
    const char* context_name() const override { return "options-sound"; }
    void on_ready(App& app) override;
    void on_activate(App& app, const std::string& id) override;

private:
    void refresh(App& app);
    ScnScreen scn_;
    TextTable text_;
    std::string doc_;
};

} // namespace bigred
