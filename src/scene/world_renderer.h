// THE WORLD, DRAWN: one renderer for every executable that shows a level.
//
// Everything LevelScene bakes - terrain, static decor, the animation steps, the destruction
// states, the start lights, the billboards, the horizon objects - is uploaded and drawn here, in
// the one order the world has, with the one pair of programs (render/world_shaders.h). The race
// and map_preview used to carry a copy each, and the two had already drifted apart: the viewer
// had neither the start lights nor the horizon objects (code review of 2026-09-28, A1).
// A new kind of world geometry is added HERE, once.
//
// What is not the world - the vehicles, the ground effects, a debugging overlay - is handed in
// per frame as ready-made vertices (WorldFrame), and drawn in its proper place in the order.
//
// ⚠️ GL resources: create() and upload() need a current GL context, and release() must run
// while it still is - a screen calls it from exit(), as it does for everything else it owns.
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "formats/shading.h"
#include "formats/terrain.h"
#include "render/gl_loader.h"
#include "render/mat4.h"
#include "render/sky.h"
#include "scene/level_scene.h"

namespace bigred {

// Where the world is seen from, in RENDER space (+Y up, X mirrored, kRenderScale).
struct WorldView {
    Mat4 mvp = Mat4::identity();
    Mat4::Vec3 eye{0.0f, 0.0f, 0.0f};
    Mat4::Vec3 forward{0.0f, 0.0f, 1.0f};
    Mat4::Vec3 right{1.0f, 0.0f, 0.0f};
    Mat4::Vec3 up{0.0f, 1.0f, 0.0f};
    float tan_half_fov = 0.57735027f;   // tan(30 degrees): the 60-degree field of both callers
    float aspect = 16.0f / 9.0f;
};

// Everything one frame needs that is not the level itself.
struct WorldFrame {
    WorldView view;
    // The wall clock, for what is animated but not simulated: the `anim` steps, the billboards.
    uint32_t wall_ms = 0;
    // The destruction state and the simulation tick it is read at. Null: everything intact.
    const SceneryState* world = nullptr;
    int32_t sim_tick = 0;
    // Which state of the start gantry shows - LevelScene::StartLight.
    LevelScene::StartLight start_light = LevelScene::StartLight::Red;
    // The distance lighting. The game takes the defaults; map_preview tunes them live.
    int shade_view_cells = kShadeViewDistanceCellsDefault;
    float shade_strength = kShadeStrengthDefault;
    float shade_dark_ceil = kShadeDarkCeilDefault;
    // Streamed geometry, drawn in its place in the order. VEHICLES are back-face culled (the
    // original shows its glass from the inside only); EFFECTS are not; LINES are GL_LINES,
    // depth-tested (map_preview's checkpoint ring). Null or empty: nothing.
    const std::vector<TerrainVertex>* vehicles = nullptr;
    const std::vector<TerrainVertex>* effects = nullptr;
    const std::vector<TerrainVertex>* lines = nullptr;
    // THE CABIN: drawn LAST, over a cleared depth buffer, with its own
    // projection - its near plane is a couple of units, where the world's is ten. Like the
    // original, which paints it in a pass of its own after everything: nothing of the world can
    // come through the dashboard. Null or empty: nothing.
    const std::vector<TerrainVertex>* cabin = nullptr;
    // Its parts - the steering wheel - painted OVER it, on a depth buffer cleared again:
    // the original draws the body and then the parts, and without a depth buffer the order is
    // the whole story. The wheel is anchored level with the dashboard (z 100 on the JEEP), so a
    // depth test put it behind the panel (user, 2026-09-29: "le volant doit etre visible devant").
    const std::vector<TerrainVertex>* cabin_parts = nullptr;
    Mat4 cabin_mvp = Mat4::identity();
};

class WorldRenderer {
public:
    WorldRenderer() = default;
    WorldRenderer(const WorldRenderer&) = delete;
    WorldRenderer& operator=(const WorldRenderer&) = delete;
    ~WorldRenderer();

    // The two programs and the streamed buffers. False only when the world program itself does
    // not compile; a sky program that fails leaves a flat clear colour behind, not an error.
    bool create();
    // Every static buffer and the texture atlas, from the scene as it is now. Call again after
    // the scene is reloaded (map_preview's level switch).
    void upload(const LevelScene& scene);
    // The meshes alone - vbuf, the animation steps, the destruction states - for a scene rebaked
    // in place (map_preview's height-scale and decor-Y keys). The atlas is untouched.
    void upload_meshes(const LevelScene& scene);
    void release();
    bool ready() const { return program_ != 0; }

    // How the textures are sampled: smoothed (the default), or nearest texel. The game turns
    // smoothing off on the low rungs of the `+`/`-` ladder (320x200, 640x480): the original
    // sampled nearest, and its big pixels smoothed over look like a blurred modern render
    // (user, 2026-09-29). Cheap to call every frame: the texture is touched only on a change.
    void set_smooth_textures(bool smooth);

    // Rewrites atlas layer `layer` - both halves, raw and lit, as a vehicle's sheet is emissive -
    // with `rgba` (kTmWidth x kTmHeight RGBA8). The cabin's dashboard, redrawn every frame.
    void update_layer(int layer, const std::vector<uint8_t>& rgba);

    // Clears the current viewport and draws the world into it: sky, horizon objects, terrain,
    // decor, animation steps, destruction states, start lights, lines, vehicles, effects,
    // billboards - in that order.
    void draw(const LevelScene& scene, const WorldFrame& f);

private:
    struct Buffer {
        GLuint vao = 0, vbo = 0;
    };
    void draw_sky(const LevelScene& scene, const WorldView& v);
    void stream(Buffer& b, const std::vector<TerrainVertex>& v, GLenum mode = GL_TRIANGLES);

    GLuint program_ = 0, sky_program_ = 0;
    GLint mvp_loc_ = -1, atlas_loc_ = -1, cam_pos_loc_ = -1, shade_raw_loc_ = -1,
          shade_span_loc_ = -1, atlas_layers_loc_ = -1, shade_strength_loc_ = -1,
          shade_dark_ceil_loc_ = -1, cull_side_loc_ = -1;
    struct SkyU {
        GLint forward = -1, right = -1, up = -1, tan_half = -1, aspect = -1;
        GLint sky_color[kSkyBands.size()]{};
        GLint sky_angle[kSkyBands.size()]{};
        GLint ground_color[kGroundBands.size()]{};
        GLint ground_angle[kGroundBands.size()]{};
    } sky_u_;
    GLuint sky_vao_ = 0;

    Buffer world_, anim_, wreck_, lights_;       // static
    Buffer sprites_, horizon_, vehicles_, effects_, lines_, cabin_, cabin_parts_;   // streamed
    GLuint atlas_ = 0;
    int atlas_layers_ = 0;
    bool smooth_textures_ = true;
    size_t vbuf_size_ = 0, terrain_vertices_ = 0;
    std::vector<std::vector<std::pair<int, int>>> anim_ranges_;
    std::vector<TerrainVertex> scratch_;
};

} // namespace bigred
