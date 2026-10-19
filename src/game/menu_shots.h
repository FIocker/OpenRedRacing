// `openbigred --shots [out_dir] [game_dir]`: every menu screen, at several window sizes, saved
// as a PNG of its canvas, plus a report of what collides on it.
//
// ⚠️ WHY IT EXISTS: the menus were judged by the user's eye, one screenshot at a time, and a
// layout that fitted the window it was written on overflowed on the next one (the logo counter
// wrapping onto two lines once past 9, the rows at the bottom of a screen running into its hint
// line). This tool is the check that does not need anyone at the keyboard: it opens a hidden
// window, pushes each screen in turn, reads its canvas back, and walks its RmlUi document for
//   OVERLAP  two visible things (text lines, images, filled boxes) that are not nested and
//            cross each other;
//   OUTSIDE  something that leaves the canvas;
//   WRAP     a text that the layout broke over several lines - sometimes wanted (a circuit's
//            description), usually not (a label, a button, a counter).
// The PNGs are for the eye, the report is for a diff.
//
// It only drives screens that need no race and no network peer: the lobby is opened as a host
// on the loopback interface, so nobody on the LAN sees it.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "app/screen.h"

namespace bigred {

// A PNG of `rgba` (top row first, 4 bytes a pixel), stored uncompressed - a debugging aid.
bool write_png(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h);

class MenuShots : public Screen {
public:
    explicit MenuShots(std::string out_dir) : out_dir_(std::move(out_dir)) {}

    bool enter(App& app) override;
    void draw(App& app) override;
    const char* name() const override { return "menu-shots"; }

private:
    void on_frame(App& app);

    std::string out_dir_;
    size_t shot_ = 0, size_ = 0;
    int frames_ = -1;   // -1: the next screen has not been pushed yet
    int issues_ = 0;
    std::string report_;
};

} // namespace bigred
