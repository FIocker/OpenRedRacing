#include "car_screen.h"

#include <RmlUi/Core.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <unordered_map>

#include "app/app.h"
#include "formats/game_files.h"
#include "formats/level_script.h"   // env_name_for_level
#include "formats/texture.h"        // kRawPaletteBytes
#include "scene/character_model.h"

namespace bigred {
namespace {

std::string upper_of(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Where the turning vehicle sits, as fractions of the canvas.
//
// ⚠️ IT IS ON THE LEFT BECAUSE THE ORIGINAL'S OWN COORDINATES PUT IT THERE. `TOURN.ENG` places
// the six statistics at `@X200,90` to `@X200,140` and the vehicle's name at `@X120,40` on a
// 320x200 screen, so the model had the whole left half below the name - x < 200, y > 55. The
// layout below is those numbers turned into percentages, which is also why the buttons sit
// where they do (`@X20,165` and `@X260,165`).
//
// ⚠️ NOTHING IN THE DOCUMENT MAY OVERLAP THIS BOX: the document is painted afterwards, so a
// widget laid over it would sit on the car rather than behind it.
constexpr float kViewX = 0.02f, kViewY = 0.33f, kViewW = 0.48f, kViewH = 0.46f;

// One revolution in ten seconds: slow enough to look at a wheel, fast enough that the far side
// comes round while the player is still deciding.
constexpr float kTurnsPerSecond = 0.10f;

// How many statistics a vehicle's entry can carry. Six in every shipped vehicle, and the sixth
// is always the nitro.
constexpr int kStatRows = 6;

// THE ORIGINAL'S PIECES: CAR_HED in the wibbly lettering at its `@X16,10`, the NEXT plate
// (CARSCN.SPR 5-9 at 14,162, CAR_NXT on it) and the OK (0-4 at 250,155) - CARSCN.TXT. The
// rest is read, not looked at, so it is steady lettering: the vehicle's name and kind, its
// six statistics (the original draws its own sheet that way too), the colour and the logo.
//
// ⚠️ THE TITLE RUNS ACROSS THE WHOLE TOP, as it does in the original (it is 29 lines tall and
// nearly the full width): the logo plate sits under it, on the right, and the sheet under the
// plate.
constexpr const char* kStyle = R"(
#name {
    position: absolute; left: 5%; top: 20.5%; width: 76%;
    font-size: 24dp; color: #ffdf40; white-space: nowrap;
}
/* The kind of machine, and which of the circuit's vehicles this is. */
#kind { position: absolute; left: 5%; top: 26.5%; width: 76%; font-size: 16dp; color: #ffffff; white-space: nowrap; }
/* THE SHEET, right of the turning vehicle: ONE FLOWING COLUMN, so a longer line only pushes
   the rest down (2026-09-26). The original's `@X200,90..140` is the same column. */
#sheet { position: absolute; left: 52%; top: 40%; width: 46%; }
.stat { font-size: 16dp; color: #a0c0ff; margin-bottom: 10dp; white-space: nowrap; }
.stat span { color: #ffdf40; }
#colour, #logo_row {
    font-size: 16dp; color: #a0c0ff; background: #00000080; padding: 6dp 8dp;
    margin-top: 12dp; white-space: nowrap;
}
#colour { margin-top: 16dp; }
#colour:hover, #colour:focus, #logo_row:hover, #logo_row:focus { color: #ffdf40; }
/* The logo on its plate, under the title on the right. The two images are stacked: the
   hand-drawn plate, then the logo centred on it - `LOGOSCN`'s 58x58 `glbox` round 56x56. */
#plate { position: absolute; left: 84%; top: 20.5%; width: 13%; }
#plate img { position: absolute; left: 0; top: 0; width: 100%; }
#plate img.mark { left: 14%; top: 14%; width: 72%; }
)";

} // namespace

const char* CarScreen::document_source() const { return doc_.c_str(); }

bool CarScreen::enter(App& app) {
    text_.load(app.game_dir(), config_language(app.game_dir()));
    if (level_.cars.empty()) return false;
    if (index_ < 0 || index_ >= static_cast<int>(level_.cars.size())) index_ = 0;
    colour_ = app.profile().vehicle_color;
    const int presets = static_cast<int>(color_presets().size());
    if (presets > 0) colour_ = ((colour_ % presets) + presets) % presets;
    logo_ = app.profile().logo_index;
    load_artwork(app);
    // ⚠️ BEFORE the base class, both of them: on_ready() builds the preview out of `palette_`
    // and registers the logo images the document then asks for by name.
    logos_.load(app.game_dir());
    if (const int n = logos_.count(); n > 0) logo_ = ((logo_ % n) + n) % n;
    // The sprites through the environment's own CARSEL.RAW - the screen's one palette.
    scn_.load(app, "CARSCN", "CARSEL.RAW", "CARSCN.SPR", env_name_for_level(level_.name) + ".ENV");
    // No <img id="bg">: the picture is drawn by hand UNDER the turning vehicle (see draw()).
    doc_ = "<rml><head><style>" + scn_.focus_css() + kStyle + "</style></head><body>" +
           scn_.text_rml(text_, "CAR_HED") +
           "<div id=\"name\"></div><div id=\"kind\"></div>"
           "<div id=\"sheet\">"
           "<div class=\"stat\" id=\"stat0\"></div><div class=\"stat\" id=\"stat1\"></div>"
           "<div class=\"stat\" id=\"stat2\"></div><div class=\"stat\" id=\"stat3\"></div>"
           "<div class=\"stat\" id=\"stat4\"></div><div class=\"stat\" id=\"stat5\"></div>"
           "<div id=\"colour\"></div><div id=\"logo_row\"></div></div>"
           "<div id=\"plate\"></div>" +
           ScnScreen::box_open("next", 20, 165, 60, 35) + scn_.anisprite_rml(5, 5, 14, 162, 20, 165) +
           scn_.text_rml(text_, "CAR_NXT", 20, 165, "b") + "</div>" +
           ScnScreen::box_open("ok", 250, 155, 60, 45) + scn_.anisprite_rml(0, 5, 250, 155, 250, 155) +
           "</div></body></rml>";
    return RmlUiScreen::enter(app);
}

// `CARSEL.RAW`: the vehicle screen's OWN background, and the answer to a question this screen
// got wrong on its first day. It is not in `CARSCN.SCN` - that archive ships no `.RAW` at all -
// but in the `.ENV`s, one per environment family, and its name says what it is for. Eight
// hand-drawn plates: CATCH AIR over hot-air balloons for the helicopter circuits, eat DIRT over
// tyre tracks, Truckin' over a line of rigs, one per family.
//
// ⚠️ NOT the circuit's `VIEW.RAW`. That one is the race's presentation picture, title artwork
// and all, and it belongs to the screen underneath - the user said so as soon as they saw it.
//
// ⚠️ AND ITS PALETTE IS THE SCREEN'S PALETTE. The vehicle is drawn through the background's own
// 768 bytes, not through the level's `MAP.COL`, because that is what a 256-colour screen means:
// one palette for everything on it. It costs nothing in fidelity - the eight plates reserve the
// vehicles' own ramps, and the greys, the red ramp 57..63 and the blues 106/108 come out within
// a few per cent of the level's. Same rule as `TRAKSCN.SPR`'s buttons, palettised by the level
// they are drawn over.
void CarScreen::load_artwork(App& app) {
    const std::string env = env_name_for_level(level_.name);
    try {
        const Archive& arc = *game_archive(app.game_dir(), env + ".ENV");
        if (auto it = arc.find("CARSEL.RAW");
            it != arc.end() && it->second.size() >= kRawPaletteBytes) {
            palette_ = load_palette_rgb(it->second.data(), kRawPaletteBytes);
            background_.load(it->second);
            return;
        }
    } catch (const std::exception&) {
        // Falls through to the level's own palette below.
    }
    // `TEST.ENV` - the authors' sandbox - carries no `CARSEL.RAW`. Rather than show no vehicle
    // at all there, take the palette the race itself will use; the screen simply has no
    // backdrop. Nothing here is fatal: without either, the screen still names the vehicles,
    // which is the choice it exists to offer.
    try {
        const ArchivePtr lev = level_files(app.game_dir(), level_.name);
        if (auto it = lev->find("MAP.COL"); it != lev->end())
            palette_ = load_palette_rgb(it->second.data(), it->second.size());
    } catch (const std::exception&) {
    }
}

// The preset the player is on, as the BRIGHTEST step of the ramp the renderer will generate -
// see scene/vehicle_render.h. The preset table is a list of flat RGB, which is exactly that.
VehicleTint CarScreen::tint() const {
    const std::vector<ColorPreset>& presets = color_presets();
    if (presets.empty()) return VehicleTint{};
    const ColorPreset& p = presets[static_cast<size_t>(colour_) % presets.size()];
    return VehicleTint{p.r, p.g, p.b};
}

void CarScreen::exit(App& app) {
    display_.release();
    background_.release();
    RmlUiScreen::exit(app);
}

void CarScreen::on_ready(App& app) {
    if (document_) {
        const auto set = [this](const char* id, const Rml::String& html) {
            if (Rml::Element* el = document_->GetElementById(id)) el->SetInnerRML(html);
        };
    }
    register_logos(app);
    // OK, not BACK: the bottom pad button must mean "this is the one", not "forget it".
    set_focus_order({"colour", "logo_row", "next", "ok"}, "ok");
    refresh(app);
}

void CarScreen::refresh(App& app) {
    show_vehicle(app);
    if (!document_) return;
    const auto set = [this](const char* id, const Rml::String& html) {
        if (Rml::Element* el = document_->GetElementById(id)) el->SetInnerRML(html);
    };
    const std::string& car = level_.cars[static_cast<size_t>(index_)];

    // `INF_<NAME>`: what the machine is called, then what kind of machine it is - "Hurley Rough
    // 'n' Ready" / "Four Wheel Drive Off-Roader". Two LINES of one entry, which is why the
    // table had to learn to hand them back separately.
    //
    // ⚠️ THE PLAYER NEVER READS THE FILE NAME - same rule as the circuits, which show "KENYA"
    // and never "DIRT1" (game/level_catalog.h). It is the fallback and nothing more: `UFO` and
    // `FEHELI` are the two vehicles the tables say nothing about.
    const std::vector<std::string> info = text_.text_lines("INF_" + car);
    set("name", upper_of(info.empty() ? car : info[0]).c_str());
    // The kind of machine names the choice between the arrows. A vehicle the tables say
    // nothing about falls back on its name, so the row is never empty.
    // The kind of machine, and which of the circuit's vehicles it is.
    set("kind", (upper_of(info.size() > 1 ? info[1] : (info.empty() ? car : info[0])) + "   -   " +
                 std::to_string(index_ + 1) + " / " + std::to_string(level_.cars.size())).c_str());

    // The six statistics. Each line of `VHC_<NAME>` is two `@M` references - the label and the
    // qualitative word - and BOTH are ordinary entries of the same table, so they come out in
    // the player's language for free.
    const std::vector<std::vector<std::string>> stats = text_.macro_lines("VHC_" + car);
    for (int row = 0; row < kStatRows; ++row) {
        const std::string id = "stat" + std::to_string(row);
        if (static_cast<size_t>(row) >= stats.size() || stats[static_cast<size_t>(row)].size() < 2) {
            set(id.c_str(), "");
            continue;
        }
        const std::vector<std::string>& refs = stats[static_cast<size_t>(row)];
        const std::string label = upper_of(text_.text(refs[0]));
        const std::string value = upper_of(text_.text(refs[1]));
        if (label.empty() && value.empty()) { set(id.c_str(), ""); continue; }
        set(id.c_str(), (label + " <span>" + value + "</span>").c_str());
    }

    // The colour, named and shown at once: the preset's own RGB is the swatch, so the label
    // needs no second box next to it.
    const std::vector<ColorPreset>& presets = color_presets();
    if (!presets.empty()) {
        const ColorPreset& p = presets[static_cast<size_t>(colour_) % presets.size()];
        char rgb[8];
        std::snprintf(rgb, sizeof(rgb), "#%02x%02x%02x", p.r, p.g, p.b);
        set("colour", (upper_of(text_.text("CHA_COL", "COLOUR")) + ": <span style=\"color: " +
                       rgb + ";\">" + p.name + "</span>").c_str());
    }

    // The logo is a picture, so the row only counts: the plate top right IS the value.
    if (logos_.count() > 0) {
        set("logo_row", (upper_of(text_.text("LOGO_HED", "SELECT LOGO")) + ": <span>" +
                         std::to_string(logo_ + 1) + " / " +
                         std::to_string(logos_.count()) + "</span>").c_str());
    }
    refresh_logo();
}

// The model, rebuilt from the `.VHC` each time the choice moves. ⚠️ NOT KEPT PER VEHICLE: a
// level names up to a handful of cars and only one is ever on screen, so holding every mesh of
// the list would trade real memory for a load that is already imperceptible.
// The thirteen logos and the six plates' five frames, registered once under names the document
// then asks for like any other image. Nothing here is fatal: a copy of the game without
// `LOGOSCN.SCN` simply has no logo to offer.
void CarScreen::register_logos(App& app) {
    for (int i = 0; i < logos_.count(); ++i) {
        int w = 0, h = 0;
        std::vector<uint8_t> rgba = logos_.logo_rgba(i, w, h);
        if (!rgba.empty())
            app.ui().renderer().register_image(team_logo_image_name(i), std::move(rgba), w, h);
    }
    for (int c = 0; c < kTeamPlateColours; ++c) {
        for (int f = 0; f < kTeamPlateFrames; ++f) {
            int w = 0, h = 0;
            std::vector<uint8_t> rgba = logos_.plate_rgba(c, f, w, h);
            if (rgba.empty()) continue;
            app.ui().renderer().register_image(team_plate_image_name(c, f), std::move(rgba), w, h);
        }
    }
}

// ⚠️ THE TWO `<img>` ARE BUILT ONCE and only their `src` is swapped afterwards - rebuilding the
// element ten times a second would throw RmlUi's hover state away with it. Same rule as the
// globe's widgets (rule 8 of the menu flow spec).
void CarScreen::refresh_logo() {
    if (!document_ || !logos_.ready()) return;
    Rml::Element* box = document_->GetElementById("plate");
    if (!box) return;
    const VehicleTint t = tint();
    const int plate = logos_.nearest_plate(t.r, t.g, t.b);
    if (box->GetNumChildren() < 2) {
        box->SetInnerRML("<img/><img class=\"mark\"/>");
        if (box->GetNumChildren() < 2) return;
    }
    box->GetChild(0)->SetAttribute("src", team_plate_image_name(plate, plate_frame_));
    box->GetChild(1)->SetAttribute("src", team_logo_image_name(logo_));
}

// ⚠️ `turn_` IS LEFT ALONE: the new model - another vehicle, or the same one repainted - picks
// up the turn where the last one was, so a change of colour looks like the paint changing and
// not like the vehicle being put back on its mark (user, 2026-09-25).
void CarScreen::show_vehicle(App& app) {
    display_.release();
    if (palette_.empty()) return;
    const VehicleTint t = tint();
    // The chosen emblem, painted into the sheet's own logo cell - so the car in the preview
    // wears it exactly where the race will put it. The pixels only have to live until load()
    // returns: it copies them into the sheet there and then.
    int lw = 0, lh = 0;
    const std::vector<uint8_t> emblem = logos_.logo_rgba(logo_, lw, lh);
    VehicleLogo vl;
    vl.rgba = emblem.empty() ? nullptr : emblem.data();
    vl.w = lw;
    vl.h = lh;
    // The player's driver at the wheel, as the race will seat them (scene/character_model.h).
    VehicleDriver driver;
    VehicleTint driver_tint;
    CharacterShapes shapes;
    if (load_character_shapes(app.game_dir(), app.profile().character, shapes)) {
        driver.head = &shapes.head;
        driver.mouth_tm = &shapes.mouth_tm;
        if (const std::vector<ColorPreset>& presets = color_presets(); !presets.empty()) {
            const ColorPreset& p =
                presets[static_cast<size_t>(app.profile().character_color) % presets.size()];
            driver_tint = VehicleTint{p.r, p.g, p.b};
            driver.tint = &driver_tint;
        }
    }
    display_.load(app.game_dir(), level_.cars[static_cast<size_t>(index_)], palette_, &t,
                  vl.valid() ? &vl : nullptr, driver.head ? &driver : nullptr);
}

void CarScreen::update(App& app, double dt_ms) {
    // ⚠️ A CLOCK, NEVER A FRAME COUNT: the player may be at
    // 240 fps, and the vehicle must turn at the same speed whatever they are at.
    turn_ += kTurnsPerSecond * static_cast<float>(dt_ms) / 1000.0f;
    turn_ -= std::floor(turn_);
    anim_ms_ += dt_ms;
    if (const int f = static_cast<int>(anim_ms_ / 110.0) % kTeamPlateFrames; f != plate_frame_) {
        plate_frame_ = f;
        refresh_logo();
    }
    RmlUiScreen::update(app, dt_ms);
}

void CarScreen::draw(App& app) {
    // ⚠️ ORDER MATTERS: the artwork, then the vehicle, then the document. RmlUi paints last, so
    // anything the document holds is painted OVER the car - which is why the background is a
    // `.RAW` drawn by hand here instead of an `<img>` in the document. Same trap as the globe's.
    background_.draw();
    if (canvas_w_ > 0 && canvas_h_ > 0) {
        const float cw = static_cast<float>(canvas_w_), ch = static_cast<float>(canvas_h_);
        app.presentation().draw_model(
            kViewX * cw, kViewY * ch, kViewW * cw, kViewH * ch,
            [&](int tw, int th, float x, float y, float w, float h) {
                display_.draw(tw, th, x, y, w, h, turn_);
            });
    }
    RmlUiScreen::draw(app);
}

void CarScreen::on_activate(App& app, const std::string& id) {
    if (id == "next" || id == "prev") {
        const int n = static_cast<int>(level_.cars.size());
        index_ = (index_ + (id == "next" ? 1 : n - 1)) % n;
        refresh(app);
        return;
    }
    if (id == "colour") {
        // Cyclic, like `CHARSCN`'s own `col` label - the user's description of the original.
        const int presets = static_cast<int>(color_presets().size());
        if (presets > 0) colour_ = (colour_ + 1) % presets;
        refresh(app);        // the model is rebuilt: the ramp is baked into its vertices
        return;
    }
    if (id == "logo_row") {
        // ⚠️ REBUILDS THE MODEL, because the emblem is painted INTO the texture sheet rather
        // than selected by a UV - see scene/vehicle_render.h. refresh() does that for us.
        if (const int n = logos_.count(); n > 0) logo_ = (logo_ + 1) % n;
        refresh(app);
        return;
    }
    if (id == "back") {
        // The original's own `CAR_BAK`, and it is not OK: it leaves the choice as it was found.
        app.pop();
        return;
    }
    if (id == "ok") {
        // ⚠️ WRITTEN BACK BEFORE THE POP, never after: `pop()` is deferred to the end of the
        // frame (app/app.h) but this object is destroyed by it, so anything it has to say must
        // be said now.
        if (selection_) *selection_ = index_;
        // ⚠️ THE COLOUR IS THE PROFILE'S and outlives the race, so it is SAVED, not just kept
        // in memory - a player who picks a colour and quits expects it back. BACK writes
        // nothing, which is what makes it a way out rather than a second OK.
        if (app.profile().vehicle_color != colour_ || app.profile().logo_index != logo_) {
            app.profile().vehicle_color = colour_;
            app.profile().logo_index = logo_;
            save_profile(app.profile());
        }
        app.pop();
        return;
    }
}

} // namespace bigred
