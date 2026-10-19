// The display: what resolution the game renders at, how the menus are framed, and the `+`/`-`
// keys that walk between them.
//
//
// ⚠️ THE RESOLUTION LADDER IS A NOD TO THE ORIGINAL, and it is the user's request of
// 2026-09-22: Big Red Racing switched between 320x200 and 640x480 with `+` and `-`, and so does
// this. The third rung is the one the original could not offer - the window's own resolution -
// and it is the DEFAULT, because this project's standing decision is that HD is the default and
// the period look is an option the player turns on.
//
// The two display spaces take the ladder differently, and on purpose:
//
//   * THE MENUS are laid out at the real resolution of a centred 4:3 box, and their TEXT and
//     artwork ignore the ladder. Only the 3D MODELS they show - a vehicle, a driver, the globe
//     - are drawn at the rung's resolution (draw_model).
//     ⚠️ Until 2026-09-27 the rung was a filter over the whole finished menu, quantised to
//     320x200: the wibbly lettering survived it, every steady text became unreadable. The
//     user's rule: the resolution is for the game and the 3D, not for the menus. And laying a
//     menu out in 320 real pixels was tried on 2026-09-22 and abandoned the same day.
//   * THE WORLD really renders at that many lines, into an offscreen buffer stretched over the
//     window. There is nothing to preserve there - a modern mesh rasterised into 200 lines IS
//     what the original looked like - and the chunky result is the whole point.
//
// ⚠️ WHAT IS HELD IS THE LINE COUNT, not the pair of numbers. The width follows the window's
// own shape, so "320x200" on a 16:9 window is 356x200: the same pixel size as the original,
// with no distortion and no letterbox. The names are the original's because those are the
// numbers a player of it remembers.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "render/gl_loader.h"

namespace bigred {

// One rung of the ladder, from lowest to highest. `lines` is the vertical resolution;
// 0 means "the window's own", which is the last rung and the default.
struct DisplayMode {
    int lines;
    const char* name;
};

inline constexpr DisplayMode kDisplayModes[] = {
    {200, "320x200"},
    {480, "640x480"},
    {0, "native"},
};
inline constexpr int kDisplayModeCount = static_cast<int>(sizeof(kDisplayModes) /
                                                          sizeof(kDisplayModes[0]));
// HD is the default, menus included. See the header comment.
inline constexpr int kDefaultDisplayMode = kDisplayModeCount - 1;


class Presentation {
public:
    ~Presentation();

    bool init();
    bool ready() const { return program_ != 0; }
    // Frees every GL object, while the context is still current. ⚠️ The App used to do this
    // by assigning a fresh Presentation over itself, which copies zeroed handles in and leaks
    // the real ones - harmless only because the context died a line later.
    void release();

    // --- the resolution ladder ----------------------------------------------------------
    void set_mode(int index);
    // `delta` of +1 is the `+` key: one rung TOWARDS the window's own resolution. It stops at
    // both ends rather than wrapping - a player holding `-` expects to land on 320x200 and stay
    // there, not to be thrown back to HD.
    void step_mode(int delta);
    int mode() const { return mode_; }
    const char* mode_name() const { return kDisplayModes[mode_].name; }
    // The vertical resolution of the current rung, or 0 for the window's own.
    int lines() const { return kDisplayModes[mode_].lines; }

    // --- the menus ------------------------------------------------------------------------
    // Directs drawing into the offscreen canvas and clears it. The canvas is the size of the
    // 4:3 box in REAL pixels, so a screen lays out in real pixels - `canvas_width/height()`.
    // Reallocates when the box changes size.
    void begin_canvas(int window_w, int window_h);
    // Puts the canvas on screen in its 4:3 box, letterboxing the rest.
    void present(int window_w, int window_h);

    // A 3D model inside a menu, at the rung's resolution. `draw` renders it into a target it
    // is handed as (target_w, target_h, x, y, w, h) - exactly the arguments VehicleDisplay::draw
    // and Globe::draw already take - and leaves the viewport on the whole target. At the native
    // rung that is the canvas itself; below it, a small buffer of big square pixels, then laid
    // over the canvas rectangle (x, y, w, h) - canvas pixels, top-left origin.
    using ModelDraw = std::function<void(int target_w, int target_h, float x, float y, float w,
                                         float h)>;
    void draw_model(float x, float y, float w, float h, const ModelDraw& draw);

    int canvas_width() const { return canvas_.w; }
    int canvas_height() const { return canvas_.h; }
    // The canvas as it was last drawn, top row first, RGBA. For `openbigred --shots`.
    bool read_canvas(std::vector<uint8_t>& rgba, int& w, int& h) const;

    // --- the world ------------------------------------------------------------------------
    // Directs drawing into an offscreen buffer of the current rung's resolution, WITH a depth
    // buffer, covering the whole window's shape. Returns false at the native rung and when
    // anything failed, and the caller then draws straight to the window as before - which is
    // why neither of these can be fatal.
    bool begin_world(int window_w, int window_h);
    // Stretches that buffer over the whole window, nearest-neighbour. Only call it when
    // begin_world returned true.
    void present_world(int window_w, int window_h);

    struct Box {
        int x = 0, y = 0, w = 0, h = 0;
    };
    // Where the 4:3 box lands, for a given window. Exposed because input needs the same numbers
    // the blit used.
    static Box box_for(int window_w, int window_h);

    // Maps a window-space mouse position into canvas coordinates. Returns false in the
    // letterbox, where no widget can be: a caller must treat that as "over nothing" rather than
    // clamping, or the edge rows of buttons would swallow clicks from outside the picture.
    static bool to_canvas(int window_w, int window_h, int mx, int my, float& cx, float& cy);

private:
    // One offscreen buffer. Both carry depth - the menus' since a menu can show a 3D model
    // (see begin_canvas) - and they differ in their filter: the menus' samples linearly (the
    // filter takes one tap per cell, and a linear tap there is a cheap box filter), the world's
    // nearest, because there the big pixels ARE the effect.
    struct Target {
        GLuint fbo = 0, colour = 0, depth = 0;
        int w = 0, h = 0;
        bool ensure(int w, int h, bool want_depth, bool linear);
        void release();
    };

    Target canvas_;
    Target world_;
    Target model_;
    GLuint program_ = 0, vao_ = 0;
    GLint tex_loc_ = -1, uv_scale_loc_ = -1;
    int mode_ = kDefaultDisplayMode;
};

} // namespace bigred
