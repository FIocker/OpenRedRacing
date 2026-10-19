// THE BOARD: everything drawn over a race view - the placing and the mini-map, the lap and the
// clocks, the damage gauge, the speed and the boost, the banners, the name tags - plus the three
// overlays of the whole window (the race map of a three-way split, the quit question, the fade).
//
// Out of RaceScreen since 2026-09-28 (code review, item A2). It reads
// the race through a BoardFrame the screen fills in for each view, and nothing else: what the
// board shows is decided there, how it is laid out here. A new readout is a new field of the
// frame and a few lines of layout.
//
// Pixel art throughout: see render/hud.h. Coordinates are in pixels from the top-left corner of
// the view.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "formats/bigred_archive.h"
#include "formats/level_script.h"
#include "formats/sprite.h"
#include "render/hud.h"
#include "render/mat4.h"
#include "sim/race_session.h"

namespace bigred {

class LevelScene;

// A name over a car (network and split-screen races): where, what, and how far away.
struct BoardTag {
    float x = 0, y = 0, z = 0;   // render space, just over the roof
    float distance = 0;          // from the camera, world units, for the fade
    std::string text;
    bool human = false;          // a person's name; a ghost is "IA", dimmer
};

// A racer's dot on the mini-map, in the order to draw them (the view's own car last).
struct BoardDot {
    int racer = 0;
    int glyph = 0;   // game_spr::kDot0 + colour
};

struct BoardFrame {
    int width = 0, height = 0;           // the view, pixels
    Mat4 mvp = Mat4::identity();         // the world's, for the name tags
    const RaceSession* session = nullptr;
    int player = 0;                      // the racer this view belongs to
    const VehicleTuning* tuning = nullptr;
    const NitroReserve* nitros = nullptr;
    bool show_minimap = true;
    std::vector<BoardDot> dots;
    std::vector<BoardTag> tags;
    std::string watching;                // "WATCHING ..." when the view follows someone else
    int results_in = -1;                 // whole seconds to the results, -1 = no count
    // One line under the board - a network race's state - and its colour. Empty: none.
    std::string status;
    float status_rgb[3] = {1.0f, 1.0f, 0.4f};
};

class RaceBoard {
public:
    // The game's own artwork: GAMESVGA.SPR (INGAME.WAP) for digits and banners, SML_ALPH.SPR
    // (ALWAYS.WAP) for free text, DEFAULT.COL, and the level's TOPMAP.SPR through its palette.
    // A missing file is not fatal - the race runs without a board.
    void build(const std::string& game_dir, const std::unordered_map<std::string, Blob>& ingame,
               const LevelScene& scene);
    bool ready() const { return hud_.ready(); }

    // One view's board.
    void draw(const BoardFrame& f);
    // Three players: the fourth corner is the race seen from above, every car on it.
    void draw_race_map(const RaceSession& session, const std::vector<BoardDot>& dots, int w, int h);
    // The question over everything, split views included: the whole target, dimmed.
    void draw_quit_confirm(int width, int height, bool network);
    // The whole target, black, at `fade` (0 = the picture, 1 = black).
    void draw_fade(int width, int height, float fade);

private:
    Hud hud_;
    // The level's `TOPMAP.SPR` projection - see formats/sprite.h. Invalid when the level ships
    // no mini-map (TEST1), and the board then simply has none.
    TopmapProjection topmap_;
};

} // namespace bigred
