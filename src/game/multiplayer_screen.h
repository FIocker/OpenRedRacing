// MULTIPLAYER, and the network half of it: the fork, "create or join", and the list of games
// found on the network.
//
//
// ⚠️ MULTIPLAYER IS A FORK, NOT A MODE (the user's decision, 2026-09-22): LOCAL is the split
// screen, NETWORK the LAN game. Local is not built and says so (a PlaceholderScreen); the
// network is.
#pragma once

#include <string>

#include "app/rml_screen.h"
#include "formats/localised_text.h"
#include "ui/scn_kit.h"
#include "game/lobby_screen.h"
#include "net/discovery.h"

namespace bigred {

struct NetLook;
NetLook look_from_profile(App& app);

class MultiplayerScreen : public RmlUiScreen {
public:
    bool enter(App& app) override;
    const char* name() const override { return "multiplayer"; }

protected:
    const char* document_source() const override;
    const char* context_name() const override { return "multiplayer"; }
    void on_ready(App& app) override;
    void on_activate(App& app, const std::string& id) override;
private:
    // NETSCN - the original's multiplayer screen - and the player's language.
    ScnScreen scn_;
    TextTable text_;
    std::string doc_;
};

// NETWORK: create a game, or join one.
class NetworkScreen : public RmlUiScreen {
public:
    bool enter(App& app) override;
    const char* name() const override { return "network"; }

protected:
    const char* document_source() const override;
    const char* context_name() const override { return "network"; }
    void on_ready(App& app) override;
    void on_activate(App& app, const std::string& id) override;

private:
    // NETSCN - the original's multiplayer screen - and the player's language.
    ScnScreen scn_;
    TextTable text_;
    std::string doc_;
    void show_error(const std::string& text);
};

// JOIN: every game answering on the LAN, live, plus an address typed by hand for a network
// where the broadcast does not get through.
class JoinScreen : public RmlUiScreen {
public:
    JoinScreen() = default;
    // The automatic test (openbigred -NETJOIN): joins `address` at once, or the first open
    // game found, and hands `au` to the lobby. `loopback_only` keeps the search on this
    // machine, which never raises the firewall's question.
    JoinScreen(std::string address, LobbyScreen::Auto au, bool loopback_only = false)
        : auto_address_(std::move(address)), auto_(au), loopback_only_(loopback_only) {}

    bool enter(App& app) override;
    void exit(App& app) override;
    void resumed(App& app) override;
    bool handle_event(App& app, const SDL_Event& e) override;
    void update(App& app, double dt_ms) override;
    const char* name() const override { return "join"; }

protected:
    const char* document_source() const override;
    const char* context_name() const override { return "join"; }
    void on_ready(App& app) override;
    void on_activate(App& app, const std::string& id) override;

private:
    // NETSCN - the original's multiplayer screen - and the player's language.
    ScnScreen scn_;
    TextTable text_;
    std::string doc_;
    void rebuild();
    void refresh_address();
    void join(App& app, const std::string& address, uint16_t port);

    DiscoveryBrowser browser_;
    std::string shown_;      // what the list was last built from, to rebuild only on change
    std::string address_;    // typed by hand
    std::string error_;
    std::string auto_address_;
    LobbyScreen::Auto auto_;
    bool loopback_only_ = false;
    bool auto_done_ = false;
};

} // namespace bigred
