// The in-game HUD: the lap board, the clock, the placing, the damage gauge and the countdown.
//
// It draws with the GAME'S OWN artwork - the `GAMESVGA.SPR` set out of `INGAME.WAP` for the
// digits, the ordinals and the banners, and `SML_ALPH.SPR` out of `ALWAYS.WAP` for free text.
// See src/formats/sprite.h for the index table.
//
// ⚠️ EVERY GLYPH IS PIXEL ART AND IS DRAWN AS SUCH: the atlas is sampled with GL_NEAREST, the
// text scale is rounded to a whole number and every quad lands on a whole pixel. Bilinear
// filtering on an 8-pixel glyph blown up six times is a smear, which is exactly what the user
// reported of the first HUD ("les nouvelles ecritures sont illisibles").
//
// ⚠️ EVERYTHING HERE IS DISPLAY. Nothing the HUD reads may change what the simulation does, and
// nothing it draws may be counted in frames: the clock it shows comes from the race's own tick
// count.
//
// The whole set is packed into ONE texture at load time, so a frame of HUD is a single draw call
// whatever it says.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "formats/sprite.h"
#include "formats/threedw.h" // RGB
#include "gl_loader.h"

namespace bigred {

class Hud {
public:
    ~Hud();

    // Packs `sprites` (a `.SPR`) and `font` into one RGBA atlas and uploads it. Glyph `i` of
    // `font` is ASCII `32 + i`, which both candidate fonts satisfy.
    //
    // `font_indexed` says which of the two the caller handed over: `SML_ALPH.SPR` carries
    // PALETTE INDICES and is coloured through `palette` like any sprite - that is what keeps
    // its black outline - while `MICRO.FNT` is a bare mask and takes the caller's colour flat.
    // Returns false if there is nothing to draw with, and the HUD then quietly draws nothing.
    //
    // `minimap` is the level's `TOPMAP.SPR` glyph, if it has one. It goes in the same atlas but
    // through ITS OWN palette - the level's `MAP.COL`, which is what the original draws it with -
    // since the rest of the set is coloured by `DEFAULT.COL`.
    bool build(const std::vector<SpriteGlyph>& sprites, const std::vector<SpriteGlyph>& font,
               bool font_indexed, const std::vector<RGB>& palette,
               const SpriteGlyph* minimap = nullptr,
               const std::vector<RGB>* minimap_palette = nullptr);
    bool ready() const { return texture_ != 0 && program_ != 0; }

    // One frame: begin, emit, flush. Coordinates are in PIXELS from the top-left corner, which
    // is what a layout is easiest to read in.
    void begin(int screen_w, int screen_h);
    void sprite(int index, float x, float y, float scale = 1.0f, float alpha = 1.0f);
    // Same, centred horizontally on `cx`.
    void sprite_centred(int index, float cx, float y, float scale = 1.0f, float alpha = 1.0f);
    void text(const std::string& s, float x, float y, float scale, float r, float g, float b,
              float a = 1.0f);
    void text_centred(const std::string& s, float cx, float y, float scale, float r, float g,
                      float b, float a = 1.0f);

    // --- the readouts, in the game's OWN digits -------------------------------------------
    //
    // The lap board, the clock and the speed are numbers, and the game has a font for numbers:
    // `GAMESVGA.SPR`'s small set (indices 27-36), the one the damage gauge already uses. These
    // draw with it, so everything numeric on screen is one typeface.
    //
    // ⚠️ MONOSPACED, deliberately: the artwork's `1` is half the width of its `0`, so a
    // proportional clock jitters sideways every tenth of a second. Each digit is centred in a
    // cell as wide as the widest one. `scale` is applied to the artwork's own 42-pixel height.
    float digit_cell(float scale) const;
    float number_width(int value, float scale, int min_digits = 1) const;
    // Returns the pen position after the number, so a caller can keep writing.
    float number(int value, float x, float y, float scale, int min_digits = 1,
                 float alpha = 1.0f);
    // `M:SS.CC` from a millisecond count, separators included.
    float clock_width(float scale) const;
    void clock(uint32_t ms, float x, float y, float scale, float alpha = 1.0f);
    // The three separators the readouts need, drawn as blocks out of the atlas's white pixel so
    // they sit at the digits' own weight instead of borrowing a second font's punctuation.
    float separator_width(char c, float scale) const;
    float separator(char c, float x, float y, float scale, float r, float g, float b,
                    float a = 1.0f);
    // A plain filled rectangle - the gauges. It reuses the atlas's one opaque white pixel, so it
    // costs nothing extra and stays in the same draw call.
    void rect(float x, float y, float w, float h, float r, float g, float b, float a = 1.0f);
    void flush();

    // Measurements, so a caller can lay out without guessing.
    float text_width(const std::string& s, float scale) const;
    float text_height(float scale) const { return static_cast<float>(font_rows_) * text_scale(scale); }
    // The whole-number scale `text` will actually use: a glyph is pixel art, so it is magnified
    // by 1, 2, 3... and never by 2.7. Callers that centre something under a line of text need
    // the same figure, hence the accessor.
    static float text_scale(float scale) { return scale < 1.0f ? 1.0f : static_cast<float>(static_cast<int>(scale + 0.5f)); }
    int sprite_width(int index) const;
    // The mini-map, top-left corner at (x, y), `scale` screen pixels per image pixel.
    bool has_minimap() const { return minimap_.ok; }
    int minimap_width() const { return minimap_.w; }
    int minimap_height() const { return minimap_.h; }
    void minimap(float x, float y, float scale, float alpha = 1.0f);
    int sprite_height(int index) const;

private:
    struct Entry {  // where a glyph ended up in the atlas
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        int w = 0, h = 0, advance = 0;
        bool ok = false;
    };
    struct Vertex {
        float x, y, u, v, r, g, b, a;
    };

    void quad(const Entry& e, float x, float y, float w, float h, float r, float g, float bl,
              float a);

    std::vector<Entry> sprites_;
    std::vector<Entry> font_;
    Entry minimap_;
    bool font_indexed_ = false;  // the font carries its own colours (SML_ALPH) - see build()
    int font_rows_ = kFntRows;   // the font's glyph height in pixels, whichever font it is
    Entry white_;  // the one opaque pixel `rect` draws with

    GLuint texture_ = 0, program_ = 0, vao_ = 0, vbo_ = 0;
    GLint screen_loc_ = -1, tex_loc_ = -1;
    std::vector<Vertex> batch_;
    int screen_w_ = 1, screen_h_ = 1;
};

} // namespace bigred
