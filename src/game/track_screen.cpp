#include "track_screen.h"

#include <RmlUi/Core.h>

#include <cstdio>
#include <memory>
#include <regex>
#include <unordered_map>

#include "app/app.h"
#include "game/competition.h"
#include "game/split_screen.h"
#include "formats/bigred_archive.h"
#include "formats/sprite.h"
#include "formats/texture.h"
#include "formats/threedw.h"
#include "game/car_screen.h"
#include "game/race_screen.h"
#include "ui/team_logos.h"

namespace bigred {
namespace {

// THE PICTURE CARRIES ITS OWN TITLE: every `VIEW.RAW` has the circuit's name painted in its top
// half ("The Coffee Ground", "Spanish Inquisition"), each in a different place. Our words - the
// country, the one-line description, the vehicle - are read, not looked at: steady lettering
// on a dark band BELOW the painted title and above the plates (2026-09-26). Everything else is
// TRAKSCN.TXT's own: three plates of TRAKSCN.SPR wibbling under VIE_VHC / VIE_PRV / VIE_RAC.
constexpr const char* kStyle = R"(
#info {
    position: absolute; left: 0; top: 50%; width: 92%; padding: 8dp 4%;
    background: #000000a0;
}
#title { font-size: 32dp; color: #ffdf40; margin-bottom: 8dp; white-space: nowrap; }
#blurb { font-size: 16dp; color: #ffffff; margin-bottom: 8dp; }
#car_line { font-size: 16dp; color: #a0c0ff; white-space: nowrap; }
)";

std::string upper_of(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// ⚠️ THE PLATES' LABELS ARE WHITE, not TRAKSCN.TXT's `@C8`. Ramp 8 of the circuit's palette is
// often the colour of the picture behind it, which left the words unreadable (user, 2026-09-28).
std::string white_text(const std::string& rml) {
    static const std::regex kColour("color:#[0-9a-fA-F]+;");
    return std::regex_replace(rml, kColour, "color:#ffffff;");
}

} // namespace

const char* TrackScreen::document_source() const { return doc_.c_str(); }

bool TrackScreen::enter(App& app) {
    text_.load(app.game_dir(), config_language(app.game_dir()));
    // ⚠️ THE PLATES ARE PALETTISED BY THE LEVEL: TRAKSCN ships no picture of its own, the
    // circuit's VIEW.RAW is laid under it - see the header.
    scn_.load(app, "TRAKSCN", "VIEW.RAW", "TRAKSCN.SPR", level_.name + ".LEV");
    doc_ = scn_.document_open(kStyle) +
           "<div id=\"info\"><div id=\"title\"></div><div id=\"blurb\" class=\"wrap-ok\"></div>"
           "<div id=\"car_line\"></div></div>";
    // TRAKSCN.TXT: `mbox 1, 0,145,78,55` / `mbox 2, 121,155,78,55` / `mbox 3, 242,145,78,55`,
    // an anisprite of glyphs 0-4 in each (the middle one lower, at 121,165), and each label at
    // its own `@X` with its `@C8` - ramp 8 of THIS circuit's palette.
    static const struct { const char* id; const char* key; int bx, by, sx, sy; } kPlates[] = {
        {"car", "VIE_VHC", 0, 145, 0, 145},
        {"view", "VIE_PRV", 121, 155, 121, 165},
        {"race", "VIE_RAC", 242, 145, 242, 145},
    };
    // ⚠️ NO CAR PLATE ON A SPLIT SCREEN (user, 2026-09-30): the vehicles are chosen in four on
    // the screen RACE opens, and a CAR here read as "choose now" - it only confused.
    split_ = app.locals().split();
    int n = 0;
    for (const auto& pl : kPlates) {
        if (split_ && std::string(pl.id) == "car") { ++n; continue; }
        doc_ += ScnScreen::box_open(pl.id, pl.bx, pl.by, 78, 55) +
                scn_.anisprite_rml(0, 5, pl.sx, pl.sy, pl.bx, pl.by) +
                white_text(scn_.text_rml(text_, pl.key, pl.bx, pl.by, (n++ % 2) ? "b" : "")) +
                "</div>";
    }
    doc_ += "</body></rml>";
    if (!RmlUiScreen::enter(app)) return false;
    start_intro(app);
    return true;
}

// ⚠️ THE INTRO BELONGS TO THIS SCREEN, not to the race (user, 2026-09-23). It is played once,
// and it is NEVER drawn for: on the original disc the intros are short jingles among 38 tracks,
// and a music folder that holds only the eight pieces has no file for them. Standing in a whole
// song for a jingle would push the circuit's own music minutes into the race.
void TrackScreen::start_intro(App& app) {
    if (intro_started_ || !app.music().ready()) return;
    intro_started_ = true;
    if (level_.intro_track > 0 && app.music().has_cd_track(level_.intro_track))
        app.music().play_cd_track(level_.intro_track, level_.music_track);
}

