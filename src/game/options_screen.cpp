#include "options_screen.h"

#include <RmlUi/Core.h>

#include <cctype>
#include <cstdio>
#include <memory>

#include <SDL.h>

#include "app/app.h"
#include "app/player_profile.h"
#include "game/placeholder_screen.h"
#include "game/profile_screen.h"

namespace bigred {
namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// A value drawn over one of the original's widgets in the STEADY lettering: a resolution, a
// setting - read, not looked at.
constexpr const char* kValueCss =
    ".value{position:absolute;left:0;top:0;width:100%;height:100%;font-size:24dp;"
    "text-align:center;line-height:90dp;color:#ffffff;}";

} // namespace

// --- the hub: OPTSCN -------------------------------------------------------------------------
//
// OPTSCN.TXT's five boxes are Joysticks, Controls, Display, Sound, Damage. This engine has no
// joystick calibration and keeps the damage in the DRIVER screen (the user's call, 2026-09-23),
// so the first box is DRIVER in our own words; the other three keep their line of TOURN.<LANG>
// at its own place. The fifth - the original's one game setting, the damage - holds ours: the
// SPEED class (RaceRules::speed_percent), stepped to the next one each time it is chosen. The
// animated OK is the way back, as there.

namespace {
constexpr const char* kSpeedCss =
    ".speedval{position:absolute;left:0;width:560dp;top:0;height:100%;font-size:24dp;"
    "text-align:right;line-height:86dp;color:#ffffff;white-space:nowrap;}"
    ".hit:hover .speedval,.hit:focus .speedval{color:#ffff40;}";
} // namespace

bool OptionsScreen::enter(App& app) {
    scn_.load(app, "OPTSCN");
    text_.load(app.game_dir(), config_language(app.game_dir()));
    doc_ = scn_.document_open(kSpeedCss);
    doc_ += ScnScreen::box_open("driver", 10, 40, 210, 24) +
            scn_.words_rml("DRIVER", 80, 40, -1, 10, 40) + "</div>";
    doc_ += ScnScreen::box_open("controls", 10, 70, 210, 24) +
            scn_.text_rml(text_, "OPT_KEYS", 10, 70, "b") + "</div>";
    doc_ += ScnScreen::box_open("display", 10, 100, 210, 24) +
            scn_.text_rml(text_, "OPT_DISP", 10, 100) + "</div>";
    doc_ += ScnScreen::box_open("sound", 10, 130, 210, 24) +
            scn_.text_rml(text_, "OPT_SOUND", 10, 130, "b") + "</div>";
    doc_ += ScnScreen::box_open("speed", 10, 160, 210, 24) +
            scn_.words_rml("SPEED", 50, 160, -1, 10, 160) +
            "<div id=\"speed_value\" class=\"speedval\"></div></div>";
    doc_ += ScnScreen::box_open("back", 250, 155, 60, 45) +
            scn_.anisprite_rml(0, 5, 250, 155, 250, 155) + "</div>";
    doc_ += "</body></rml>";
    return RmlUiScreen::enter(app);
}

const char* OptionsScreen::document_source() const { return doc_.c_str(); }

void OptionsScreen::on_ready(App& app) {
    set_focus_order({"driver", "controls", "display", "sound", "speed", "back"});
    refresh(app);
}

void OptionsScreen::refresh(App& app) {
    if (!document_) return;
    if (Rml::Element* e = document_->GetElementById("speed_value"))
        e->SetInnerRML(kSpeedClasses[app.profile().speed_class % kSpeedClassCount].name);
}

void OptionsScreen::on_activate(App& app, const std::string& id) {
    if (id == "display") { app.push(std::make_unique<OptionsDisplayScreen>()); return; }
    if (id == "sound") { app.push(std::make_unique<OptionsSoundScreen>()); return; }
    if (id == "controls") { app.push(std::make_unique<OptionsControlsScreen>()); return; }
    if (id == "driver") { app.push(std::make_unique<ProfileScreen>(ProfileScreen::Mode::Edit)); return; }
    if (id == "speed") {
        PlayerProfile& p = app.profile();
        p.speed_class = (p.speed_class + 1) % kSpeedClassCount;
        app.save_profile_now();
        refresh(app);
        return;
    }
    if (id == "back") { app.pop(); return; }
}

// --- Controls: KEYSCN -------------------------------------------------------------------------
//
// KEYSCN.TXT: the title, then KEYS_UP / DOWN / LEFT / RIGHT / BOOST / SPOT down the left at
// y = 45..170, one `mbox` per row at x = 185 ("add mboxes for which key you want to redefine" -
// the authors' own comment), the animated OK at 250,155. The key each row holds is written in
// the steady lettering where the box was: a key name is read, not looked at.

