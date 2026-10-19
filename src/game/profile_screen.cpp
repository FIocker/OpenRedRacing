#include "profile_screen.h"

#include <RmlUi/Core.h>

#include <SDL.h>

#include <cctype>
#include <cstdio>
#include <memory>

#include "app/app.h"
#include "game/character_screen.h"
#include "game/main_menu_screen.h"
#include "scene/character_model.h"

namespace bigred {
namespace {

// Same convention as quick_race_screen.cpp: uppercase, sized in dp (app/rml_screen.h), rounded to the
// font's own 8px multiples.
constexpr const char* kStyle = R"(
/* Under the wibbly title, which takes the top of the screen as in the original. */
#panel { position: absolute; left: 8%; top: 21%; width: 84%; }
#title { font-size: 32dp; color: #ffdf40; margin-bottom: 24dp; white-space: nowrap; }
#sub { font-size: 16dp; color: #c0c0c0; margin-bottom: 24dp; }
/* ONE ROW PER SETTING, label on the left, its control on the right, a fixed height each: the
   screen used to stack every label ABOVE its control, and seven of them no longer fitted - the
   SAVE button ended up under the bottom of the screen (2026-09-26). */
.row { display: block; height: 48dp; white-space: nowrap; }
.row .label {
    display: inline-block; width: 44%; font-size: 16dp; color: #a0a0c0; vertical-align: middle;
}
.field {
    display: inline-block; font-size: 24dp; color: #ffffff; background: #00000080;
    border: 2dp #606060; padding: 4dp 12dp; width: 50%; vertical-align: middle;
}
/* NAMESCN: the name is typed in the wibbly lettering, where NAME_POINTER puts it (45,80). */
#create_field { position: absolute; left: 135dp; top: 288dp; font-family: wibs; font-size: 87dp; white-space: nowrap; }
#create_field .caret { width: 36dp; height: 10dp; }
.field.active { border: 2dp #ffdf40; }
/* The caret is DRAWN, not the font's "_" glyph - see the comment by kCaretVisible below for
   why. A thin bar at the baseline reads as an underscore; toggling .hidden is the blink. */
.caret { display: inline-block; width: 14dp; height: 4dp; background: #ffffff; vertical-align: bottom; margin-left: 2dp; }
.caret.hidden { visibility: hidden; }
.picker { display: inline-block; vertical-align: middle; }
.picker .button { display: inline-block; vertical-align: middle; }
.swatch {
    display: inline-block; width: 24dp; height: 24dp; vertical-align: middle;
    margin: 0 12dp; border: 2dp #606060;
}
.swatch img { width: 100%; height: 100%; }
#logo_image { width: 32dp; height: 32dp; border: 0; }
/* Wide enough for the longest value - "10 / 13" wrapped onto two lines in 120px. */
.colorname {
    display: inline-block; font-size: 16dp; width: 160dp; text-align: center;
    vertical-align: middle;
}
#buttons { margin-top: 16dp; }
.button {
    display: inline-block; font-size: 24dp; color: #ffffff; background: #00000080;
    padding: 6dp 16dp; margin-right: 24dp;
}
.picker .button { margin-right: 0; }
.button:hover, .button:focus { color: #ffdf40; }
/* A real checkbox, to the right of the sentence that names it - not "LABEL: ON" wrapping onto
   its own line. The row itself is the clickable/focusable target, not just the little box, so
   the hit area matches what the player reads. */
.toggle-row { cursor: pointer; }
.toggle-row:hover .label, .toggle-row:focus .label { color: #ffdf40; }
.checkbox {
    display: inline-block; width: 18dp; height: 18dp;
    background: #00000080; border: 2dp #ffffff; vertical-align: middle;
}
.checkbox.on { background: #ffdf40; border: 2dp #ffdf40; }
)";

std::string hex(uint8_t r, uint8_t g, uint8_t b) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", r, g, b);
    return buf;
}

std::string trim(std::string s) {
    size_t start = 0;
    while (start < s.size() && s[start] == ' ') ++start;
    size_t end = s.size();
    while (end > start && s[end - 1] == ' ') --end;
    return s.substr(start, end - start);
}

} // namespace

bool ProfileScreen::enter(App& app) {
    if (mode_ == Mode::Edit) {
        // From the App's live copy, not a fresh disk read: Edit mode is only reachable through
        // Options -> Pilote, after a profile already exists and possibly after another screen
        // (Options -> Display/Sound) changed fields of it in memory without saving yet.
        profile_ = app.profile();
        pseudo_ = profile_.pseudo;
        logo_buffer_ = profile_.logo;
    } else {
        profile_ = PlayerProfile();
        pseudo_.clear();
        logo_buffer_.clear();
    }
    editing_logo_ = false;
    text_.load(app.game_dir(), config_language(app.game_dir()));
    scn_.load(app, "CHOOSCN", "CONFIG4B.RAW");
    build_document();
    SDL_StartTextInput();
    return RmlUiScreen::enter(app);
}

void ProfileScreen::exit(App& app) {
    SDL_StopTextInput();
    RmlUiScreen::exit(app);
}

void ProfileScreen::build_document() {
    // Both modes are on NAMESCN / CHOOSCN's picture, CONFIG4B.RAW, with CHOOSCN's animated OK
    // at 250,155 as the way forward - Escape is the way back, as in the original.
    std::string body = scn_.document_open(kStyle);
    if (mode_ == Mode::Create) {
        // NAMESCN as the original lays it out: NAME ("ENTER YOUR NAME:") at its `@X30,40`,
        // and what is typed at NAME_POINTER's 45,80, in the same lettering.
        body += scn_.text_rml(text_, "NAME") +
                "<div id=\"create_field\"><span id=\"pseudo_text\"></span>"
                "<span id=\"pseudo_caret\" class=\"caret\"></span></div>" +
                ScnScreen::box_open("create", 250, 155, 60, 45) +
                scn_.anisprite_rml(0, 5, 250, 155, 250, 155) + "</div>";
    } else {
        // A picker: "<", what is chosen, ">". `middle` is the swatch or picture, if any.
        const auto picker = [](const std::string& label, const std::string& key,
                               const std::string& middle) {
            return "<div class=\"row\"><span class=\"label\">" + label +
                   "</span><div class=\"picker\">"
                   "<div class=\"button\" id=\"" + key + "_prev\">&lt;</div>" + middle +
                   "<div id=\"" + key + "_name\" class=\"colorname\"></div>"
                   "<div class=\"button\" id=\"" + key + "_next\">&gt;</div></div></div>\n";
        };
        // The title is ours - the original has no single screen for all of this - placed where
        // CHAR_HED is. The form itself is read and set, so it stays in the steady lettering.
        body += scn_.words_rml("DRIVER PROFILE", 30, 10) +
                "<div id=\"panel\">"
                "<div class=\"row\"><span class=\"label\">NAME</span>"
                "<div id=\"pseudo_field\" class=\"field\">"
                "<span id=\"pseudo_text\"></span><span id=\"pseudo_caret\" class=\"caret\"></span>"
                "</div></div>\n"
                // The driver has a screen of their own (game/character_screen.h): the fact
                // sheet and the turning model do not fit on a row.
                "<div class=\"row\"><span class=\"label\">CHARACTER</span>"
                "<div class=\"button\" id=\"character\"><span id=\"char_swatch\" "
                "class=\"swatch\"></span><span id=\"char_name\"></span></div></div>\n" +
                picker("VEHICLE COLOUR", "vehicle", "<div id=\"vehicle_swatch\" class=\"swatch\"></div>") +
                picker("TEAM LOGO", "logo", "<div id=\"logo_image\" class=\"swatch\"></div>") +
                "<div class=\"row\"><span class=\"label\">CUSTOM LOGO FILE</span>"
                "<div id=\"logo_field\" class=\"field\">"
                "<span id=\"logo_text\"></span><span id=\"logo_caret\" class=\"caret\"></span>"
                "</div></div>\n"
                "<div class=\"row toggle-row\" id=\"hide_logos\">"
                "<span class=\"label\">HIDE OTHER LOGOS</span>"
                "<span id=\"hide_logos_box\" class=\"checkbox\"></span>"
                "</div>\n" +
                picker("DAMAGE TAKEN", "damage", "") + "</div>" +
                ScnScreen::box_open("save", 250, 155, 60, 45) +
                scn_.anisprite_rml(0, 5, 250, 155, 250, 155) + "</div>";
    }
    body += "\n</body></rml>";
    doc_source_ = std::move(body);
}

// The thirteen emblems the game ships, under the shared names ui/team_logos.h defines - so the
// vehicle screen and this one show the same picture instead of each uploading its own copy.
void ProfileScreen::register_logos(App& app) {
    if (!logos_.load(app.game_dir())) return;
    for (int i = 0; i < logos_.count(); ++i) {
        int w = 0, h = 0;
        std::vector<uint8_t> rgba = logos_.logo_rgba(i, w, h);
        if (!rgba.empty())
            app.ui().renderer().register_image(team_logo_image_name(i), std::move(rgba), w, h);
    }
}

void ProfileScreen::on_ready(App& app) {
    register_logos(app);
    if (mode_ == Mode::Create) {
        set_focus_order({"create"});   // typing needs no focus: every key goes to the name
    } else {
        set_focus_order({"pseudo_field", "character", "vehicle_prev", "vehicle_next",
                          "logo_prev", "logo_next", "logo_field", "hide_logos", "damage_prev",
                          "damage_next", "save"}, "save");
    }
    refresh();
}

void ProfileScreen::refresh() {
    if (!document_) return;
    const bool pseudo_active = !editing_logo_;   // Create mode: editing_logo_ never flips, so
                                                  // this is always true there - one field, always on
    if (Rml::Element* t = document_->GetElementById("pseudo_text")) t->SetInnerRML(pseudo_);
    if (Rml::Element* c = document_->GetElementById("pseudo_caret"))
        c->SetClass("hidden", !pseudo_active || !caret_visible_);
    if (Rml::Element* f = document_->GetElementById("pseudo_field")) f->SetClass("active", pseudo_active);

    if (mode_ != Mode::Edit) return;

    const auto& presets = color_presets();
    const ColorPreset& cc = presets[static_cast<size_t>(profile_.character_color)];
    const ColorPreset& vc = presets[static_cast<size_t>(profile_.vehicle_color)];
    if (Rml::Element* e = document_->GetElementById("char_swatch"))
        e->SetProperty("background-color", hex(cc.r, cc.g, cc.b));
    if (Rml::Element* e = document_->GetElementById("char_name"))
        e->SetInnerRML(std::string(character_info(profile_.character).first_name) + " - CHANGE");
    if (Rml::Element* e = document_->GetElementById("vehicle_swatch"))
        e->SetProperty("background-color", hex(vc.r, vc.g, vc.b));
    if (Rml::Element* e = document_->GetElementById("vehicle_name")) e->SetInnerRML(vc.name);
    if (Rml::Element* t = document_->GetElementById("logo_text"))
        t->SetInnerRML(logo_buffer_.empty() ? std::string("(DEFAULT)") : logo_buffer_);
    if (Rml::Element* c = document_->GetElementById("logo_caret"))
        c->SetClass("hidden", !editing_logo_ || !caret_visible_);
    if (Rml::Element* f = document_->GetElementById("logo_field")) f->SetClass("active", editing_logo_);

    // The team emblem: the picture itself is the value, with its rank beside it.
    if (const int n = logos_.count(); n > 0) {
        const int idx = ((profile_.logo_index % n) + n) % n;
        if (Rml::Element* e = document_->GetElementById("logo_image"))
            e->SetInnerRML(("<img src=\"" + team_logo_image_name(idx) + "\"/>").c_str());
        if (Rml::Element* e = document_->GetElementById("logo_name"))
            e->SetInnerRML((std::to_string(idx + 1) + " / " + std::to_string(n)).c_str());
    }
    if (Rml::Element* e = document_->GetElementById("hide_logos_box"))
        e->SetClass("on", profile_.hide_other_logos);
    if (Rml::Element* e = document_->GetElementById("damage_name")) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%d %%", profile_.damage_option);
        e->SetInnerRML(buf);
    }
}

void ProfileScreen::update(App& app, double dt_ms) {
    RmlUiScreen::update(app, dt_ms);
    // A plain timer, not a tick count: this is presentation on a screen with no simulation to
    // stay in lockstep with (the simulation/presentation frontier). 500ms is the usual
    // OS caret rate.
    caret_timer_ms_ += dt_ms;
    if (caret_timer_ms_ >= 500.0) {
        caret_timer_ms_ = 0.0;
        caret_visible_ = !caret_visible_;
        refresh();
    }
}

void ProfileScreen::on_activate(App& app, const std::string& id) {
    const auto& presets = color_presets();
    const int n = static_cast<int>(presets.size());
    if (id == "create" || id == "save") { commit(app); return; }
    if (id == "back") { app.pop(); return; }
    if (id == "logo_prev" || id == "logo_next") {
        // The game's own thirteen, cyclic. ⚠️ NOT the custom-logo field below it: that one names
        // a file in the player's folder and is a different feature (app/player_profile.h).
        if (const int count = logos_.count(); count > 0) {
            const int delta = (id == "logo_next") ? 1 : count - 1;
            profile_.logo_index = ((profile_.logo_index + delta) % count + count) % count;
        }
        refresh();
        return;
    }
    if (id == "character" || id == "char_swatch" || id == "char_name") {
        app.push(std::make_unique<CharacterScreen>(&profile_));
        return;
    }
    if (id == "vehicle_prev") { profile_.vehicle_color = (profile_.vehicle_color + n - 1) % n; refresh(); return; }
    if (id == "vehicle_next") { profile_.vehicle_color = (profile_.vehicle_color + 1) % n; refresh(); return; }
    // ⚠️ BOTH IDS: the row's own ("hide_logos", reached via a click on the label) AND the
    // checkbox span's own ("hide_logos_box", reached via a click ON the box itself) - the box
    // needs an id for refresh() to find it, which means the "walk up to the nearest id"
    // click-bubbling in RmlUiScreen::handle_event() stops AT the box rather than passing it.
    // Without this the checkbox looked clickable but only the label text actually was - the
    // bug the user hit.
    if (id == "hide_logos" || id == "hide_logos_box") {
        profile_.hide_other_logos = !profile_.hide_other_logos;
        refresh();
        return;
    }
    if (id == "damage_prev") {
        profile_.damage_option = profile_.damage_option < 5 ? 0 : profile_.damage_option - 5;
        refresh();
        return;
    }
    if (id == "damage_next") {
        profile_.damage_option = profile_.damage_option > 95 ? 100 : profile_.damage_option + 5;
        refresh();
        return;
    }
    if (id == "pseudo_field" || id == "logo_field") {
        editing_logo_ = (id == "logo_field");
        caret_visible_ = true;
        caret_timer_ms_ = 0.0;
        refresh();
        return;
    }
}

// Back from the driver screen, which wrote into `profile_` on OK.
void ProfileScreen::resumed(App&) { refresh(); }

void ProfileScreen::on_back(App& app) {
    if (mode_ == Mode::Create) { app.quit(); return; }   // rule 1: Escape on the FIRST screen quits
    app.pop();                                            // Edit: discard whatever was being edited
}

void ProfileScreen::commit(App& app) {
    const std::string trimmed = trim(pseudo_);
    if (trimmed.empty()) return;   // nothing to do until the player types something

    profile_.pseudo = trimmed;
    if (mode_ == Mode::Edit) profile_.logo = trim(logo_buffer_);
    app.profile() = profile_;   // the App's copy is what every other screen reads from now on
    app.save_profile_now();

    // ⚠️ POP THEN PUSH, NOT A PLAIN POP: in Create mode this screen IS the root (there is no
    // profile yet, so nothing was underneath it to reveal - see openbigred.cpp). Both are
    // applied in the SAME apply_pending() pass (app/app.cpp), so the stack is never actually
    // empty between them and the game does not exit.
    app.pop();
    if (mode_ == Mode::Create) app.push(std::make_unique<MainMenuScreen>());
}

bool ProfileScreen::handle_event(App& app, const SDL_Event& e) {
    std::string& active = (mode_ == Mode::Edit && editing_logo_) ? logo_buffer_ : pseudo_;
    const size_t cap = (mode_ == Mode::Edit && editing_logo_) ? kMaxLogo : kMaxPseudo;

    if (e.type == SDL_TEXTINPUT) {
        // ⚠️ ASCII ONLY: SDL delivers this as UTF-8, but the game's own font has no lowercase
        // and its accented capitals are single-byte CP437/850, not UTF-8 (ui/rml_font_engine.h,
        // formats/localised_text.h). Appending "e" typed as e9 (2 UTF-8 bytes) both mangled
        // the uppercasing (toupper on a continuation byte) AND backspace, which pops one BYTE:
        // one press left a dangling, unrenderable half-character stuck in the field - the bug
        // the user hit. Bytes outside printable ASCII are dropped instead of accepted broken.
        for (const char* p = e.text.text; *p; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c < 0x20 || c > 0x7e) continue;
            if (active.size() < cap) active += static_cast<char>(std::toupper(c));
        }
        caret_visible_ = true;
        caret_timer_ms_ = 0.0;   // solid while typing, like a real text field's caret
        refresh();
        return true;
    }
    if (e.type == SDL_KEYDOWN && !e.key.repeat) {
        if (e.key.keysym.sym == SDLK_BACKSPACE) {
            if (!active.empty()) active.pop_back();
            caret_visible_ = true;
            caret_timer_ms_ = 0.0;
            refresh();
            return true;
        }
        if ((e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) &&
            mode_ == Mode::Create) {
            commit(app);
            return true;
        }
    }
    return RmlUiScreen::handle_event(app, e);
}

} // namespace bigred