// Coming back from the car screen or from the fly-by. The intro is long over by then and the
// race's own piece may have replaced it, so the circuit's music is put back rather than the
// front end's - this screen is already inside a circuit.
void TrackScreen::resumed(App& app) {
    // Back from a championship race: the championship takes it from here. The same when the
    // player quit the competition from the results: whoever started it ends it.
    if (app.competition().race_pending() || app.competition().leaving()) {
        app.pop();
        return;
    }
    // Back from a race of a split screen's series: on to the next circuit, if any. Every seat
    // chooses its vehicle again - the next circuit has its own list.
    if (raced_) {
        raced_ = false;
        LocalPlayers& locals = app.locals();
        Competition::LocalSeries& ser = app.competition().series();
        if (locals.split() && ser.next + 1 < ser.circuits.size()) {
            ++ser.next;
            const std::string& next = ser.circuits[ser.next];
            for (const LevelEntry& l : scan_levels(app.game_dir()))
                if (l.name == next) {
                    for (LocalSeat& s : locals.seats) s.vehicle = -1;
                    app.pop();
                    app.push(std::make_unique<TrackScreen>(l));
                    return;
                }
        }
    }
    // Back from the split vehicle screen, everyone having chosen: off they go.
    if (split_go_) {
        split_go_ = false;
        start_race(app);
        return;
    }
    // ⚠️ THE INTRO RUNS ON until the player leaves this screen or starts the race (user,
    // 2026-09-28): back from the vehicle screen it used to be cut here for the circuit's music.
    // Only when neither the intro nor that music is playing is the music asked for.
    if (app.music().ready() && level_.music_track > 0 &&
        !(level_.intro_track > 0 && app.music().playing_cd_track(level_.intro_track)) &&
        !app.music().playing_cd_track(level_.music_track))
        app.music().play_cd_track(level_.music_track);
    if (document_) {
        if (Rml::Element* el = document_->GetElementById("car_line"))
            el->SetInnerRML(split_ ? "" : car_label().c_str());
    }
}

// The chosen vehicle as the player reads it: its commercial name out of `INF_<NAME>`, never the
// file name - the same rule as game/car_screen.cpp. The file name is only the fallback.
std::string TrackScreen::car_label() const {
    const std::string& car = level_.cars[static_cast<size_t>(vehicle_)];
    const std::vector<std::string> info = text_.text_lines("INF_" + car);
    return upper_of(info.empty() ? car : info[0]);
}

void TrackScreen::on_ready(App&) {
    if (!document_) return;
    const auto set = [this](const char* id, const Rml::String& html) {
        if (Rml::Element* el = document_->GetElementById(id)) el->SetInnerRML(html);
    };
    set("title", level_.title.c_str());
    set("blurb", level_.description.c_str());
    set("car_line", split_ ? "" : car_label().c_str());
    if (split_) set_focus_order({"view", "race"}, "race");
    else set_focus_order({"car", "view", "race"}, "race");
}

void TrackScreen::on_activate(App& app, const std::string& id) {
    // ⚠️ A SPLIT SCREEN CHOOSES ITS VEHICLES IN FOUR (user, 2026-09-26): CAR opens the
    // quarters instead of the one-player screen, and RACE goes through them first - the race
    // starts from resumed() once everyone has chosen.
    if (app.locals().split() && (id == "car" || id == "race")) {
        app.push(std::make_unique<SplitCarScreen>(level_, id == "race" ? &split_go_ : nullptr));
        return;
    }
    if (id == "car") {
        app.push(std::make_unique<CarScreen>(level_, &vehicle_));
        return;
    }
    if (id == "view") {
        RaceScreen::Setup setup;
        setup.level = level_.name;
        setup.level_title = level_.title;
        setup.fly_by = true;
        app.push(std::make_unique<RaceScreen>(std::move(setup)));
        return;
    }
    if (id == "race") start_race(app);
}

void TrackScreen::start_race(App& app) {
    {
        RaceScreen::Setup setup;
        setup.level = level_.name;
        setup.level_title = level_.title;
        setup.vehicle = level_.cars[static_cast<size_t>(vehicle_)];
        setup.damage_option = app.profile().damage_option;
        setup.rules.speed_percent = speed_percent_of(app.profile());
        // The colour the player picked on the vehicle screen, so the car they chose is the car
        // that drives off. Same table, same brightest-step convention.
        if (const std::vector<ColorPreset>& presets = color_presets(); !presets.empty()) {
            const ColorPreset& p =
                presets[static_cast<size_t>(app.profile().vehicle_color) % presets.size()];
            setup.tinted = true;
            setup.tint = VehicleTint{p.r, p.g, p.b};
        }
        setup.character = app.profile().character;
        setup.character_colour = app.profile().character_color;
        // And the emblem, read here rather than carried across screens: the vehicle screen may
        // never have been opened, and the profile is the thing that remembers.
        TeamLogos logos;
        if (logos.load(app.game_dir())) {
            int lw = 0, lh = 0;
            std::vector<uint8_t> emblem = logos.logo_rgba(app.profile().logo_index, lw, lh);
            if (lw == kLogoCell && lh == kLogoCell) setup.logo = std::move(emblem);
        }
        // A split screen: every player as their seat says - player one included, whose seat
        // replaces the profile - on the vehicle they chose.
        fill_split_setup(app, level_, vehicle_, setup);
        // The intro is playing right now and RACE cuts it - see race_screen.h's `skip_intro`.
        setup.skip_intro = true;
        raced_ = true;
        app.push(std::make_unique<RaceScreen>(std::move(setup)));
        return;
    }
}

} // namespace bigred