namespace {
constexpr int kKeyRowY[kDriveKeyCount] = {45, 70, 95, 120, 145, 170};
constexpr const char* kKeyText[kDriveKeyCount] = {"KEYS_UP",    "KEYS_DOWN",  "KEYS_LEFT",
                                                  "KEYS_RIGHT", "KEYS_BOOST", "KEYS_SPOT"};
constexpr const char* kKeyCss =
    ".keyname{position:absolute;left:150dp;width:420dp;top:0;height:100%;font-size:24dp;"
    "text-align:right;line-height:86dp;color:#ffffff;white-space:nowrap;}"
    ".hit:hover .keyname,.hit:focus .keyname{color:#ffff40;}"
    ".keyname.wait{color:#ff8040;}";

std::string key_id(int a) { return "key" + std::to_string(a); }
} // namespace

bool OptionsControlsScreen::enter(App& app) {
    scn_.load(app, "KEYSCN");
    text_.load(app.game_dir(), config_language(app.game_dir()));
    doc_ = scn_.document_open(kKeyCss);
    doc_ += scn_.text_rml(text_, "KEYS_TITLE");
    for (int a = 0; a < kDriveKeyCount; ++a)
        doc_ += ScnScreen::box_open(key_id(a), 10, kKeyRowY[a], 200, 24) +
                scn_.text_rml(text_, kKeyText[a], 10, kKeyRowY[a], a % 2 ? "b" : "") +
                "<div class=\"keyname\" id=\"" + key_id(a) + "_name\"></div></div>";
    doc_ += ScnScreen::box_open("back", 250, 155, 65, 45) +
            scn_.anisprite_rml(0, 5, 250, 155, 250, 155) + "</div>";
    doc_ += "</body></rml>";
    return RmlUiScreen::enter(app);
}

const char* OptionsControlsScreen::document_source() const { return doc_.c_str(); }

void OptionsControlsScreen::exit(App& app) {
    app.save_profile_now();
    RmlUiScreen::exit(app);
}

void OptionsControlsScreen::on_ready(App& app) {
    std::vector<std::string> ids;
    for (int a = 0; a < kDriveKeyCount; ++a) ids.push_back(key_id(a));
    ids.push_back("back");
    set_focus_order(ids);
    refresh(app);
}

void OptionsControlsScreen::refresh(App& app) {
    if (!document_) return;
    for (int a = 0; a < kDriveKeyCount; ++a) {
        Rml::Element* e = document_->GetElementById(key_id(a) + "_name");
        if (!e) continue;
        const bool wait = waiting_ == a;
        e->SetClass("wait", wait);
        const SDL_Scancode sc = static_cast<SDL_Scancode>(app.profile().keys[static_cast<size_t>(a)]);
        std::string name = SDL_GetScancodeName(sc);
        // A modifier works from either side (the race reads both), so it is named without one.
        if (sc >= SDL_SCANCODE_LCTRL && sc <= SDL_SCANCODE_RGUI) {
            if (const size_t sp = name.find(' '); sp != std::string::npos) name = name.substr(sp + 1);
        }
        e->SetInnerRML(wait ? std::string("PRESS A KEY") : upper(name.empty() ? "NONE" : name));
    }
}

void OptionsControlsScreen::on_activate(App& app, const std::string& id) {
    if (id == "back") { app.pop(); return; }
    for (int a = 0; a < kDriveKeyCount; ++a)
        if (id == key_id(a)) {
            waiting_ = a;
            refresh(app);
            return;
        }
}

bool OptionsControlsScreen::handle_event(App& app, const SDL_Event& e) {
    if (waiting_ < 0) return RmlUiScreen::handle_event(app, e);
    // Waiting for a key: the keyboard belongs to the capture, not to the navigation - an arrow
    // pressed now is the key being chosen, not a move to the next row.
    if (e.type == SDL_KEYDOWN && !e.key.repeat) {
        const SDL_Scancode sc = e.key.keysym.scancode;
        std::array<int, kDriveKeyCount>& keys = app.profile().keys;
        const size_t w = static_cast<size_t>(waiting_);
        if (sc == SDL_SCANCODE_ESCAPE) {
            // gives up: the key stays what it was
        } else if (sc == SDL_SCANCODE_DELETE || sc == SDL_SCANCODE_BACKSPACE) {
            const int def = default_drive_keys()[w];
            for (size_t o = 0; o < keys.size(); ++o)
                if (o != w && keys[o] == def) keys[o] = keys[w];
            keys[w] = def;
        } else {
            // Two actions on one key would fight: the other row takes this one's old key.
            for (size_t o = 0; o < keys.size(); ++o)
                if (o != w && keys[o] == sc) keys[o] = keys[w];
            keys[w] = sc;
        }
        app.menu_click();
        waiting_ = -1;
        refresh(app);
        return true;
    }
    // A click or a pad button anywhere gives up too; the pad has no keys to offer.
    if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONDOWN) {
        waiting_ = -1;
        refresh(app);
        return true;
    }
    return RmlUiScreen::handle_event(app, e);
}

