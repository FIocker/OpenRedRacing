// Where each player's view goes on a split screen. The user's layout (2026-09-26):
//
//   2 players   one on top, one below - two full-width bands; or, if player one chooses it
//               (user, 2026-09-30), side by side - two full-height columns;
//   3 players   three corners, and the FOURTH (bottom right) given to a large mini-map of the
//               whole race, filling almost all of it;
//   4 players   one per corner.
//
// Pure arithmetic in window pixels, top-left origin - the Hud's own convention - so it can be
// checked without a window (physics_bench's SPLIT block).
#pragma once

#include <vector>

namespace bigred {

struct SplitRect {
    int x = 0, y = 0, w = 0, h = 0;
};

struct SplitLayout {
    std::vector<SplitRect> views;   // one per player, player 1 first
    bool has_map = false;           // three players: the fourth corner is the map
    SplitRect map;
};

// `gap` is the black line between two views, in pixels.
inline SplitLayout split_layout(int players, int width, int height, int gap = 2,
                                bool side_by_side = false) {
    SplitLayout out;
    if (players <= 1) {
        out.views.push_back({0, 0, width, height});
        return out;
    }
    const int half_w = (width - gap) / 2, half_h = (height - gap) / 2;
    const int right_x = width - half_w, bottom_y = height - half_h;
    if (players == 2 && side_by_side) {
        out.views.push_back({0, 0, half_w, height});
        out.views.push_back({right_x, 0, half_w, height});
        return out;
    }
    if (players == 2) {
        out.views.push_back({0, 0, width, half_h});
        out.views.push_back({0, bottom_y, width, half_h});
        return out;
    }
    out.views.push_back({0, 0, half_w, half_h});             // top left
    out.views.push_back({right_x, 0, half_w, half_h});       // top right
    out.views.push_back({0, bottom_y, half_w, half_h});      // bottom left
    if (players >= 4) {
        out.views.push_back({right_x, bottom_y, half_w, half_h});
    } else {
        out.has_map = true;
        out.map = {right_x, bottom_y, half_w, half_h};
    }
    return out;
}

} // namespace bigred
