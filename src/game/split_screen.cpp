#include "split_screen.h"

#include <random>

#include <RmlUi/Core.h>
#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>
#include <unordered_map>

#include "app/app.h"
#include "game/competition.h"
#include "app/player_profile.h"
#include "formats/game_files.h"
#include "formats/level_script.h"   // env_name_for_level
#include "formats/texture.h"        // kRawPaletteBytes, load_palette_rgb
#include "game/globe_screen.h"
#include "game/track_screen.h"
#include "scene/character_model.h"

namespace bigred {
namespace {

// A quarter of the canvas, as fractions: two columns, two rows, the bottom strip left to the
// hint line. Quarter 0 is top left and belongs to player one.
constexpr float kQuarterW = 0.485f, kQuarterH = 0.455f;
float quarter_x(int q) { return 0.01f + static_cast<float>(q % 2) * 0.495f; }
float quarter_y(int q) { return 0.01f + static_cast<float>(q / 2) * 0.465f; }

// One style for both screens. ⚠️ A QUARTER HAS NO BACKGROUND, only a border: the model is
// drawn before the document, and a box filled over it would dim it. Only the text columns are
// filled.
constexpr const char* kStyle = R"(
.quarter {
    position: absolute; width: 48.5%; height: 45.5%;
    border-width: 2dp; border-color: #60607080;
}
#q0 { left: 1%; top: 1%; }
#q1 { left: 50.5%; top: 1%; }
#q2 { left: 1%; top: 47.5%; }
#q3 { left: 50.5%; top: 47.5%; }
.quarter.ready { border-color: #60ff60; }
/* Who plays in this quarter, in the wibbly lettering - the original's own 1P_CHAR / 2P_CHAR
   ("PLAYER 1 SELECT") heading its two-player screens - at its smallest size, so it stays above
   the sheet. Their controls, and everything in the quarter, are read: steady lettering. */
.qhead {
    position: absolute; left: 3%; top: 1%; width: 94%; font-family: wibs; font-size: 29dp;
    color: #ffdf40; white-space: nowrap;
}
.qhead span { font-family: bigred; font-size: 16dp; color: #a0a0c0; margin-left: 12dp; }
.sheet {
    position: absolute; left: 44%; top: 13%; width: 53%; padding: 6dp 1%;
    background: #00000090;
}
.label { font-size: 16dp; color: #a0a0c0; white-space: nowrap; }
.line { font-size: 16dp; margin-bottom: 8dp; white-space: nowrap; }
.line .arrow { display: inline-block; width: 14%; text-align: center; color: #808080; }
.line .value {
    display: inline-block; width: 72%; text-align: center; background: #00000080;
}
.line.on .arrow { color: #ffdf40; }
.line.on .value { color: #ffdf40; background: #404050c0; }
.line img { display: inline-block; width: 16dp; height: 16dp; vertical-align: middle; }
.line .mark { display: inline-block; width: 16dp; height: 16dp; margin-right: 8dp; }
.keys { font-size: 16dp; color: #c0c0c0; white-space: nowrap; margin-top: 4dp; }
.keys.ok { color: #80ff80; }
.join {
    position: absolute; left: 5%; top: 28%; width: 90%; text-align: center;
    font-size: 16dp; color: #ffffff;
}
.join .dim { color: #a0a0c0; }
.car {
    position: absolute; left: 4%; top: 70%; width: 92%; padding: 4dp 0;
    background: #00000090; text-align: center;
}
.car .line { margin: 0 0 4dp 0; }
.car .line .value { width: 76%; white-space: normal; }
.car .line .arrow { width: 12%; color: #ffdf40; }
)";

int wrap(int v, int n) { return n <= 0 ? 0 : ((v % n) + n) % n; }

std::string upper(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '&') out += "&amp;";
        else out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string hex(const ColorPreset& p) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", p.r, p.g, p.b);
    return buf;
}

const ColorPreset* preset(int index) {
    const std::vector<ColorPreset>& presets = color_presets();
    return presets.empty() ? nullptr : &presets[static_cast<size_t>(wrap(index, static_cast<int>(presets.size())))];
}

// The four quarters' shells, then the hint line. Their content is rebuilt by each screen.
std::string quarters_document() {
    std::string doc = "<rml><head><style>" + std::string(kStyle) + "</style></head><body>";
    for (int q = 0; q < kMaxLocalPlayers; ++q)
        doc += "<div class=\"quarter\" id=\"q" + std::to_string(q) + "\"></div>";
    doc += "<div id=\"hint\"></div></body></rml>";
    return doc;
}

// The quarter's rectangle in canvas pixels, and the part of it a model is drawn in.
void quarter_box(int q, int cw, int ch, float fx, float fy, float fw, float fh, float& x, float& y,
                 float& w, float& h) {
    const float qx = quarter_x(q) * static_cast<float>(cw), qy = quarter_y(q) * static_cast<float>(ch);
    const float qw = kQuarterW * static_cast<float>(cw), qh = kQuarterH * static_cast<float>(ch);
    x = qx + fx * qw;
    y = qy + fy * qh;
    w = fw * qw;
    h = fh * qh;
}

void register_logo_images(App& app, const TeamLogos& logos) {
    for (int i = 0; i < logos.count(); ++i) {
        int w = 0, h = 0;
        std::vector<uint8_t> rgba = logos.logo_rgba(i, w, h);
        if (!rgba.empty())
            app.ui().renderer().register_image(team_logo_image_name(i), std::move(rgba), w, h);
    }
}

// "< VALUE >", highlighted when the player is on it.
std::string picker_line(const std::string& value, bool on) {
    return std::string("<div class=\"line") + (on ? " on" : "") +
           "\"><span class=\"arrow\">&lt;</span><span class=\"value\">" + value +
           "</span><span class=\"arrow\">&gt;</span></div>";
}

} // namespace

// --- whose press is this ----------------------------------------------------------------------

const char* ok_key_name(Controls c) {
    switch (c) {
    case Controls::KeyboardRight: return "ENTER";
    case Controls::KeyboardLeft: return "SPACE";
    default: return "A";
    }
}

const char* back_key_name(Controls c) {
    switch (c) {
    case Controls::KeyboardRight: return "BKSP";
    case Controls::KeyboardLeft: return "TAB";
    default: return "B";
    }
}

bool SeatInputs::read(App& app, const SDL_Event& e, SeatPress& out) {
    if (!armed_init_) {
        for (auto& pad : armed_)
            for (auto& axis : pad) axis[0] = axis[1] = true;
        armed_init_ = true;
    }
    out = SeatPress();
    // ⚠️ BY SCANCODE, the key's POSITION: the left half is W A S D on a QWERTY board and
    // Z Q S D on an AZERTY one, under the same fingers (app/local_players.h).
    if (e.type == SDL_KEYDOWN) {
        if (e.key.repeat) return false;
        switch (e.key.keysym.scancode) {
        case SDL_SCANCODE_UP: out = {Controls::KeyboardRight, MenuVerb::Up}; return true;
        case SDL_SCANCODE_DOWN: out = {Controls::KeyboardRight, MenuVerb::Down}; return true;
        case SDL_SCANCODE_LEFT: out = {Controls::KeyboardRight, MenuVerb::Left}; return true;
        case SDL_SCANCODE_RIGHT: out = {Controls::KeyboardRight, MenuVerb::Right}; return true;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: out = {Controls::KeyboardRight, MenuVerb::Ok}; return true;
        case SDL_SCANCODE_BACKSPACE: out = {Controls::KeyboardRight, MenuVerb::Back}; return true;
        case SDL_SCANCODE_W: out = {Controls::KeyboardLeft, MenuVerb::Up}; return true;
        case SDL_SCANCODE_S: out = {Controls::KeyboardLeft, MenuVerb::Down}; return true;
        case SDL_SCANCODE_A: out = {Controls::KeyboardLeft, MenuVerb::Left}; return true;
        case SDL_SCANCODE_D: out = {Controls::KeyboardLeft, MenuVerb::Right}; return true;
        case SDL_SCANCODE_SPACE: out = {Controls::KeyboardLeft, MenuVerb::Ok}; return true;
        case SDL_SCANCODE_TAB: out = {Controls::KeyboardLeft, MenuVerb::Back}; return true;
        default: return false;
        }
    }
    if (e.type == SDL_CONTROLLERBUTTONDOWN) {
        const int pad = app.pad_index_of(e.cbutton.which);
        if (pad < 0 || pad >= kMaxLocalPlayers) return false;
        out.who = static_cast<Controls>(static_cast<int>(Controls::Pad1) + pad);
        switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: out.verb = MenuVerb::Up; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: out.verb = MenuVerb::Down; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: out.verb = MenuVerb::Left; break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: out.verb = MenuVerb::Right; break;
        case SDL_CONTROLLER_BUTTON_A:
        case SDL_CONTROLLER_BUTTON_START: out.verb = MenuVerb::Ok; break;
        case SDL_CONTROLLER_BUTTON_B: out.verb = MenuVerb::Back; break;
        default: out.verb = MenuVerb::Ok; break;   // any other button: "I am here"
        }
        return true;
    }
    if (e.type == SDL_CONTROLLERAXISMOTION &&
        (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX || e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY)) {
        const int pad = app.pad_index_of(e.caxis.which);
        if (pad < 0 || pad >= kMaxLocalPlayers) return false;
        const int axis = e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX ? 0 : 1;
        bool* armed = armed_[pad][axis];
        static constexpr int kThreshold = 16000;
        const int v = e.caxis.value;
        if (v > -kThreshold / 2) armed[0] = true;
        if (v < kThreshold / 2) armed[1] = true;
        out.who = static_cast<Controls>(static_cast<int>(Controls::Pad1) + pad);
        if (v < -kThreshold && armed[0]) {
            armed[0] = false;
            out.verb = axis == 0 ? MenuVerb::Left : MenuVerb::Up;
            return true;
        }
        if (v > kThreshold && armed[1]) {
            armed[1] = false;
            out.verb = axis == 0 ? MenuVerb::Right : MenuVerb::Down;
            return true;
        }
    }
    return false;
}

// --- the seats --------------------------------------------------------------------------------

LocalPlayers default_local_players(App& app, int players) {
    const PlayerProfile& p = app.profile();
    static constexpr Controls kDefaults[kMaxLocalPlayers] = {
        Controls::KeyboardRight, Controls::KeyboardLeft, Controls::Pad1, Controls::Pad2};
    LocalPlayers out;
    for (int i = 0; i < players && i < kMaxLocalPlayers; ++i) {
        LocalSeat s;
        s.controls = kDefaults[i];
        if (i == 0) {
            s.name = p.pseudo.empty() ? std::string("PLAYER 1") : p.pseudo;
            s.character = p.character;
            s.character_color = p.character_color;
            s.vehicle_color = p.vehicle_color;
            s.logo_index = p.logo_index;
        } else {
            // Different from the first player's at a glance: another driver, other colours,
            // another emblem.
            s.name = "PLAYER " + std::to_string(i + 1);
            s.character = wrap(p.character + i, kCharacterCount);
            const int n = static_cast<int>(color_presets().size());
            s.vehicle_color = wrap(p.vehicle_color + i * 5, n);
            s.character_color = wrap(p.character_color + i * 5 + 2, n);
            s.logo_index = p.logo_index + i;
        }
        out.seats.push_back(s);
    }
    return out;
}

void fill_split_setup(App& app, const LevelEntry& level, int main_vehicle, RaceScreen::Setup& setup) {
    const LocalPlayers& locals = app.locals();
    if (!locals.split() || level.cars.empty()) return;
    const int n_cars = static_cast<int>(level.cars.size());
    TeamLogos logos;
    const bool have_logos = logos.load(app.game_dir()) && logos.count() > 0;
    const auto emblem = [&](int index) {
        std::vector<uint8_t> rgba;
        if (!have_logos) return rgba;
        int lw = 0, lh = 0;
        rgba = logos.logo_rgba(wrap(index, logos.count()), lw, lh);
        if (lw != kLogoCell || lh != kLogoCell) rgba.clear();
        return rgba;
    };
    // Player one: the seat, not the profile - it may have been changed on the split screen.
    const LocalSeat& first = locals.seats[0];
    setup.player_name = first.name;
    if (first.vehicle >= 0 && first.vehicle < n_cars) main_vehicle = first.vehicle;
    setup.vehicle = level.cars[static_cast<size_t>(wrap(main_vehicle, n_cars))];
    if (const ColorPreset* p = preset(first.vehicle_color)) {
        setup.tinted = true;
        setup.tint = VehicleTint{p->r, p->g, p->b};
    }
    setup.character = first.character;
    setup.character_colour = first.character_color;
    setup.logo = emblem(first.logo_index);

    setup.split_side_by_side = app.profile().split_side_by_side;
    setup.rules.speed_percent = speed_percent_of(app.profile());
    setup.controls.clear();
    for (const LocalSeat& s : locals.seats) setup.controls.push_back(s.controls);
    for (size_t k = 1; k < locals.seats.size(); ++k) {
        const LocalSeat& s = locals.seats[k];
        RaceScreen::Setup::Guest g;
        g.name = s.name;
        const int v = s.vehicle >= 0 && s.vehicle < n_cars ? s.vehicle
                                                           : main_vehicle + static_cast<int>(k);
        g.vehicle = level.cars[static_cast<size_t>(wrap(v, n_cars))];
        g.damage_option = app.profile().damage_option;
        if (const ColorPreset* p = preset(s.vehicle_color)) {
            g.tinted = true;
            g.tint = VehicleTint{p->r, p->g, p->b};
        }
        g.character = s.character;
        g.character_colour = s.character_color;
        g.logo = emblem(s.logo_index);
        setup.guests.push_back(std::move(g));
    }
}

// --- the first screen: who plays, and as whom -------------------------------------------------

namespace {
// The lines of a quarter's sheet, in order.
enum Row {
    kRowDriver, kRowDriverColour, kRowCarColour, kRowLogo, kRowCount,
    kRowRaces = kRowCount, kRowTwoSplit
};
// Player one's quarter has a row more, how many races, chosen or drawn; and with exactly two
// players in, one more again: how they share the screen, one above the other or side by side
// (user, 2026-09-30). Only then - it means nothing to three, and a quarter has no room for it.
int rows_of(int quarter, int joined) {
    return quarter == 0 ? kRowCount + (joined == 2 ? 2 : 1) : kRowCount;
}
constexpr int kRaceOptions = 6;   // 1, 2, 3 chosen on the globe; 1, 2, 3 drawn at random
constexpr double kStartDelayMs = 2000.0;
} // namespace

const char* SplitScreenScreen::document_source() const {
    static const std::string doc = quarters_document();
    return doc.c_str();
}

bool SplitScreenScreen::enter(App& app) {
    const LocalPlayers seeds = default_local_players(app, kMaxLocalPlayers);
    for (int q = 0; q < kMaxLocalPlayers; ++q)
        quarters_[static_cast<size_t>(q)].seat = seeds.seats[static_cast<size_t>(q)];
    // No picture and no drivers: the quarters still say who is in and what they chose.
    if (const Blob* raw = find_member(*game_archive_or_empty(app.game_dir(), "CHARSCN.SCN"),
                                      "DRIVER.RAW");
        raw && raw->size() >= kRawPaletteBytes)
        driver_palette_ = load_palette_rgb(raw->data(), kRawPaletteBytes);
    if (const Blob* raw = find_member(*game_archive_or_empty(app.game_dir(), "CHOOSCN.SCN"),
                                      "CONFIG4B.RAW"))
        background_.load(*raw);
    logos_.load(app.game_dir());
    // The screenshot tool's quarters: joined on the controls they would have by default.
    for (int q = 0; q < demo_ && q < kMaxLocalPlayers; ++q) {
        Quarter& qt = quarters_[static_cast<size_t>(q)];
        qt.taken = true;
        qt.ready = q == 0;
        qt.row = q % kRowCount;
    }
    return RmlUiScreen::enter(app);
}

void SplitScreenScreen::exit(App& app) {
    // Left for the menu: nobody is playing at this machine any more, and no series is on.
    app.locals().clear();
    app.competition().end();
    for (Quarter& q : quarters_) q.display.release();
    background_.release();
    RmlUiScreen::exit(app);
}

// Back from the circuits: everyone still in, and asked again to be ready.
void SplitScreenScreen::resumed(App& app) {
    RmlUiScreen::resumed(app);
    for (Quarter& q : quarters_) q.ready = false;
    countdown_ms_ = -1.0;
    refresh(app);
}

void SplitScreenScreen::on_ready(App& app) {
    register_logo_images(app, logos_);
    for (int q = 0; q < kMaxLocalPlayers; ++q)
        if (quarters_[static_cast<size_t>(q)].taken) show_driver(app, q);
    refresh(app);
}

int SplitScreenScreen::joined() const {
    int n = 0;
    for (const Quarter& q : quarters_) n += q.taken ? 1 : 0;
    return n;
}

bool SplitScreenScreen::all_ready() const {
    for (const Quarter& q : quarters_)
        if (q.taken && !q.ready) return false;
    return joined() >= 2;
}

void SplitScreenScreen::show_driver(App& app, int q) {
    Quarter& qt = quarters_[static_cast<size_t>(q)];
    qt.display.release();
    if (driver_palette_.empty()) return;
    const ColorPreset* p = preset(qt.seat.character_color);
    const VehicleTint tint = p ? VehicleTint{p->r, p->g, p->b} : VehicleTint{};
    qt.display.load_character(app.game_dir(), qt.seat.character, driver_palette_, p ? &tint : nullptr);
}

bool SplitScreenScreen::handle_event(App& app, const SDL_Event& e) {
    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE && !e.key.repeat) {
        app.pop();
        return true;
    }
    SeatPress p;
    if (!inputs_.read(app, e, p)) return false;
    press(app, p);
    return true;
}

void SplitScreenScreen::press(App& app, const SeatPress& p) {
    int q = -1;
    for (int i = 0; i < kMaxLocalPlayers; ++i)
        if (quarters_[static_cast<size_t>(i)].taken && quarters_[static_cast<size_t>(i)].seat.controls == p.who)
            q = i;
    if (q < 0) {
        // Not in yet: anything but "back" takes the first free quarter.
        if (p.verb == MenuVerb::Back) return;
        for (int i = 0; i < kMaxLocalPlayers && q < 0; ++i)
            if (!quarters_[static_cast<size_t>(i)].taken) q = i;
        if (q < 0) return;   // four already
        Quarter& qt = quarters_[static_cast<size_t>(q)];
        qt.taken = true;
        qt.ready = false;
        qt.row = 0;
        qt.seat.controls = p.who;
        // A third player in hides player one's SCREEN row: their cursor must not stay on it.
        Quarter& first = quarters_[0];
        if (first.row >= rows_of(0, joined())) first.row = 0;
        show_driver(app, q);
        refresh(app);
        return;
    }
    Quarter& qt = quarters_[static_cast<size_t>(q)];
    LocalSeat& s = qt.seat;
    switch (p.verb) {
    case MenuVerb::Back:
        // Ready: back to changing things. Not ready: out of the game, the quarter free again.
        if (qt.ready) qt.ready = false;
        else {
            qt.taken = false;
            qt.display.release();
        }
        break;
    case MenuVerb::Ok:
        qt.ready = true;
        break;
    case MenuVerb::Up:
    case MenuVerb::Down:
        if (!qt.ready) qt.row = wrap(qt.row + (p.verb == MenuVerb::Down ? 1 : -1), rows_of(q, joined()));
        break;
    case MenuVerb::Left:
    case MenuVerb::Right: {
        if (qt.ready) break;
        const int step = p.verb == MenuVerb::Right ? 1 : -1;
        const int colours = static_cast<int>(color_presets().size());
        switch (qt.row) {
        case kRowDriver:
            s.character = wrap(s.character + step, kCharacterCount);
            show_driver(app, q);
            break;
        case kRowDriverColour:
            s.character_color = wrap(s.character_color + step, colours);
            show_driver(app, q);
            break;
        case kRowCarColour: s.vehicle_color = wrap(s.vehicle_color + step, colours); break;
        case kRowLogo:
            if (logos_.count() > 0) s.logo_index = wrap(s.logo_index + step, logos_.count());
            break;
        case kRowRaces: races_option_ = wrap(races_option_ + step, kRaceOptions); break;
        case kRowTwoSplit:
            app.profile().split_side_by_side = !app.profile().split_side_by_side;
            app.save_profile_now();
            break;
        default: break;
        }
        break;
    }
    case MenuVerb::None: break;
    }
    refresh(app);
}

void SplitScreenScreen::refresh(App& app) {
    if (!document_) return;
    for (int q = 0; q < kMaxLocalPlayers; ++q) {
        Quarter& qt = quarters_[static_cast<size_t>(q)];
        Rml::Element* box = document_->GetElementById("q" + std::to_string(q));
        if (!box) continue;
        box->SetClass("ready", qt.taken && qt.ready);
        const std::string who = "PLAYER " + std::to_string(q + 1);
        if (!qt.taken) {
            box->SetInnerRML("<div class=\"qhead\"><span>" + who + "</span></div>"
                             "<div class=\"join wrap-ok\">TO JOIN, PRESS<br/><span class=\"dim\">A PAD "
                             "BUTTON<br/>AN ARROW KEY<br/>OR W A S D</span></div>");
            continue;
        }
        const LocalSeat& s = qt.seat;
        std::string rml = "<div class=\"qhead\">" + upper(s.name) + " <span>" +
                          controls_name(s.controls) + "</span></div><div class=\"sheet\">";
        const auto field = [&](const char* label, const std::string& value, int row) {
            rml += std::string("<div class=\"label\">") + label + "</div>" +
                   picker_line(value, !qt.ready && qt.row == row);
        };
        field("DRIVER", upper(character_info(s.character).first_name), kRowDriver);
        const ColorPreset* dc = preset(s.character_color);
        field("DRIVER COLOUR",
              dc ? "<span style=\"color: " + hex(*dc) + ";\">" + upper(dc->name) + "</span>" : "-",
              kRowDriverColour);
        const ColorPreset* vc = preset(s.vehicle_color);
        field("CAR COLOUR",
              vc ? "<span style=\"color: " + hex(*vc) + ";\">" + upper(vc->name) + "</span>" : "-",
              kRowCarColour);
        const int n_logos = logos_.count();
        field("EMBLEM",
              n_logos > 0 ? "<img class=\"mark\" src=\"" +
                                team_logo_image_name(wrap(s.logo_index, n_logos)) + "\"/>" +
                                std::to_string(wrap(s.logo_index, n_logos) + 1) + "/" +
                                std::to_string(n_logos)
                          : std::string("-"),
              kRowLogo);
        if (q == 0)
            field("RACES", std::to_string(races_option_ % 3 + 1) +
                               (races_option_ >= 3 ? " - RANDOM" : " - CHOSEN"),
                  kRowRaces);
        if (q == 0 && joined() == 2)
            field("SCREEN", app.profile().split_side_by_side ? "LEFT / RIGHT" : "TOP / BOTTOM",
                  kRowTwoSplit);
        if (qt.ready)
            rml += "<div class=\"keys ok\">READY!</div><div class=\"keys\">" +
                   std::string(back_key_name(s.controls)) + ": CHANGE</div>";
        else
            rml += "<div class=\"keys\">" + std::string(ok_key_name(s.controls)) +
                   ": READY</div><div class=\"keys\">" + back_key_name(s.controls) +
                   ": LEAVE</div>";
        rml += "</div>";
        box->SetInnerRML(rml);
    }
}

void SplitScreenScreen::update(App& app, double dt_ms) {
    turn_ += 0.07f * static_cast<float>(dt_ms) / 1000.0f;
    turn_ -= std::floor(turn_);
    // Everyone in is ready: a moment for a late arrival, then the circuits.
    if (!all_ready()) countdown_ms_ = -1.0;
    else if (countdown_ms_ < 0.0) countdown_ms_ = kStartDelayMs;
    else if ((countdown_ms_ -= dt_ms) <= 0.0) {
        countdown_ms_ = -1.0;
        LocalPlayers chosen;
        for (const Quarter& q : quarters_)
            if (q.taken) chosen.seats.push_back(q.seat);
        app.locals() = chosen;
        app.competition().start_series(static_cast<int>(chosen.seats.size()),
                                       races_option_ % 3 + 1, races_option_ >= 3);
        Competition::LocalSeries& ser = app.competition().series();
        if (ser.random_races) {
            // Drawn: distinct circuits with something to drive, then straight to the first.
            std::vector<LevelEntry> pool;
            for (const LevelEntry& l : scan_levels(app.game_dir()))
                if (!l.cars.empty()) pool.push_back(l);
            std::mt19937 rng{std::random_device{}()};
            std::shuffle(pool.begin(), pool.end(), rng);
            for (int i = 0; i < ser.races && static_cast<size_t>(i) < pool.size(); ++i)
                ser.circuits.push_back(pool[static_cast<size_t>(i)].name);
            if (!pool.empty()) {
                std::printf("[split] %d race(s) drawn:", ser.races);
                for (const std::string& n : ser.circuits) std::printf(" %s", n.c_str());
                std::printf("\n");
                app.push(std::make_unique<TrackScreen>(pool[0]));
                return;
            }
        }
        app.push(std::make_unique<GlobeScreen>());
    }
    if (document_) {
        std::string hint;
        const int n = joined();
        if (countdown_ms_ >= 0.0)
            hint = "EVERYONE IS READY - THE CIRCUITS IN " +
                   std::to_string(static_cast<int>(countdown_ms_ / 1000.0) + 1);
        else if (n < 2)
            hint = "TWO PLAYERS AT LEAST - ESC: BACK";
        else
            hint = "WAITING FOR EVERYONE TO BE READY - ESC: BACK";
        if (Rml::Element* el = document_->GetElementById("hint"); el && el->GetInnerRML() != hint)
            el->SetInnerRML(hint);
    }
    RmlUiScreen::update(app, dt_ms);
}

void SplitScreenScreen::draw(App& app) {
    // The artwork, the drivers, then the document - RmlUi paints last (car_screen.cpp).
    background_.draw();
    if (canvas_w_ > 0 && canvas_h_ > 0) {
        for (int q = 0; q < kMaxLocalPlayers; ++q) {
            Quarter& qt = quarters_[static_cast<size_t>(q)];
            if (!qt.taken) continue;
            float x, y, w, h;
            quarter_box(q, canvas_w_, canvas_h_, 0.02f, 0.12f, 0.40f, 0.84f, x, y, w, h);
            const float t = turn_ + 0.25f * static_cast<float>(q);
            app.presentation().draw_model(x, y, w, h, [&](int tw, int th, float mx, float my,
                                                          float mw, float mh) {
                qt.display.draw(tw, th, mx, my, mw, mh, t - std::floor(t));
            });
        }
    }
    RmlUiScreen::draw(app);
}

// --- the second screen: what each one drives -------------------------------------------------

const char* SplitCarScreen::document_source() const {
    static const std::string doc = quarters_document();
    return doc.c_str();
}

bool SplitCarScreen::enter(App& app) {
    if (level_.cars.empty() || !app.locals().split()) return false;
    players_ = std::min(static_cast<int>(app.locals().seats.size()), kMaxLocalPlayers);
    const int n = static_cast<int>(level_.cars.size());
    for (int q = 0; q < players_; ++q) {
        const int was = app.locals().seats[static_cast<size_t>(q)].vehicle;
        // Where this player left it, or a different vehicle from the one before.
        quarters_[static_cast<size_t>(q)].vehicle = was >= 0 && was < n ? was : wrap(q, n);
    }
    text_.load(app.game_dir(), config_language(app.game_dir()));
    logos_.load(app.game_dir());
    // `CARSEL.RAW`, the vehicle screen's own background and palette (car_screen.cpp).
    try {
        const Archive& arc = *game_archive(app.game_dir(), env_name_for_level(level_.name) + ".ENV");
        if (const auto it = arc.find("CARSEL.RAW");
            it != arc.end() && it->second.size() >= kRawPaletteBytes) {
            palette_ = load_palette_rgb(it->second.data(), kRawPaletteBytes);
            background_.load(it->second);
        }
    } catch (const std::exception&) {
    }
    if (palette_.empty()) {
        try {
            const ArchivePtr lev = level_files(app.game_dir(), level_.name);
            if (const auto it = lev->find("MAP.COL"); it != lev->end())
                palette_ = load_palette_rgb(it->second.data(), it->second.size());
        } catch (const std::exception&) {
        }
    }
    return RmlUiScreen::enter(app);
}

void SplitCarScreen::exit(App& app) {
    for (Quarter& q : quarters_) q.display.release();
    background_.release();
    RmlUiScreen::exit(app);
}

void SplitCarScreen::on_ready(App& app) {
    for (int q = 0; q < players_; ++q) show_vehicle(app, q);
    refresh(app);
}

// The model as the race will draw it: the seat's colour, its emblem, its driver at the wheel.
void SplitCarScreen::show_vehicle(App& app, int q) {
    Quarter& qt = quarters_[static_cast<size_t>(q)];
    qt.display.release();
    if (palette_.empty()) return;
    const LocalSeat& s = app.locals().seats[static_cast<size_t>(q)];
    const ColorPreset* vc = preset(s.vehicle_color);
    const VehicleTint tint = vc ? VehicleTint{vc->r, vc->g, vc->b} : VehicleTint{};
    int lw = 0, lh = 0;
    const std::vector<uint8_t> emblem =
        logos_.count() > 0 ? logos_.logo_rgba(wrap(s.logo_index, logos_.count()), lw, lh)
                           : std::vector<uint8_t>();
    VehicleLogo logo;
    logo.rgba = emblem.empty() ? nullptr : emblem.data();
    logo.w = lw;
    logo.h = lh;
    VehicleDriver driver;
    VehicleTint driver_tint;
    CharacterShapes shapes;
    if (load_character_shapes(app.game_dir(), s.character, shapes)) {
        driver.head = &shapes.head;
        driver.mouth_tm = &shapes.mouth_tm;
        if (const ColorPreset* dc = preset(s.character_color)) {
            driver_tint = VehicleTint{dc->r, dc->g, dc->b};
            driver.tint = &driver_tint;
        }
    }
    qt.display.load(app.game_dir(), level_.cars[static_cast<size_t>(qt.vehicle)], palette_,
                    vc ? &tint : nullptr, logo.valid() ? &logo : nullptr,
                    driver.head ? &driver : nullptr);
}

bool SplitCarScreen::handle_event(App& app, const SDL_Event& e) {
    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE && !e.key.repeat) {
        app.pop();   // back to the circuit, nothing chosen
        return true;
    }
    SeatPress p;
    if (!inputs_.read(app, e, p)) return false;
    int q = -1;
    for (int i = 0; i < players_; ++i)
        if (app.locals().seats[static_cast<size_t>(i)].controls == p.who) q = i;
    if (q < 0) return true;   // not one of the players
    Quarter& qt = quarters_[static_cast<size_t>(q)];
    const int n = static_cast<int>(level_.cars.size());
    switch (p.verb) {
    case MenuVerb::Ok: qt.ready = true; break;
    case MenuVerb::Back: qt.ready = false; break;
    case MenuVerb::Left:
    case MenuVerb::Right:
    case MenuVerb::Up:
    case MenuVerb::Down:
        if (qt.ready) break;
        qt.vehicle = wrap(qt.vehicle + (p.verb == MenuVerb::Right || p.verb == MenuVerb::Down ? 1 : -1), n);
        show_vehicle(app, q);
        break;
    case MenuVerb::None: break;
    }
    refresh(app);
    return true;
}

void SplitCarScreen::refresh(App& app) {
    if (!document_) return;
    for (int q = 0; q < kMaxLocalPlayers; ++q) {
        Rml::Element* box = document_->GetElementById("q" + std::to_string(q));
        if (!box) continue;
        if (q >= players_) {
            box->SetInnerRML("");
            box->SetClass("ready", false);
            continue;
        }
        const Quarter& qt = quarters_[static_cast<size_t>(q)];
        const LocalSeat& s = app.locals().seats[static_cast<size_t>(q)];
        box->SetClass("ready", qt.ready);
        const std::string& car = level_.cars[static_cast<size_t>(qt.vehicle)];
        // `INF_<NAME>`: the machine's name, then the kind of machine it is (car_screen.cpp).
        const std::vector<std::string> info = text_.text_lines("INF_" + car);
        const std::string title = upper(info.empty() ? car : info[0]);
        // Only the name: the kind of machine ("CUSTOMIZED ALL-TERRAIN MILITARY PERSONNEL
        // CARRIER") took three lines of a quarter and pushed the rest out of it.
        std::string rml = "<div class=\"qhead\">" + upper(s.name) + " <span>" +
                          controls_name(s.controls) + "</span></div><div class=\"car wrap-ok\">";
        rml += picker_line(title, !qt.ready);
        rml += qt.ready ? "<div class=\"keys ok\">READY!</div>"
                        : "<div class=\"keys\">" + std::string(ok_key_name(s.controls)) +
                              ": THIS ONE</div>";
        rml += "</div>";
        box->SetInnerRML(rml);
    }
    if (Rml::Element* el = document_->GetElementById("hint")) {
        bool all = players_ > 0;
        for (int q = 0; q < players_; ++q) all = all && quarters_[static_cast<size_t>(q)].ready;
        el->SetInnerRML(all ? "EVERYONE HAS CHOSEN - OFF WE GO"
                            : "EACH PLAYER CHOOSES A VEHICLE - ESC: BACK");
    }
}

void SplitCarScreen::update(App& app, double dt_ms) {
    turn_ += 0.10f * static_cast<float>(dt_ms) / 1000.0f;
    turn_ -= std::floor(turn_);
    bool all = players_ > 0;
    for (int q = 0; q < players_; ++q) all = all && quarters_[static_cast<size_t>(q)].ready;
    // A short moment on "READY!" for the last one to choose, then the race.
    if (!all) countdown_ms_ = -1.0;
    else if (countdown_ms_ < 0.0) countdown_ms_ = 700.0;
    else if ((countdown_ms_ -= dt_ms) <= 0.0) {
        countdown_ms_ = -1.0;
        for (int q = 0; q < players_; ++q)
            app.locals().seats[static_cast<size_t>(q)].vehicle = quarters_[static_cast<size_t>(q)].vehicle;
        if (go_) *go_ = true;
        app.pop();
        return;
    }
    RmlUiScreen::update(app, dt_ms);
}

void SplitCarScreen::draw(App& app) {
    background_.draw();
    if (canvas_w_ > 0 && canvas_h_ > 0) {
        for (int q = 0; q < players_; ++q) {
            float x, y, w, h;
            quarter_box(q, canvas_w_, canvas_h_, 0.05f, 0.10f, 0.90f, 0.60f, x, y, w, h);
            const float t = turn_ + 0.25f * static_cast<float>(q);
            VehicleDisplay& d = quarters_[static_cast<size_t>(q)].display;
            app.presentation().draw_model(x, y, w, h, [&](int tw, int th, float mx, float my,
                                                          float mw, float mh) {
                d.draw(tw, th, mx, my, mw, mh, t - std::floor(t));
            });
        }
    }
    RmlUiScreen::draw(app);
}

} // namespace bigred
