// The split screen: two to four players at one machine.
//
//
// THE USER'S FLOW (2026-09-26), in two screens cut in four:
//
//   `SplitScreenScreen` is MULTIPLAYER > SPLIT SCREEN. The screen is in four from the start,
//   one quarter per player to come. A player joins by pressing something on THEIR controls -
//   a pad button, an arrow, W A S D - and the next free quarter becomes theirs: their driver
//   turning, and what they can change (driver, the driver's colour, the car's colour, the
//   emblem), then READY. Once at least two have joined and every one of them is ready, the
//   seats are written into App::locals() and the globe opens: player one picks the circuit as
//   in a single race, the others watching the same screen.
//
//   `SplitCarScreen` is the vehicle choice that follows, pushed by the presentation screen
//   (game/track_screen.h) in place of its own vehicle screen: the four quarters again, each with
//   its player's vehicle turning, in their colour and with their driver. Once everyone is
//   ready, the presentation screen starts the race.
//
// ⚠️ INPUT IS READ PER DEVICE HERE, NOT THROUGH THE MENU FOCUS: every other screen merges the
// keyboard and every pad into one cursor (app/rml_screen.h), which is right for one player and
// useless for four. `SeatInputs` says which controls an event came from; Escape alone stays
// the screen's own, and leaves it.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "app/local_players.h"
#include "app/rml_screen.h"
#include "formats/localised_text.h"
#include "formats/threedw.h"   // RGB
#include "game/level_catalog.h"
#include "game/race_screen.h"
#include "ui/background.h"
#include "ui/team_logos.h"
#include "ui/vehicle_display.h"

namespace bigred {

enum class MenuVerb { None, Up, Down, Left, Right, Ok, Back };

// One press on one player's controls. The keyboard's two halves are two players: the ARROWS
// with Enter and Backspace, W A S D (Z Q S D) with Space and Tab. A pad is its own player, its
// d-pad or left stick moving, its bottom button validating and its right one going back.
struct SeatPress {
    Controls who = Controls::Everything;
    MenuVerb verb = MenuVerb::None;
};

class SeatInputs {
public:
    // False when the event is not a press of anyone's controls.
    bool read(App& app, const SDL_Event& e, SeatPress& out);

private:
    // Edge-triggering of each pad's stick, the way RmlUiScreen does it for the one cursor.
    bool armed_[kMaxLocalPlayers][2][2] = {};
    bool armed_init_ = false;
};

// What validates and what goes back on these controls, as the quarter prints them.
const char* ok_key_name(Controls c);
const char* back_key_name(Controls c);

class SplitScreenScreen : public RmlUiScreen {
public:
    // `demo`: that many quarters already joined, one ready - for `openbigred --shots`.
    explicit SplitScreenScreen(int demo = 0) : demo_(demo) {}
    // Player one's RACES row: 0..2 = 1..3 chosen on the globe, 3..5 = 1..3 drawn at random.
    int races_option_ = 0;

    bool enter(App& app) override;
    void exit(App& app) override;
    void resumed(App& app) override;
    bool handle_event(App& app, const SDL_Event& e) override;
    void update(App& app, double dt_ms) override;
    void draw(App& app) override;
    const char* name() const override { return "split-screen"; }

protected:
    const char* document_source() const override;
    const char* context_name() const override { return "split-screen"; }
    void on_ready(App& app) override;

private:
    struct Quarter {
        bool taken = false;
        bool ready = false;
        int row = 0;          // which line of the sheet the player is on
        LocalSeat seat;
        VehicleDisplay display;
    };
    void press(App& app, const SeatPress& p);
    void refresh(App& app);
    void show_driver(App& app, int q);
    int joined() const;
    bool all_ready() const;

    std::array<Quarter, kMaxLocalPlayers> quarters_;
    SeatInputs inputs_;
    std::vector<RGB> driver_palette_;   // DRIVER.RAW's, which every driver is drawn through
    Background background_;
    TeamLogos logos_;
    float turn_ = 0.0f;
    double countdown_ms_ = -1.0;        // running once everyone is ready
    int demo_ = 0;
};

class SplitCarScreen : public RmlUiScreen {
public:
    // `go` points into the presentation screen underneath, like CarScreen's `selection`: set
    // once everyone has chosen, and that screen then starts the race. Null: choose, and only
    // come back.
    SplitCarScreen(LevelEntry level, bool* go) : level_(std::move(level)), go_(go) {}

    bool enter(App& app) override;
    void exit(App& app) override;
    bool handle_event(App& app, const SDL_Event& e) override;
    void update(App& app, double dt_ms) override;
    void draw(App& app) override;
    const char* name() const override { return "split-car"; }

protected:
    const char* document_source() const override;
    const char* context_name() const override { return "split-car"; }
    void on_ready(App& app) override;

private:
    struct Quarter {
        bool ready = false;
        int vehicle = 0;      // index into level_.cars
        VehicleDisplay display;
    };
    void refresh(App& app);
    void show_vehicle(App& app, int q);

    LevelEntry level_;
    bool* go_ = nullptr;
    int players_ = 0;
    std::array<Quarter, kMaxLocalPlayers> quarters_;
    SeatInputs inputs_;
    TextTable text_;
    std::vector<RGB> palette_;   // CARSEL.RAW's, as on the one-player vehicle screen
    Background background_;
    TeamLogos logos_;
    float turn_ = 0.0f;
    double countdown_ms_ = -1.0;
};

// The seats a split screen starts from: the first player's look out of the profile, the others
// as PLAYER 2..4; the arrows for the first, W A S D for the second, the pads for the rest.
LocalPlayers default_local_players(App& app, int players);

// Everyone's part of a split-screen race, out of App::locals(): the first player's colour,
// emblem and driver replace the profile's (a seat is changed on the split screen), and each
// guest drives the vehicle their seat chose - or, when none was chosen (the command line), the
// one after the first player's, and so on round the list. Does nothing when there is one player.
void fill_split_setup(App& app, const LevelEntry& level, int main_vehicle, RaceScreen::Setup& setup);

} // namespace bigred
