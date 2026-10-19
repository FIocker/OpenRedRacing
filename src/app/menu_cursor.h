// The game's own mouse pointer: `ALWAYS.SPR` glyphs 0-7, a red arrow that swells and shrinks,
// cycled like any animated widget of the original (the `mouse` widget of every `.SCN`). Shown over the menus, hidden over a race.
//
//
// ⚠️ ITS COLOUR IS DRAWN AT RANDOM WHEN THE GAME STARTS. The user remembers the original's
// pointer changing colour from one launch to the next (2026-09-27); the code read so far does
// not show where (the widget animation only turns the frames, and the arrow's body is palette 48/49 of
// the SCREEN's palette - which already differs from menu to menu). HYPOTHESE, kept because the
// user's description of the original is data: the body is repainted in one of the game's own
// colour presets, drawn once per launch; the outline (47) stays dark.
#pragma once

#include <SDL.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bigred {

class MenuCursor {
public:
    // Reads the eight frames and draws the colour. False without ALWAYS.WAP: the system
    // pointer then stays, which is not wrong.
    bool load(const std::string& game_dir);
    // Once a frame: the game's pointer over a menu (`menu`), none over a race. Sized to the
    // window: a whole-number magnification of the 32x32 artwork, window_h / 360.
    void update(bool menu, int window_h, uint32_t now_ms);
    void release();

private:
    void build(int scale);

    struct Frame {
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;   // already repainted
    };
    std::vector<Frame> frames_;
    std::vector<SDL_Cursor*> cursors_;
    int hot_x_ = 0, hot_y_ = 0;       // the arrow's tip, in artwork pixels
    int scale_ = 0;
    int shown_ = -1;                  // the frame SDL shows, -1 = the system's
    bool hidden_ = false;
};

} // namespace bigred
