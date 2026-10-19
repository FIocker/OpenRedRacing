// The original's menu screens, rebuilt in RmlUi: its picture, its sprites and its wibbly
// lettering at the very places its `.SCN` script and its `TOURN.<LANG>` text put them.
//
// The decision: the modern engine keeps the lists and anything read, the original's pieces
// build the rest.
//
// Coordinates are the ORIGINAL SCREEN'S, 320x200. On the 960x720dp canvas every menu is laid
// out on (app/rml_screen.h) a column is 3 dp and a line 3.6 dp: the picture is the original's
// stretched to 4:3, exactly as a VGA monitor stretched it, and so is everything on it.
#pragma once

#include <string>
#include <vector>

#include "formats/localised_text.h"
#include "formats/sprite.h"
#include "formats/threedw.h"   // RGB

namespace bigred {

class App;

// 320x200 -> dp.
inline float scn_dp_x(int x) { return static_cast<float>(x) * 3.0f; }
inline float scn_dp_y(int y) { return static_cast<float>(y) * 3.6f; }

class ScnScreen {
public:
    // Opens `<name>.SCN` and reads its palette off `raw` (by default `<name>.RAW`; some screens
    // borrow another screen's picture). Every glyph of `spr` (by default `<name>.SPR`, when the
    // archive has one) is registered as an image through THAT palette - the original has one
    // palette per screen, so a sprite is only right in its own screen's colours.
    // `raw_archive` is where the picture lives when it is not in the `.SCN` itself: TRAKSCN's
    // is the circuit's own `VIEW.RAW`, in its `.LEV` - and so is the palette of its sprites.
    bool load(App& app, const std::string& name, const std::string& raw = {},
              const std::string& spr = {}, const std::string& raw_archive = {});

    const std::string& name() const { return name_; }
    // The background's image source, for `<img id="bg" src=...>`.
    std::string background_src() const { return bg_src_; }
    // Glyph `i` of the screen's `.SPR`, registered by load(); empty when there is none.
    std::string sprite_src(int i) const;
    int sprite_count() const { return static_cast<int>(sprites_.size()); }
    int sprite_w(int i) const;
    int sprite_h(int i) const;

    // Shade 2 of palette ramp `ramp`, as "#rrggbb": the colour the original draws a text in
    // after `@C<ramp>` (palette index ramp*16 + 2).
    std::string ramp_colour(int ramp) const;

    // The runs of `key` at their `@X` places, each an absolutely placed `.st` element; runs the
    // entry does not place continue the one before them as a span. Relative to (ox, oy) - the
    // corner of the box they are put in, (0, 0) for the whole screen. `extra` is added to each
    // element's class (e.g. "b" for the second wibble phase).
    std::string text_rml(const TextTable& text, const std::string& key, int ox = 0, int oy = 0,
                         const std::string& extra = {}) const;
    // Words of OUR OWN - an entry the original does not have - placed and coloured as if an
    // `@X<x>,<y> @C<ramp>` had put them there.
    std::string words_rml(const std::string& words, int x, int y, int ramp = -1, int ox = 0,
                          int oy = 0, const std::string& extra = {}) const;
    // A clickable box on the original's `mbox` rectangle, holding `inner` (usually the texts
    // of that choice, laid out with ox/oy = the box's corner).
    static std::string box_open(const std::string& id, int x, int y, int w, int h,
                                const std::string& cls = "hit");
    // A sprite of the screen's `.SPR` at (x, y), its own size, relative to (ox, oy).
    std::string sprite_rml(int i, int x, int y, int ox = 0, int oy = 0) const;
    // An `anisprite`: glyphs first..first+count-1 cycled every 5 ticks (70 ms), as
    // `anim 5,5,255, 0,1,2,3,4` does in every `.SCN`. RmlUiScreen turns the frames.
    std::string anisprite_rml(int first, int count, int x, int y, int ox = 0, int oy = 0,
                              const std::string& id = {}) const;
    // The rules every rebuilt screen shares: a focused or hovered choice's wibbly text turns
    // yellow (kFocusRamp), a focused or hovered sprite button grows a little. Put inside the
    // document's <style>.
    std::string focus_css() const;
    // "<rml><head><style>" + focus_css() + `css` + "</style></head><body>" + the picture.
    std::string document_open(const std::string& css = {}) const;

    // A text with no `@C` of its own keeps the colour it is drawn in: palette index 16, white
    // in every menu palette. HYPOTHESE - the original recolours only fonts flagged in
    // a table only filled at run time; the palettes reserving 16 for white is what
    // decides it. The ramp that marks the focused choice, yellow in every menu palette.
    static constexpr int kFocusRamp = 5;

private:
    std::string name_, bg_src_;
    std::vector<RGB> palette_;
    std::vector<SpriteGlyph> sprites_;
};

} // namespace bigred
