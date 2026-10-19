// THE LOBBY of a network game: who is there, what the race will be, who is ready.
//
//
// The same screen for the host and for a client, as specified: the players listed with their
// colour and emblem, the host first; ONLY THE HOST sets the race - the circuit, the laps, the
// ghosts - and presses START once everyone else is ready; the others can only get themselves
// ready. Then every machine goes to the vehicle screen (game/car_screen.h, the same one the
// single race uses) and the race starts on its own once everyone has chosen.
//
// ⚠️ THE LOBBY STAYS ON THE STACK UNDER EVERYTHING THAT FOLLOWS. The vehicle screen, the race
// and its results are pushed on top of it, and popping the results lands back here, ready for
// the next race. It reads the network game - owned by the App, which keeps it alive - and
// never holds a copy of its state: `NetGame::revision()` says when to redraw.
#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "app/rml_screen.h"
#include "ui/scn_kit.h"
#include "game/level_catalog.h"
#include "ui/background.h"
#include "ui/vehicle_display.h"

namespace bigred {

class LobbyScreen : public RmlUiScreen {
public:
    // TESTS WITHOUT A HAND ON THE KEYBOARD (openbigred's -NETHOST / -NETJOIN): a client makes
    // itself ready at once, the host starts as soon as `start_at` players are in and ready,
    // everyone takes the circuit's first vehicle without the vehicle screen, and the races
    // drive themselves.
    struct Auto {
        bool on = false;
        int start_at = 2;
        uint32_t quit_after = 0;
        uint32_t end_after = 0;   // the race ends by itself at this tick (RaceScreen::Setup)
        uint32_t desync_at = 0;   // RaceScreen::Setup::desync_at
        int races = 1;            // how many races before the instance quits
    };
    LobbyScreen() = default;
    explicit LobbyScreen(Auto a) : auto_(a) {}

    bool enter(App& app) override;
    void exit(App& app) override;
    void resumed(App& app) override;
    void update(App& app, double dt_ms) override;
    void draw(App& app) override;
    const char* name() const override { return "lobby"; }

protected:
    const char* document_source() const override;
    const char* context_name() const override { return "lobby"; }
    void on_ready(App& app) override;
    void on_activate(App& app, const std::string& id) override;
    void on_back(App& app) override;   // leaving the lobby leaves the game

private:
    // CHOOSCN's CONFIG4B.RAW: the palette of the wibbly title.
    ScnScreen scn_;
    void rebuild(App& app);
    const LevelEntry* level_entry(const std::string& name) const;
    void push_race(App& app);
    void change_level(App& app, int delta);
    // Host, "CIRCUIT: RANDOM": a circuit drawn among those that can start everyone in the
    // lobby, other than the one just raced when there is a choice. Drawn on the host's machine
    // alone and sent like any other setting, so every machine races the same one.
    void draw_random_level(App& app);
    // The circuit's mini-map as an image the document can name, registered the first time it
    // is asked for. Empty when the level has no TOPMAP.SPR or cannot be read.
    std::string map_image(App& app, const LevelEntry& level);
    // The circuit's vehicles, small and turning beside its mini-map.
    void show_cars(App& app, const LevelEntry& level);

    std::vector<LevelEntry> levels_;   // the circuits a race can be run on
    Background background_;            // CONFIG4B.RAW, drawn under the vehicles
    std::vector<std::unique_ptr<VehicleDisplay>> cars_;
    std::string cars_level_;           // the circuit cars_ were built for
    float car_turn_ = 0.0f;            // in turns
    std::set<std::string> maps_tried_; // levels whose mini-map map_image() has looked for
    std::set<std::string> maps_ok_;    // ...and those it found
    std::string address_;              // this machine, for the others to type
    uint32_t shown_revision_ = 0;
    bool shown_once_ = false;
    int vehicle_sel_ = -1;             // written by the vehicle screen on OK
    bool car_screen_open_ = false;
    bool car_screen_offered_ = false;  // pushed once per vehicle phase, then a button
    bool in_race_ = false;
    Auto auto_;
    bool auto_ready_sent_ = false;
    int races_done_ = 0;
};

} // namespace bigred
