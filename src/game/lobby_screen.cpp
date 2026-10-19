#include "lobby_screen.h"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>

#include "app/app.h"
#include "formats/game_files.h"
#include "formats/sprite.h"
#include "formats/texture.h"   // kRawPaletteBytes, load_palette_rgb
#include "formats/tourn.h"   // kFrontEndMusicTrack
#include "game/car_screen.h"
#include "game/multiplayer_screen.h"
#include "game/race_screen.h"
#include "net/discovery.h"
#include "net/net_game.h"
#include "ui/team_logos.h"

namespace bigred {
namespace {

// Where the circuit's vehicles turn, as fractions of the canvas: right of the mini-map, under
// the "VEHICLES" heading, left of the actions (#preview's own 4%..52% box, see the style).
constexpr float kCarsX = 0.23f, kCarsY = 0.70f, kCarsW = 0.30f, kCarsH = 0.26f;
// The vehicle screen's pace: one revolution in ten seconds.
constexpr float kCarTurnsPerSecond = 0.10f;

constexpr const char* kDocument = R"(<rml>
<head>
<style>
/* FOUR FIXED AREAS, sized for twelve players (2026-09-26): the players top left, the race
   settings top right, the circuit's preview bottom left, what this player can do bottom
   right. The preview used to follow the players down the page and the actions sat at a fixed
   70%: past six players the two met. */
/* The title is the original's lettering, placed by ui/scn_kit.h on the whole screen: its
   holder covers the canvas and lets the pointer through. Everything under it is read -
   players, settings, the address, the state - and stays in the steady lettering; only the
   actions wibble, at half the original's size so that they fit their column. */
#title { position: absolute; left: 0; top: 0; width: 100%; height: 100%; pointer-events: none; }
#players { position: absolute; left: 4%; top: 19%; width: 48%; }
#race { position: absolute; left: 55%; top: 19%; width: 41%; }
#preview { position: absolute; left: 4%; top: 66%; width: 48%; }
#actions { position: absolute; left: 55%; top: 68%; width: 41%; }
.head { font-size: 16dp; color: #a0a0c0; margin-bottom: 4dp; white-space: nowrap; }
.player {
    position: relative; font-size: 16dp; background: #00000080; padding: 2dp 6dp;
    margin-bottom: 4dp; white-space: nowrap;
}
.player.me .name { color: #ffdf40; }
.swatch { display: inline-block; width: 14dp; height: 14dp; margin-right: 8dp; }
/* THE STATE IS PINNED TO THE ROW'S RIGHT EDGE and the name is cut short rather than pushing it
   out: "NOT READY" takes a third of the row at 16dp, a sixteen-letter name the rest, and laid
   side by side the two ran out of the row into the settings (user, 2026-09-26). */
.player .name { display: inline-block; width: 58%; overflow: hidden; vertical-align: top; }
.player.series .name { width: 40%; }
.player .pts { position: absolute; right: 37%; top: 2dp; color: #c0c0c0; }
.state { position: absolute; right: 6dp; top: 2dp; color: #a0ffa0; }
.state.no { color: #ff9090; }
.setting { font-size: 16dp; margin-bottom: 10dp; white-space: nowrap; }
.setting .value { display: inline-block; width: 70%; text-align: center; background: #00000080; }
.arrow { display: inline-block; width: 15%; text-align: center; background: #00000080; }
.arrow:hover, .arrow:focus { color: #ffdf40; }
.entry {
    display: block; font-family: wibs; font-size: 58dp; color: #ffffff;
    margin-bottom: 4dp; white-space: nowrap;
}
.entry.b { font-family: wibs-b; }
.entry:hover, .entry:focus { color: #ffdf40; }
.entry.off { color: #808080; }
#status { font-size: 16dp; color: #ffdf40; margin-top: 12dp; }
#status.error { color: #ff8080; }
/* The circuit: its mini-map, and the vehicles it allows turning beside it (user, 2026-09-25
   and 2026-09-26). The vehicles are drawn by draw(), not by the document, in the box
   kCarsX..kCarsY below the heading - written out as words, the kind of each machine ran under
   the actions on the right. */
#preview .map { display: inline-block; width: 36%; vertical-align: top; }
#preview .cars {
    display: inline-block; width: 58%; margin-left: 4%; vertical-align: top;
    font-size: 16dp; color: #a0c0ff; white-space: nowrap;
}
</style>
</head>
<body>
    <div id="title">NETWORK GAME</div>
    <div id="players"></div>
    <div id="race"></div>
    <div id="preview"></div>
    <div id="actions"></div>
</body>
</rml>)";

std::string escape_upper(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '&') out += "&amp;";
        else if (c == '"') out += "&quot;";
        else out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string hex_colour(int index) {
    const std::vector<ColorPreset>& presets = color_presets();
    if (presets.empty()) return "#ffffff";
    const ColorPreset& p = presets[static_cast<size_t>(index) % presets.size()];
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", p.r, p.g, p.b);
    return buf;
}

} // namespace

const char* LobbyScreen::document_source() const { return kDocument; }

bool LobbyScreen::enter(App& app) {
    for (LevelEntry& l : scan_levels(app.game_dir()))
        if (!l.cars.empty()) levels_.push_back(std::move(l));
    address_ = local_address();
    // The background is drawn by hand, not by the document: the vehicles land between the two.
    if (const Blob* raw = find_member(*game_archive_or_empty(app.game_dir(), "CHOOSCN.SCN"),
                                      "CONFIG4B.RAW"))
        background_.load(*raw);
    scn_.load(app, "CHOOSCN", "CONFIG4B.RAW");
    if (!RmlUiScreen::enter(app)) return false;
    NetGame& net = app.net();
    // The host announces the circuit's vehicles now: it is the one that turns everyone's
    // choice into a vehicle name when the race starts.
    if (net.is_host())
        if (const LevelEntry* e = level_entry(net.lobby().settings.level))
            net.set_settings(net.lobby().settings, e->cars);
    return true;
}

void LobbyScreen::exit(App& app) {
    // Popped: the player left, one way or another. Leaving the lobby is leaving the game.
    if (app.has_net()) app.net().leave();
    cars_.clear();
    background_.release();
    RmlUiScreen::exit(app);
}

void LobbyScreen::on_ready(App& app) { rebuild(app); }

void LobbyScreen::on_back(App& app) {
    app.net().leave();
    app.pop();
}

// The circuit's vehicles as the race will draw them, through the level's own `MAP.COL`, in
// this player's colour. Rebuilt only when the circuit changes: a lobby redraws on every
// network message.
void LobbyScreen::show_cars(App& app, const LevelEntry& level) {
    if (level.name == cars_level_) return;
    cars_level_ = level.name;
    cars_.clear();
    std::vector<RGB> palette;
    try {
        const ArchivePtr lev = level_files(app.game_dir(), level.name);
        if (const auto col = lev->find("MAP.COL"); col != lev->end())
            palette = load_palette_rgb(col->second.data(), col->second.size());
    } catch (const std::exception&) {
    }
    if (palette.empty()) return;
    VehicleTint tint{};
    const VehicleTint* t = nullptr;
    if (const std::vector<ColorPreset>& presets = color_presets(); !presets.empty()) {
        const ColorPreset& p =
            presets[static_cast<size_t>(app.profile().vehicle_color) % presets.size()];
        tint = VehicleTint{p.r, p.g, p.b};
        t = &tint;
    }
    for (const std::string& car : level.cars) {
        auto d = std::make_unique<VehicleDisplay>();
        if (d->load(app.game_dir(), car, palette, t)) cars_.push_back(std::move(d));
    }
}

void LobbyScreen::draw(App& app) {
    // The artwork, the vehicles, then the document - RmlUi paints last (car_screen.cpp).
    background_.draw();
    const int n = static_cast<int>(cars_.size());
    if (n > 0 && canvas_w_ > 0 && canvas_h_ > 0) {
        const float cw = static_cast<float>(canvas_w_), ch = static_cast<float>(canvas_h_);
        const float bw = kCarsW * cw, bh = kCarsH * ch;
        // As many columns as give each vehicle the largest square.
        int cols = 1;
        float best = 0.0f;
        for (int c = 1; c <= n; ++c) {
            const int rows = (n + c - 1) / c;
            const float side = std::min(bw / static_cast<float>(c), bh / static_cast<float>(rows));
            if (side > best) { best = side; cols = c; }
        }
        const int rows = (n + cols - 1) / cols;
        const float cell_w = bw / static_cast<float>(cols), cell_h = bh / static_cast<float>(rows);
        for (int i = 0; i < n; ++i) {
            // Each a little behind the one before, so they never all show the same side.
            const float turn = car_turn_ + static_cast<float>(i) / static_cast<float>(n);
            VehicleDisplay& car = *cars_[static_cast<size_t>(i)];
            const float spin = turn - std::floor(turn);
            app.presentation().draw_model(
                kCarsX * cw + static_cast<float>(i % cols) * cell_w,
                kCarsY * ch + static_cast<float>(i / cols) * cell_h, cell_w, cell_h,
                [&](int tw, int th, float x, float y, float w, float h) {
                    car.draw(tw, th, x, y, w, h, spin);
                });
        }
    }
    RmlUiScreen::draw(app);
}

// `TOPMAP.SPR`'s first glyph through the level's own `MAP.COL` - the palette the race's board
// draws the same map with (RaceScreen::build_hud). A .LEV is a quarter of a megabyte, so the
// archive is read once per circuit shown and never kept.
std::string LobbyScreen::map_image(App& app, const LevelEntry& level) {
    const std::string name = "lobby:map:" + level.name;
    if (maps_tried_.insert(level.name).second) {
        try {
            const ArchivePtr lev = level_files(app.game_dir(), level.name);
            const auto tm = lev->find("TOPMAP.SPR");
            const auto col = lev->find("MAP.COL");
            if (tm != lev->end() && col != lev->end()) {
                const std::vector<SpriteGlyph> g = decode_spr(tm->second.data(), tm->second.size());
                const std::vector<RGB> pal = load_palette_rgb(col->second.data(), col->second.size());
                if (!g.empty() && g[0].valid()) {
                    std::vector<uint8_t> rgba = sprite_glyph_rgba(g[0], pal);
                    if (!rgba.empty()) {
                        app.ui().renderer().register_image(name, std::move(rgba), g[0].width,
                                                           g[0].height);
                        maps_ok_.insert(level.name);
                    }
                }
            }
        } catch (const std::exception&) {
        }
    }
    return maps_ok_.count(level.name) ? name : std::string();
}

const LevelEntry* LobbyScreen::level_entry(const std::string& name) const {
    for (const LevelEntry& l : levels_)
        if (l.name == name) return &l;
    return nullptr;
}

void LobbyScreen::resumed(App& app) {
    app.music().play_cd_track(kFrontEndMusicTrack);
    NetGame& net = app.net();
    if (car_screen_open_) {
        car_screen_open_ = false;
        // The vehicle screen also sets the colour and the emblem: announce them with it.
        net.set_look(look_from_profile(app));
        if (vehicle_sel_ >= 0) net.set_vehicle(static_cast<int16_t>(vehicle_sel_));
    }
    if (in_race_) {
        in_race_ = false;
        vehicle_sel_ = -1;
        car_screen_offered_ = false;
        // Back from the results: the host opens the lobby again for the next race.
        if (net.is_host()) {
            net.back_to_lobby();
            if (net.lobby().settings.random_circuit) draw_random_level(app);
        }
        if (auto_.on && ++races_done_ >= auto_.races) {
            std::printf("[autotest] %d race(s) done, leaving\n", races_done_);
            std::fflush(stdout);
            app.quit();
        }
    }
    shown_once_ = false;
}

void LobbyScreen::change_level(App& app, int delta) {
    NetGame& net = app.net();
    if (levels_.empty()) return;
    int i = 0;
    for (size_t k = 0; k < levels_.size(); ++k)
        if (levels_[k].name == net.lobby().settings.level) i = static_cast<int>(k);
    const int n = static_cast<int>(levels_.size());
    i = ((i + delta) % n + n) % n;
    NetSettings s = net.lobby().settings;
    s.level = levels_[static_cast<size_t>(i)].name;
    // The circuit's grid decides how many can start (level_catalog.h, max_racers).
    const int cap = levels_[static_cast<size_t>(i)].max_racers;
    if (s.max_players > cap) s.max_players = static_cast<uint8_t>(cap);
    net.set_settings(s, levels_[static_cast<size_t>(i)].cars);
}

void LobbyScreen::draw_random_level(App& app) {
    NetGame& net = app.net();
    const int players = static_cast<int>(net.lobby().players.size());
    std::vector<int> pool;
    for (size_t k = 0; k < levels_.size(); ++k)
        if (levels_[k].max_racers >= players && levels_[k].name != net.lobby().settings.level)
            pool.push_back(static_cast<int>(k));
    if (pool.empty()) return;
    std::random_device rd;
    const LevelEntry& l = levels_[static_cast<size_t>(pool[rd() % pool.size()])];
    NetSettings s = net.lobby().settings;
    s.level = l.name;
    if (s.max_players > l.max_racers) s.max_players = static_cast<uint8_t>(l.max_racers);
    net.set_settings(s, l.cars);
}

void LobbyScreen::push_race(App& app) {
    NetGame& net = app.net();
    const NetRaceStart& start = net.race();
    RaceScreen::Setup setup;
    setup.network = true;
    // Asked for here, not left to the START: the race screen starts the circuit's music
    // before it builds anything, and it needs the name for that.
    setup.level = start.settings.level;
    const LevelEntry* e = level_entry(start.settings.level);
    setup.level_title = e ? e->title : start.settings.level;
    // What only this machine draws: its own colour and emblem, from the profile.
    if (const std::vector<ColorPreset>& presets = color_presets(); !presets.empty()) {
        const ColorPreset& p =
            presets[static_cast<size_t>(app.profile().vehicle_color) % presets.size()];
        setup.tinted = true;
        setup.tint = VehicleTint{p.r, p.g, p.b};
    }
    setup.character = app.profile().character;
    setup.character_colour = app.profile().character_color;
    TeamLogos logos;
    if (logos.load(app.game_dir())) {
        int lw = 0, lh = 0;
        std::vector<uint8_t> emblem = logos.logo_rgba(app.profile().logo_index, lw, lh);
        if (lw == kLogoCell && lh == kLogoCell) setup.logo = std::move(emblem);
    }
    setup.autodrive = auto_.on;
    setup.quit_after = auto_.quit_after;
    setup.end_after = auto_.end_after;
    setup.desync_at = auto_.desync_at;
    in_race_ = true;
    app.push(std::make_unique<RaceScreen>(std::move(setup)));
}

void LobbyScreen::update(App& app, double dt_ms) {
    NetGame& net = app.net();
    // THE GAME IS GONE UNDER A CLIENT - the host left, or never answered: back to the list of
    // games, which says why (JoinScreen::resumed). A lobby nobody hosts could only be left,
    // and it made the player do it by hand (user, 2026-09-26).
    if (!net.active() && !auto_.on) {
        app.pop();
        return;
    }
    if (net.active() && auto_.on && net.joined()) {
        const NetPlayer* me = net.me();
        const NetPhase phase = net.lobby().phase;
        if (phase == NetPhase::Lobby) {
            if (net.is_host()) {
                if (static_cast<int>(net.lobby().players.size()) >= auto_.start_at &&
                    net.everyone_ready())
                    net.begin_vehicle_choice();
            } else if (me && !me->ready && !auto_ready_sent_) {
                auto_ready_sent_ = true;
                net.set_ready(true);
            }
        } else {
            auto_ready_sent_ = false;
        }
        if (net.lobby().phase == NetPhase::ChoosingVehicles && me && me->vehicle < 0 &&
            !car_screen_offered_) {
            car_screen_offered_ = true;
            net.set_vehicle(0);
        }
    }
    if (net.active()) {
        const NetPlayer* me = net.me();
        // The vehicle screen opens by itself, once, when the phase begins; after that there is
        // a button, for a player who backed out of it.
        // ⚠️ NEVER in an automatic lobby. The host's own begin_vehicle_choice() changes the
        // phase in the middle of THIS update, after the automatic block above has read it, so
        // this block used to see the new phase first and open the vehicle screen - which
        // nothing automatic ever closes: the host sat on it for ever.
        if (!auto_.on && net.lobby().phase == NetPhase::ChoosingVehicles && me && me->vehicle < 0 &&
            !car_screen_offered_ && net.joined()) {
            if (const LevelEntry* e = level_entry(net.lobby().settings.level)) {
                car_screen_offered_ = true;
                car_screen_open_ = true;
                app.push(std::make_unique<CarScreen>(*e, &vehicle_sel_));
                return;
            }
        }
        if (net.lobby().phase == NetPhase::Lobby) car_screen_offered_ = false;
        if (net.pending_start()) {
            push_race(app);
            return;
        }
    }
    if (!shown_once_ || net.revision() != shown_revision_) rebuild(app);
    // A clock, never a frame count: the vehicles turn at the same pace at any frame rate.
    car_turn_ += kCarTurnsPerSecond * static_cast<float>(dt_ms) / 1000.0f;
    car_turn_ -= std::floor(car_turn_);
    RmlUiScreen::update(app, dt_ms);
}

void LobbyScreen::rebuild(App& app) {
    if (!document_) return;
    NetGame& net = app.net();
    shown_revision_ = net.revision();
    shown_once_ = true;
    const NetLobbyState& lobby = net.lobby();
    const bool host = net.is_host();
    const bool lobby_phase = lobby.phase == NetPhase::Lobby;
    const LevelEntry* level = level_entry(lobby.settings.level);

    // --- who is there --------------------------------------------------------------------
    std::string players = "<div class=\"head\">PLAYERS " + std::to_string(lobby.players.size()) +
                          " / " + std::to_string(lobby.settings.max_players) + "</div>";
    for (size_t i = 0; i < lobby.players.size(); ++i) {
        const NetPlayer& p = lobby.players[i];
        std::string state;
        bool good = true;
        if (lobby.phase == NetPhase::Racing) state = "RACING";
        else if (lobby.phase == NetPhase::ChoosingVehicles) {
            if (p.vehicle >= 0 && level && static_cast<size_t>(p.vehicle) < level->cars.size())
                state = level->cars[static_cast<size_t>(p.vehicle)];
            else {
                state = "CHOOSING";
                good = false;
            }
        } else if (p.id == 0) state = "HOST";
        else {
            state = p.ready ? "READY" : "NOT READY";
            good = p.ready;
        }
        const bool series = lobby.settings.races > 1 || lobby.series_done > 0;
        // This player's own row has its name in yellow - "(YOU)" after it no longer fitted
        // beside a sixteen-letter name, the points and the state.
        players += "<div class=\"player" + std::string(p.id == net.local_id() ? " me" : "") +
                   (series ? " series" : "") +
                   "\"><span class=\"swatch\" style=\"background: " + hex_colour(p.look.colour) +
                   ";\"></span><span class=\"name\">" + escape_upper(p.look.name) +
                   "</span><span class=\"pts\">" +
                   (series ? std::to_string(p.points) + " PT" : std::string()) +
                   "</span><span class=\"state" + (good ? "" : " no") + "\">" +
                   escape_upper(state) + "</span></div>";
    }
    // The circuit being set up: its mini-map, and what can be driven on it - each vehicle by
    // the kind of machine it is, the second line of its `INF_<NAME>` (the file name only when
    // the tables say nothing, as on the vehicle screen).
    std::string preview;
    if (level) {
        const std::string img = map_image(app, *level);
        if (!img.empty()) preview += "<img class=\"map\" src=\"" + img + "\"/>";
        preview += "<div class=\"cars\"><div class=\"head\">VEHICLES</div></div>";
        show_cars(app, *level);
    }
    if (Rml::Element* el = document_->GetElementById("players")) el->SetInnerRML(players);
    if (Rml::Element* el = document_->GetElementById("preview")) el->SetInnerRML(preview);

    // --- the race: the host sets it, the others read it ------------------------------------
    const bool edit = host && lobby_phase;
    auto row = [&](const std::string& label, const std::string& value, const std::string& id) {
        std::string r = "<div class=\"head\">" + label + "</div><div class=\"setting\">";
        if (edit) r += "<span class=\"arrow\" id=\"" + id + "_prev\">&lt;</span>";
        r += "<span class=\"value\">" + escape_upper(value) + "</span>";
        if (edit) r += "<span class=\"arrow\" id=\"" + id + "_next\">&gt;</span>";
        return r + "</div>";
    };
    std::string race = row("CIRCUIT", level ? level->title : lobby.settings.level, "level");
    race += row("LAPS", lobby.settings.laps == 0 ? std::string("AS THE CIRCUIT")
                                                 : std::to_string(lobby.settings.laps),
                "laps");
    race += row("CIRCUIT CHOICE", lobby.settings.random_circuit ? "RANDOM" : "BY THE HOST", "random");
    race += row("RACES IN THE SERIES", std::to_string(lobby.settings.races), "races");
    race += row("RECORDED OPPONENTS", lobby.settings.ghosts ? "ON" : "OFF", "ghosts");
    race += row("PLAYERS AT MOST", std::to_string(lobby.settings.max_players) + " (CIRCUIT " +
                                       std::to_string(level ? level->max_racers : 6) + ")",
                "max");
    if (host)
        race += "<div class=\"head\">YOUR ADDRESS</div><div class=\"setting\">" +
                (address_.empty() ? std::string("UNKNOWN") : address_) +
                (net.port() != kGamePort ? ":" + std::to_string(net.port()) : std::string()) +
                "</div>";
    if (Rml::Element* el = document_->GetElementById("race")) el->SetInnerRML(race);

    // --- what this player can do ---------------------------------------------------------
    std::vector<std::string> ids;
    if (edit)
        ids = {"level_prev",  "level_next",  "laps_prev",   "laps_next",
               "random_prev", "random_next", "races_prev",  "races_next",
               "ghosts_prev", "ghosts_next", "max_prev",    "max_next"};
    std::string actions;
    std::string status;
    bool error = false;
    std::string main;
    if (!net.active()) {
        status = net.error().empty() ? "THE GAME IS OVER" : net.error();
        error = true;
    } else if (!net.joined()) {
        status = "CONNECTING...";
    } else if (lobby_phase) {
        const int cap = level ? level->max_racers : kNetDefaultPlayers;
        const bool too_many = static_cast<int>(lobby.players.size()) > cap;
        if (host) {
            const bool go = net.everyone_ready() && !too_many;
            actions += "<span class=\"entry" + std::string(go ? "" : " off") +
                       "\" id=\"start\">START</span>";
            ids.push_back("start");
            main = "start";
            status = too_many ? "THIS CIRCUIT STARTS " + std::to_string(cap) + " AT MOST"
                     : lobby.players.size() < 2 ? "WAITING FOR PLAYERS - YOU CAN START ALONE"
                     : go                     ? "EVERYONE IS READY"
                                              : "WAITING FOR THE PLAYERS TO BE READY";
        } else {
            const NetPlayer* me = net.me();
            const bool ready = me && me->ready;
            actions += "<span class=\"entry\" id=\"ready\">" +
                       std::string(ready ? "NOT READY" : "READY") + "</span>";
            ids.push_back("ready");
            main = "ready";
            status = ready ? "WAITING FOR THE HOST TO START" : "PRESS READY WHEN YOU ARE";
        }
    } else if (lobby.phase == NetPhase::ChoosingVehicles) {
        const NetPlayer* me = net.me();
        if (me && me->vehicle < 0) {
            actions += "<span class=\"entry\" id=\"vehicle\">VEHICLE</span>";
            ids.push_back("vehicle");
            main = "vehicle";
            status = "CHOOSE YOUR VEHICLE";
        } else {
            status = "WAITING FOR THE OTHERS TO CHOOSE";
        }
    } else {
        status = in_race_ ? "RACING" : "THE RACE IS ON - WAITING FOR THE NEXT ONE";
    }
    // The series, under everything else: which race is next, or who won it.
    if (net.active() && net.joined() && lobby_phase &&
        (lobby.settings.races > 1 || lobby.series_done > 0)) {
        std::string line;
        if (lobby.series_over()) {
            const NetPlayer* best = nullptr;
            for (const NetPlayer& p : lobby.players)
                if (!best || p.points > best->points) best = &p;
            line = "SERIES OVER - WON BY " + (best ? best->look.name : std::string("NOBODY")) +
                   " - START BEGINS A NEW ONE";
        } else {
            line = "RACE " + std::to_string(lobby.series_done + 1) + " OF " +
                   std::to_string(lobby.settings.races);
        }
        status = line + (status.empty() ? std::string() : " - " + status);
    }
    actions += "<span class=\"entry b\" id=\"leave\">LEAVE</span>";
    ids.push_back("leave");
    if (main.empty()) main = "leave";
    actions += "<div id=\"status\" class=\"wrap-ok" + std::string(error ? " error" : "") + "\">" +
               escape_upper(status) + "</div>";
    if (Rml::Element* el = document_->GetElementById("actions")) el->SetInnerRML(actions);
    if (Rml::Element* el = document_->GetElementById("title"))
        el->SetInnerRML(scn_.words_rml(host ? "NETWORK GAME - HOST" : "NETWORK GAME", 16, 3));
    // Keep the player where they were when the lobby changed under them; the main action
    // only when that element is gone.
    const std::string was = focused_id();
    const bool still_there = !was.empty() && std::find(ids.begin(), ids.end(), was) != ids.end();
    set_focus_order(ids, still_there ? was : main);
    if (still_there)
        if (Rml::Element* el = document_->GetElementById(was)) el->Focus(true);
}

void LobbyScreen::on_activate(App& app, const std::string& id) {
    NetGame& net = app.net();
    if (id == "leave") { on_back(app); return; }
    if (id == "ready") {
        const NetPlayer* me = net.me();
        net.set_ready(!(me && me->ready));
        return;
    }
    if (id == "start") {
        const LevelEntry* le = level_entry(net.lobby().settings.level);
        if (le && static_cast<int>(net.lobby().players.size()) > le->max_racers) return;
        net.begin_vehicle_choice();
        return;
    }
    if (id == "vehicle") {
        if (const LevelEntry* e = level_entry(net.lobby().settings.level)) {
            car_screen_open_ = true;
            app.push(std::make_unique<CarScreen>(*e, &vehicle_sel_));
        }
        return;
    }
    if (!net.is_host() || net.lobby().phase != NetPhase::Lobby) return;
    NetSettings s = net.lobby().settings;
    const LevelEntry* e = level_entry(s.level);
    const std::vector<std::string> cars = e ? e->cars : std::vector<std::string>();
    if (id == "level_prev") { change_level(app, -1); return; }
    if (id == "level_next") { change_level(app, 1); return; }
    if (id == "laps_prev") s.laps = static_cast<uint8_t>(s.laps == 0 ? 9 : s.laps - 1);
    else if (id == "laps_next") s.laps = static_cast<uint8_t>(s.laps >= 9 ? 0 : s.laps + 1);
    else if (id == "ghosts_prev" || id == "ghosts_next") s.ghosts = !s.ghosts;
    else if (id == "random_prev" || id == "random_next") {
        s.random_circuit = !s.random_circuit;
        net.set_settings(s, cars);
        if (s.random_circuit) draw_random_level(app);
        return;
    }
    else if (id == "races_prev" || id == "races_next") {
        // Not in the middle of a series: its length is what the points are counted against.
        const NetLobbyState& l = net.lobby();
        if (l.series_done > 0 && !l.series_over()) return;
        int n = s.races + (id == "races_next" ? 1 : -1);
        if (n > kNetMaxSeries) n = 1;
        if (n < 1) n = kNetMaxSeries;
        s.races = static_cast<uint8_t>(n);
    }
    else if (id == "max_prev" || id == "max_next") {
        // Never below the players already in: nobody is thrown out by a setting.
        const int lo = std::max<int>(2, static_cast<int>(net.lobby().players.size()));
        const int hi = std::max(lo, e ? e->max_racers : kNetDefaultPlayers);
        int m = s.max_players + (id == "max_next" ? 1 : -1);
        if (m > hi) m = lo;
        if (m < lo) m = hi;
        s.max_players = static_cast<uint8_t>(m);
    }
    else return;
    net.set_settings(s, cars);
}

} // namespace bigred
