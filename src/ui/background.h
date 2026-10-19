// A menu screen's full-screen background - the `.SCN` scripts' `raw` keyword.
//
// 17 of the 19 screens have one; `TRAKSCN` and `CARSCN` do not, because they
// draw over the globe screen.
//
// ⚠️ A `.RAW` carries ITS OWN palette, in its first 768 bytes. It is not DEFAULT.COL and it
// differs from screen to screen, so a background cannot share the game's palette the way the
// HUD sprites do.
//
// The image is exactly 320x200, which is exactly the menu canvas (app/presentation.h), so it
// lands pixel-for-pixel with no resampling whatsoever. That is the whole reason the canvas is
// that size.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "render/gl_loader.h"

namespace bigred {

class Background {
public:
    ~Background();

    // Decodes and uploads a `.RAW` blob. Returns false on anything that does not add up - a
    // menu without its background is still usable, so this must never be fatal.
    bool load(const std::vector<uint8_t>& raw);
    bool ready() const { return texture_ != 0 && program_ != 0; }
    void release();

    // Covers the whole canvas. Call it first: it does not blend, it overwrites.
    void draw();

private:
    GLuint texture_ = 0, program_ = 0, vao_ = 0;
    GLint tex_loc_ = -1;
};

} // namespace bigred
