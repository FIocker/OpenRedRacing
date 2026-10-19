// The rotating globe of `GLOBESCN`: the original's own world map, on a real sphere.
//
// The short version of what it draws with:
//
//   `WORLDTX1.TM` + `WORLDTX2.TM`  the Earth, in `GLOBE.COL`'s palette
//   `VENUSTEX.TM`                  Mars, Venus and the Moon - ONE texture, three palettes
//                                  (`MARSPAL`/`VENUSPAL`/`MOONPAL.COL`)
//   `NAMES.TM` + `NAMES2.TM`       23 hand-written country names, 128x32 each
//
// ⚠️ THE SPHERE IS THE ORIGINAL'S OWN, not a generic one: 12 slices by 8 bands, built by
// the original, wearing its static UV table. It is not a `.3DW` - none of the
// 555 models shipped with the game is a sphere - so it is rebuilt here as the original builds it.
//
// ⚠️ THE TWO WORLD TEXTURES ARE A GORE MAP, not an equirectangular one: each row is cut into
// strips separated by palette index 4 (a flat green), the way an orange peel is cut to lie
// flat. They are used AS THEY ARE, because the original's UVs pave those strips exactly and never
// sample the green. Do not try to un-gore them: the first version of this file did, and its
// map was stretched and could only be aimed at by statistics.
//
// ⚠️ IT DRAWS WITH THE DEPTH TEST OFF, and needs nothing else: a sphere is convex, so back-face
// culling alone leaves exactly the near half visible. The markers are not part of the sphere,
// so they are hidden by hand when they face away - see draw(). (The menu canvas does carry a
// depth buffer nowadays, for the car screen's turning vehicle; the globe has no use for it.)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "render/gl_loader.h"

namespace bigred {

// A place on the globe, as `TOURN.TXT` states it - the raw binary angles, converted here and
// nowhere else. See formats/tourn.h for what they mean.
struct GlobeMarker {
    int globe_x = 0, globe_y = 0;
    bool selected = false;
};

// Which body the globe currently IS. The Earth carries every circuit that has a position; the
// other three are whole destinations of their own (`-1,-1,<n>` in `TOURN.TXT`).
enum class GlobeBody { Earth = 0, Mars = 1, Venus = 2, Moon = 3 };

class Globe {
public:
    ~Globe();

    // Unpacks `GLOBESCN.SCN` and builds the sphere, the four body textures and the name sheet.
    // False means the artwork is not there; a caller must still be able to run its screen, so
    // nothing here is fatal on its own.
    bool load(const std::string& game_dir);
    bool ready() const { return program_ != 0 && vao_ != 0; }
    void release();

    // Draws the sphere centred at (`cx`, `cy`) canvas pixels, `radius` canvas pixels across its
    // equator, turned `yaw_turns` about the poles and tilted `pitch_turns` towards the viewer.
    // Both angles are in TURNS, not degrees or radians: everything that feeds them - the
    // circuits' own coordinates, the idle spin - is a fraction of a revolution.
    void draw(int canvas_w, int canvas_h, float cx, float cy, float radius,
              float yaw_turns, float pitch_turns, GlobeBody body,
              const std::vector<GlobeMarker>& markers);

    // The yaw that brings a circuit's marker to face the viewer, and the pitch that lifts it to
    // the middle of the disc. Feed them to draw() - usually eased towards, not jumped to.
    static float yaw_for(int globe_x);
    static float pitch_for(int globe_y);

    // One country's hand-written name, RGBA8 and 128x32, for a document to show over the globe.
    // Empty when the index is not one of the 23 the sheet holds.
    static constexpr int kNameWidth = 128, kNameHeight = 32;
    std::vector<uint8_t> name_image(int name_index) const;

    // `GLOBESCN.SPR`'s own widgets, RGBA8, already through the screen's palette: the two
    // arrows and the passport stamp, five animation frames each. `out_w`/`out_h` receive the
    // frame's size. Empty when the sheet is not there.
    enum class Widget { RightArrow, LeftArrow, Stamp };
    std::vector<uint8_t> widget_image(Widget widget, int frame, int& out_w, int& out_h) const;

private:
    bool build_sphere();
    bool build_program();

    GLuint program_ = 0, vao_ = 0, vbo_ = 0;
    GLuint marker_vao_ = 0, marker_vbo_ = 0, marker_program_ = 0;
    GLint u_yaw_ = -1, u_pitch_ = -1, u_scale_ = -1, u_centre_ = -1, u_tex_ = -1, u_light_ = -1;
    GLuint body_texture_[4]{};
    int vertex_count_ = 0;

    // Kept palette-indexed and decoded on demand: a document asks for one name at a time, and
    // 23 RGBA images would be 376 KB held for the life of a screen to show one of them.
    std::vector<uint8_t> names_[2];
    std::vector<uint8_t> screen_palette_;   // THIRD3.RAW's own 768 bytes
    std::vector<uint8_t> globe_spr_;
};

} // namespace bigred
