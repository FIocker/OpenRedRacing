// The player's profile: pseudo entry (first run, "NAMESCN" in spirit) and "Options -> Pilote"
// (character colour, vehicle colour, logo filename, and the local logo-privacy toggle).
//
//
// ⚠️ THE TEXT FIELDS ARE HAND-ROLLED, NOT RML `<input>`s: this screen predates any use of
// RmlUi's form controls in this project, the game's own font only covers uppercase
// (ui/rml_font_engine.h - "no lowercase in the game's own alphabet"), and a plain text buffer
// with SDL_TEXTINPUT/BACKSPACE handled directly sidesteps having to learn how RmlUi's text
// widget behaves under a from-scratch font engine before anything depends on it. NAMESCN itself
// says "saisie clavier" (keyboard entry) - a gamepad was never expected
// to type a name, so this screen does not try to make typing pad-navigable.
#pragma once

#include <string>

#include "app/player_profile.h"
#include "app/rml_screen.h"
#include "formats/localised_text.h"
#include "ui/scn_kit.h"
#include "ui/team_logos.h"

namespace bigred {

class ProfileScreen : public RmlUiScreen {
public:
    enum class Mode { Create, Edit };
    explicit ProfileScreen(Mode mode) : mode_(mode) {}

    bool enter(App& app) override;
    void exit(App& app) override;
    void resumed(App& app) override;
    bool handle_event(App& app, const SDL_Event& e) override;
    void update(App& app, double dt_ms) override;
    const char* name() const override {
        return mode_ == Mode::Create ? "profile-create" : "profile-edit";
    }

protected:
    const char* document_source() const override { return doc_source_.c_str(); }
    const char* context_name() const override {
        return mode_ == Mode::Create ? "profile-create" : "profile-edit";
    }
    void on_ready(App& app) override;
    void on_activate(App& app, const std::string& id) override;
    // Create mode cannot be skipped - it is pushed as the FIRST screen when no profile exists,
    // so there is nothing underneath to pop back to (rule 1, "sur le premier, il quitte"). Edit
    // mode discards unsaved changes and pops, same as any cancel.
    void on_back(App& app) override;

private:
    static constexpr size_t kMaxPseudo = 16;
    static constexpr size_t kMaxLogo = 48;

    void register_logos(App& app);   // the game's own thirteen, so the picker can show them
    void build_document();     // fills doc_source_ for the current mode
    void refresh();            // pushes profile_/pseudo_/logo_buffer_ into the live document
    void commit(App& app);     // saves and leaves the screen

    Mode mode_;
    PlayerProfile profile_;    // Edit mode: loaded in enter(). Create mode: defaults.
    std::string pseudo_;       // the buffer SDL_TEXTINPUT/BACKSPACE edit when this is active
    std::string logo_buffer_;  // Edit mode's second text field - a filename, not validated yet
    bool editing_logo_ = false;   // which of the two buffers above is being typed into
    std::string doc_source_;
    // NAMESCN / CHOOSCN's picture, CONFIG4B.RAW, and CHOOSCN's animated OK.
    ScnScreen scn_;
    TextTable text_;

    // The blinking caret - a drawn bar, not a font glyph (see profile_screen.cpp for why "_"
    // itself does not work). Ticked in update(), not tied to any fixed simulation step: it is
    // pure presentation, like the HUD's own animations.
    double caret_timer_ms_ = 0.0;
    bool caret_visible_ = true;

    // The game's own logo sheet, for the TEAM LOGO picker. ⚠️ The CUSTOM logo (a file in the
    // player's own folder) is a separate field and a separate feature - see app/player_profile.h.
    TeamLogos logos_;
};

} // namespace bigred