// --- Display: LANDSCN's picture and widgets ----------------------------------------------------
//
// LANDSCN is the original's landscape-distance screen: "-", a black scaler bar, "+" on each of
// its two rows, and the animated OK (LANDSCN.SPR: OK 0-4, + 5-9, - 10-14, bar 25-29). This
// engine's one display setting takes its first row: the resolution ladder of app/presentation.h,
// its value written over the bar.

bool OptionsDisplayScreen::enter(App& app) {
    scn_.load(app, "LANDSCN");
    doc_ = scn_.document_open(kValueCss);
    doc_ += scn_.words_rml("RESOLUTION", 10, 15);
    doc_ += ScnScreen::box_open("prev", 80, 40, 30, 25) +
            scn_.anisprite_rml(10, 5, 83, 40, 80, 40) + "</div>";
    doc_ += scn_.anisprite_rml(25, 5, 109, 35);
    doc_ += ScnScreen::box_open("mode_box", 110, 40, 100, 25, "readout") +
            "<div id=\"mode_value\" class=\"value\"></div></div>";
    doc_ += ScnScreen::box_open("next", 210, 40, 30, 25) +
            scn_.anisprite_rml(5, 5, 216, 40, 210, 40) + "</div>";
    doc_ += ScnScreen::box_open("back", 250, 150, 60, 50) +
            scn_.anisprite_rml(0, 5, 250, 155, 250, 150) + "</div>";
    doc_ += "</body></rml>";
    return RmlUiScreen::enter(app);
}

const char* OptionsDisplayScreen::document_source() const { return doc_.c_str(); }

void OptionsDisplayScreen::exit(App& app) {
    // Leaving is the save point: one setting, no separate SAVE button to remember to press.
    app.profile().display_mode = app.presentation().mode();
    app.save_profile_now();
    RmlUiScreen::exit(app);
}

void OptionsDisplayScreen::on_ready(App& app) {
    set_focus_order({"prev", "next", "back"}, "next");
    refresh(app);
}

void OptionsDisplayScreen::refresh(App& app) {
    if (!document_) return;
    // ⚠️ UPPERCASE: mode_name() returns "320x200"/"640x480"/"native" for logs, but the game's
    // own font has no lowercase (ui/rml_font_engine.h) - drawing it as-is produced garbage for
    // every letter outside the font's ASCII 32-90 range, the bug the user hit.
    if (Rml::Element* e = document_->GetElementById("mode_value"))
        e->SetInnerRML(upper(app.presentation().mode_name()));
}

void OptionsDisplayScreen::on_activate(App& app, const std::string& id) {
    if (id == "back") { app.pop(); return; }
    if (id == "prev") app.presentation().step_mode(-1);
    else if (id == "next") app.presentation().step_mode(1);
    else return;
    refresh(app);
}

// --- Sound: SOUNDSCN ------------------------------------------------------------------------
//
// SOUNDSCN.TXT puts no sprite on the screen: the original draws its widgets from BUTTONS.SPR in
// code, and the sheet says which - "+" 0-4, "-" 5-9, OK 10-14, the volume bar at level L in
// 15+5L..19+5L (L = 0..7, eight levels: exactly this screen's eight notches), the cross 55-59
// and the tick 60-64. Placed on SOUNDSCN's own boxes.

namespace {
// 8 notches, 0..7, mapped over this engine's 0..256 range - the same SHAPE as the original's
// SOUNDSCN (8 crans each for CD and FX), not its literal constant: that one targets a 0..127
// volume API this project's Mixer/MusicPlayer do not have.
constexpr int kNotches = 7;   // steps between notch 0 and notch 7 - 8 notches, 7 gaps
int notch_of(int volume256) { return (volume256 * kNotches + 128) / 256; }
int volume_of(int notch) { return (notch * 256) / kNotches; }
constexpr int kBarFirst = 15, kCross = 55, kTick = 60;
} // namespace

