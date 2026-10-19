// One vehicle, turning on itself, inside a menu.
//
//
// ⚠️ IT DRAWS THE RACE'S OWN MODEL, not a second, simpler one. The mesh comes from
// scene/vehicle_render.h's `build_vehicle_model` and is placed by its `place_vehicle`, so the
// car a player picks in the menu is - body, wheels, tracks, rotors, team colour and all - the
// car the race then puts on the track. A bespoke loader here would be free to drift from it,
// and would sooner or later show a vehicle the game does not have.
//
// ⚠️ IT NEEDS THE CANVAS' DEPTH BUFFER (app/presentation.h). A car is not convex: its far
// wheels sit behind its near bodywork and back-face culling cannot sort that out, which is why
// the canvas grew a depth attachment when this was written. The globe never needed one.
//
// The pose is the simulation's own `VehiclePose`, which is what lets this be three numbers
// rather than a matrix stack: a yaw that advances with the clock, and a small constant PITCH -
// the vehicle stands nose-down, the way a showroom turntable presents one.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "formats/bigred_archive.h"
#include "formats/threedw.h"     // RGB
#include "render/gl_loader.h"
#include "scene/vehicle_render.h"

namespace bigred {

class VehicleDisplay {
public:
    ~VehicleDisplay();

    // Builds `<game_dir>/<name>.VHC` into a drawable mesh, in `palette` - the screen's own, so
    // the preview is drawn through the same table everything else on it is. `tint` is the
    // player's colour and replaces the authored repaint (scene/vehicle_render.h); null keeps
    // repaint 1. False means there is nothing to show; a screen must still work without it.
    // `logo` is the emblem painted into the sheet's own logo cell before upload - the way the
    // original puts a team badge on a car (scene/vehicle_render.h). Null keeps the authored one.
    bool load(const std::string& game_dir, const std::string& name,
              const std::vector<RGB>& palette, const VehicleTint* tint,
              const VehicleLogo* logo = nullptr, const VehicleDriver* driver = nullptr);
    // A DRIVER instead of a vehicle (scene/character_model.h): the body, and the head hung on
    // it as one more rigid part, exactly as a vehicle hangs its head at the `&40` anchor. It
    // stands upright - no forward lean. `tint` is the character's colour.
    bool load_character(const std::string& game_dir, int index, const std::vector<RGB>& palette,
                        const VehicleTint* tint);
    bool ready() const { return program_ != 0 && model_.valid(); }
    void release();

    // Draws it inside the canvas rectangle whose top-left corner is (`x`, `y`), in canvas
    // pixels - the same top-left convention every document on the screen lays out in.
    // `turn` is how far round it has come, in TURNS (a whole revolution is 1.0), because
    // everything that feeds it is a fraction of a revolution.
    void draw(int canvas_w, int canvas_h, float x, float y, float w, float h, float turn);

    // How far forward it leans, in turns. ~9 degrees: enough to show the roof and the bonnet
    // at once, little enough that the car still reads as standing on its wheels.
    static constexpr float kForwardTiltTurns = 0.025f;

private:
    bool build_gl();
    // Everything after the model exists: GL objects, the sheets, the framing sphere.
    bool finish(const std::vector<std::vector<uint8_t>>& sheets);
    void upload_textures(const std::vector<std::vector<uint8_t>>& sheets);

    VehicleModel model_;
    GLuint program_ = 0, vao_ = 0, vbo_ = 0;
    // The vehicle's OWN `.TM` sheets - a wheel rim, a hubcap - stacked as the layers of one
    // array texture, exactly as the world stacks a level's. One or two per vehicle.
    GLuint textures_ = 0;
    int texture_count_ = 0;
    GLint u_mvp_ = -1, u_atlas_ = -1;

    // The bounding sphere the camera frames, about the vehicle's OWN origin - which is the axis
    // it turns about, so the framing cannot breathe as it turns.
    float radius_ = 1.0f;
    float centre_y_ = 0.0f;
    float tilt_turns_ = kForwardTiltTurns;
};

} // namespace bigred
