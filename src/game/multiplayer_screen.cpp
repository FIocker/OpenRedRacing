#include "multiplayer_screen.h"

#include <RmlUi/Core.h>

#include <SDL.h>

#include <cctype>
#include <cstdlib>
#include <memory>

#include "app/app.h"
#include "game/level_catalog.h"
#include "game/lobby_screen.h"
#include "game/placeholder_screen.h"
#include "game/split_screen.h"
#include "net/net_game.h"
#include "ui/team_logos.h"

namespace bigred {
namespace {

// NETSCN, the original's own multiplayer screen: its picture, and its choices in the wibbly
// lettering on its boxes - `mbox 1, 10,20,250,24` (Split Screen), `mbox 2, 10,50,...` (Network
// Play), `mbox 4, 10,150,310,24` (the players count, which this game does not ask for: BACK
// takes that box, in CHOOSE_BACK's words). What is READ - the list of games, the address
// being typed, an error - stays in the steady lettering, as the user wants (2026-09-27).
constexpr const char* kStyle = R"(
#panel { position: absolute; left: 8%; top: 18%; width: 84%; }
.entry {
    display: block; font-size: 24dp; color: #ffffff; background: #00000080;
    padding: 8dp 16dp; margin-bottom: 12dp; width: 60%; white-space: nowrap;
}
.entry:hover, .entry:focus { color: #ffdf40; }
/* At most six games shown: the list must never run under the JOIN row at 120. */
#games { max-height: 190dp; overflow: hidden; }
.game { font-size: 16dp; width: 90%; }
.game.busy { color: #808080; }
.label { font-size: 16dp; color: #a0a0c0; margin-top: 16dp; margin-bottom: 8dp; }
.field { font-size: 24dp; background: #00000080; padding: 4dp 12dp; width: 50%; margin-bottom: 12dp; }
.error { position: absolute; left: 8%; top: 90%; width: 84%; font-size: 16dp; color: #ff8080; }
)";

// A choice on one of NETSCN's full-width boxes: its box, and the words in it.
std::string net_row(const ScnScreen& scn, const std::string& id, int y, const std::string& words,
                    int x, bool phase_b) {
    return ScnScreen::box_open(id, 10, y, 310, 24) +
           scn.words_rml(words, x, y, -1, 10, y, phase_b ? "b" : "") + "</div>";
}

std::string back_words(const TextTable& text) {
    std::string w = text.text("CHOOSE_BACK", "BACK");
    for (char& c : w) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return w;
}

// RML is XML: a player's name goes through this before it goes into a document.
std::string escape(const std::string& s) {
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

} // namespace

NetLook look_from_profile(App& app) {
    const PlayerProfile& p = app.profile();
    NetLook l;
    l.name = p.pseudo.empty() ? std::string("PLAYER") : p.pseudo;
    l.colour = static_cast<uint8_t>(p.vehicle_color < 0 ? 0 : p.vehicle_color);
    l.logo = static_cast<uint8_t>(p.logo_index < 0 ? 0 : p.logo_index);
    // The player's own: its bytes go with it. Chosen but gone (the file was removed since),
    // it is the first of the game's.
    if (l.logo >= kTeamLogoCount) {
        l.custom_logo = custom_logo_png();
        if (l.custom_logo.empty()) l.logo = 0;
    }
    l.damage = static_cast<uint8_t>(p.damage_option < 0 ? 0 : (p.damage_option > 100 ? 100 : p.damage_option));
    l.character = static_cast<uint8_t>(p.character < 0 ? 0 : p.character % 6);
    l.character_colour = static_cast<uint8_t>(p.character_color < 0 ? 0 : p.character_color);
    return l;
}

// --- the fork ---------------------------------------------------------------------------------

bool MultiplayerScreen::enter(App& app) {
    scn_.load(app, "NETSCN");
    text_.load(app.game_dir(), config_language(app.game_dir()));
    doc_ = scn_.document_open(kStyle) +
           ScnScreen::box_open("local", 10, 20, 250, 24) + scn_.text_rml(text_, "NET_SPLIT", 10, 20) +
           "</div>" +
           ScnScreen::box_open("network", 10, 50, 250, 24) +
           scn_.text_rml(text_, "NET_NETWORK", 10, 50, "b") + "</div>" +
           net_row(scn_, "back", 150, back_words(text_), 48, false) + "</body></rml>";
    return RmlUiScreen::enter(app);
}

const char* MultiplayerScreen::document_source() const { return doc_.c_str(); }

void MultiplayerScreen::on_ready(App&) { set_focus_order({"local", "network", "back"}); }

void MultiplayerScreen::on_activate(App& app, const std::string& id) {
    if (id == "network") { app.push(std::make_unique<NetworkScreen>()); return; }
    if (id == "local") {
        app.push(std::make_unique<SplitScreenScreen>());
        return;
    }
    if (id == "back") app.pop();
}

// --- create or join ---------------------------------------------------------------------------

bool NetworkScreen::enter(App& app) {
    scn_.load(app, "NETSCN");
    text_.load(app.game_dir(), config_language(app.game_dir()));
    doc_ = scn_.document_open(kStyle) + net_row(scn_, "create", 20, "CREATE A GAME", 30, false) +
           net_row(scn_, "join", 50, "JOIN A GAME", 38, true) +
           net_row(scn_, "back", 150, back_words(text_), 48, false) +
           "<div class=\"error\" id=\"error\"></div></body></rml>";
    return RmlUiScreen::enter(app);
}

const char* NetworkScreen::document_source() const { return doc_.c_str(); }

void NetworkScreen::on_ready(App&) { set_focus_order({"create", "join", "back"}); }

void NetworkScreen::show_error(const std::string& text) {
    if (!document_) return;
    if (Rml::Element* e = document_->GetElementById("error")) e->SetInnerRML(escape(text));
}

void NetworkScreen::on_activate(App& app, const std::string& id) {
    if (id == "create") {
        // The first circuit of the catalogue until the host picks another in the lobby.
        NetSettings s;
        const std::vector<LevelEntry> levels = scan_levels(app.game_dir());
        for (const LevelEntry& l : levels)
            if (!l.cars.empty()) {
                s.level = l.name;
                break;
            }
        if (!app.net().host(look_from_profile(app), s)) {
            show_error(app.net().error());
            return;
        }
        // The host's speed class is the game's: it travels in every start message.
        RaceRules rules = app.net().rules();
        rules.speed_percent = speed_percent_of(app.profile());
        app.net().set_rules(rules);
        app.push(std::make_unique<LobbyScreen>());
        return;
    }
    if (id == "join") { app.push(std::make_unique<JoinScreen>()); return; }
    if (id == "back") app.pop();
}

// --- the list of games ------------------------------------------------------------------------

const char* JoinScreen::document_source() const { return doc_.c_str(); }

bool JoinScreen::enter(App& app) {
    scn_.load(app, "NETSCN");
    text_.load(app.game_dir(), config_language(app.game_dir()));
    // The list and the address are read: steady, in a panel under the wibbly title. The two
    // ways out are NETSCN rows, at its 120 and 150.
    doc_ = scn_.document_open(kStyle) + scn_.words_rml("JOIN A GAME", 38, 3) +
           "<div id=\"panel\"><div class=\"label\">GAMES ON THE NETWORK</div><div id=\"games\"></div>"
           "<div class=\"label\">OR TYPE THE HOST'S ADDRESS</div>"
           "<div class=\"field\" id=\"address\"></div></div>" +
           net_row(scn_, "join_address", 120, "JOIN THIS ADDRESS", 30, true) +
           net_row(scn_, "back", 150, back_words(text_), 48, false) +
           "<div class=\"error\" id=\"error\"></div></body></rml>";
    if (!RmlUiScreen::enter(app)) return false;
    if (!browser_.open(loopback_only_)) error_ = "THE NETWORK CANNOT BE SEARCHED";
    SDL_StartTextInput();
    return true;
}

// Back from a lobby. When the game ended under this player (the host left, the connection
// dropped) the lobby popped itself, and the reason is shown here, above the list to pick
// another game from.
void JoinScreen::resumed(App& app) {
    RmlUiScreen::resumed(app);
    error_ = app.has_net() ? app.net().error() : std::string();
    refresh_address();
}

void JoinScreen::exit(App& app) {
    SDL_StopTextInput();
    browser_.close();
    RmlUiScreen::exit(app);
}

void JoinScreen::on_ready(App&) {
    rebuild();
    refresh_address();
}

void JoinScreen::refresh_address() {
    if (!document_) return;
    if (Rml::Element* e = document_->GetElementById("address"))
        e->SetInnerRML(address_.empty() ? std::string("...") : escape(address_) + "_");
    if (Rml::Element* e = document_->GetElementById("error")) e->SetInnerRML(escape(error_));
}

void JoinScreen::rebuild() {
    if (!document_) return;
    std::string rml;
    std::vector<std::string> ids;
    const std::vector<GameAnnounce>& games = browser_.games();
    for (size_t i = 0; i < games.size(); ++i) {
        const GameAnnounce& g = games[i];
        const bool open = g.phase == NetPhase::Lobby && g.players < g.max_players;
        const std::string id = "game" + std::to_string(i);
        rml += "<div class=\"entry game" + std::string(open ? "" : " busy") + "\" id=\"" + id +
               "\">" + escape(g.host_name) + " - " + escape(g.level) + " - " +
               std::to_string(g.players) + "/" + std::to_string(g.max_players) +
               (g.phase == NetPhase::Lobby ? (open ? "" : " FULL") : " RACING") + "</div>";
        ids.push_back(id);
    }
    if (games.empty()) rml = "<div class=\"game\">SEARCHING...</div>";
    if (Rml::Element* e = document_->GetElementById("games")) e->SetInnerRML(rml);
    ids.push_back("join_address");
    ids.push_back("back");
    set_focus_order(ids);
}

void JoinScreen::update(App& app, double dt_ms) {
    browser_.service(SDL_GetTicks());
    if (auto_.on && !auto_done_) {
        if (!auto_address_.empty()) {
            auto_done_ = true;
            join(app, auto_address_, kGamePort);
            return;
        }
        for (const GameAnnounce& g : browser_.games())
            if (g.phase == NetPhase::Lobby && g.players < g.max_players) {
                auto_done_ = true;
                join(app, g.address, g.game_port);
                return;
            }
    }
    // Rebuilt only when what it shows has changed: a rebuild resets the focus.
    std::string sig;
    for (const GameAnnounce& g : browser_.games())
        sig += g.address + ":" + std::to_string(g.game_port) + g.host_name + g.level +
               std::to_string(g.players) + std::to_string(static_cast<int>(g.phase)) + "|";
    if (sig != shown_) {
        shown_ = sig;
        rebuild();
    }
    RmlUiScreen::update(app, dt_ms);
}

bool JoinScreen::handle_event(App& app, const SDL_Event& e) {
    // The address field takes digits, dots and a colon - all an IPv4 address and a port need,
    // and none of them a key the menus use.
    if (e.type == SDL_TEXTINPUT) {
        for (const char* p = e.text.text; *p; ++p) {
            const char c = *p;
            if ((std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == ':') &&
                address_.size() < 21)
                address_ += c;
        }
        refresh_address();
        return true;
    }
    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_BACKSPACE) {
        if (!address_.empty()) address_.pop_back();
        refresh_address();
        return true;
    }
    return RmlUiScreen::handle_event(app, e);
}

void JoinScreen::join(App& app, const std::string& address, uint16_t port) {
    if (!app.net().join(address, port, look_from_profile(app))) {
        error_ = app.net().error();
        refresh_address();
        return;
    }
    app.push(std::make_unique<LobbyScreen>(auto_));
}

void JoinScreen::on_activate(App& app, const std::string& id) {
    if (id == "back") { app.pop(); return; }
    if (id == "join_address") {
        if (address_.empty()) {
            error_ = "TYPE AN ADDRESS FIRST";
            refresh_address();
            return;
        }
        std::string host = address_;
        uint16_t port = kGamePort;
        if (const size_t colon = host.find(':'); colon != std::string::npos) {
            port = static_cast<uint16_t>(std::atoi(host.c_str() + colon + 1));
            host.resize(colon);
        }
        join(app, host, port);
        return;
    }
    if (id.rfind("game", 0) == 0) {
        const size_t i = static_cast<size_t>(std::atoi(id.c_str() + 4));
        if (i >= browser_.games().size()) return;
        const GameAnnounce g = browser_.games()[i];
        if (g.phase != NetPhase::Lobby || g.players >= g.max_players) return;
        join(app, g.address, g.game_port);
    }
}

} // namespace bigred