bool OptionsSoundScreen::enter(App& app) {
    scn_.load(app, "SOUNDSCN", "SOUND.RAW", "BUTTONS.SPR");   // ⚠️ SOUND.RAW, not SOUNDSCN.RAW
    text_.load(app.game_dir(), config_language(app.game_dir()));
    doc_ = scn_.document_open();
    doc_ += scn_.text_rml(text_, "SOUND_HED");
    doc_ += scn_.text_rml(text_, "SOUND_CD", 0, 0, "b");
    doc_ += scn_.text_rml(text_, "SOUND_FX");
    // Each bar in the middle of its row's segment boxes, 129..232 x 45..80 and 88..123.
    const int bar_w = scn_.sprite_w(kBarFirst), bar_h = scn_.sprite_h(kBarFirst);
    const int bar_x = 129 + (103 - bar_w) / 2;
    doc_ += ScnScreen::box_open("music_prev", 103, 50, 30, 30) +
            scn_.anisprite_rml(5, 5, 103, 50, 103, 50) + "</div>";
    doc_ += scn_.anisprite_rml(kBarFirst, 5, bar_x, 45 + (35 - bar_h) / 2, 0, 0, "music_bar");
    doc_ += ScnScreen::box_open("music_next", 236, 50, 30, 30) +
            scn_.anisprite_rml(0, 5, 236, 50, 236, 50) + "</div>";
    doc_ += ScnScreen::box_open("fx_prev", 103, 93, 30, 30) +
            scn_.anisprite_rml(5, 5, 103, 93, 103, 93) + "</div>";
    doc_ += scn_.anisprite_rml(kBarFirst, 5, bar_x, 88 + (35 - bar_h) / 2, 0, 0, "fx_bar");
    doc_ += ScnScreen::box_open("fx_next", 236, 93, 30, 30) +
            scn_.anisprite_rml(0, 5, 236, 93, 236, 93) + "</div>";
    doc_ += ScnScreen::box_open("commentary", 20, 140, 230, 30) +
            scn_.text_rml(text_, "SOUND_COMMENT", 20, 140, "b") +
            scn_.anisprite_rml(kCross, 5, 225, 142, 20, 140) + "</div>";
    doc_ += ScnScreen::box_open("back", 270, 155, 45, 40) +
            scn_.anisprite_rml(10, 5, 270, 155, 270, 155) + "</div>";
    doc_ += "</body></rml>";
    return RmlUiScreen::enter(app);
}

const char* OptionsSoundScreen::document_source() const { return doc_.c_str(); }

void OptionsSoundScreen::exit(App& app) {
    app.profile().music_volume = app.music().volume();
    app.profile().fx_volume = app.mixer().master();
    app.save_profile_now();
    RmlUiScreen::exit(app);
}

void OptionsSoundScreen::on_ready(App& app) {
    set_focus_order({"music_prev", "music_next", "fx_prev", "fx_next", "commentary", "back"},
                    "back");
    refresh(app);
}

void OptionsSoundScreen::refresh(App& app) {
    if (!document_) return;
    // Which frames each animated widget cycles through: the bar at its level, the box ticked or
    // crossed. RmlUiScreen::update turns the frames.
    auto set_first = [&](Rml::Element* e, int first) {
        if (e) e->SetAttribute("data-anim-first", first);
    };
    set_first(document_->GetElementById("music_bar"), kBarFirst + 5 * notch_of(app.music().volume()));
    set_first(document_->GetElementById("fx_bar"), kBarFirst + 5 * notch_of(app.mixer().master()));
    // The original's COMMENTARY: today it picks the menus' own sound - a silly
    // sample at random instead of BEEP2 (App::menu_click). The race's
    // commentator (GENRAL) will read it too.
    if (Rml::Element* box = document_->GetElementById("commentary")) {
        Rml::ElementList tick;
        box->GetElementsByClassName(tick, "anim");
        if (!tick.empty()) set_first(tick[0], app.profile().commentary ? kTick : kCross);
    }
}

void OptionsSoundScreen::on_activate(App& app, const std::string& id) {
    if (id == "back") { app.pop(); return; }
    if (id == "commentary") {
        app.profile().commentary = !app.profile().commentary;
        refresh(app);
        return;
    }
    if (id == "music_prev" || id == "music_next") {
        int n = notch_of(app.music().volume()) + (id == "music_next" ? 1 : -1);
        n = n < 0 ? 0 : (n > kNotches ? kNotches : n);
        app.music().set_volume(volume_of(n));
    } else if (id == "fx_prev" || id == "fx_next") {
        int n = notch_of(app.mixer().master()) + (id == "fx_next" ? 1 : -1);
        n = n < 0 ? 0 : (n > kNotches ? kNotches : n);
        app.mixer().set_master(volume_of(n));
    } else {
        return;
    }
    refresh(app);
}

} // namespace bigred
