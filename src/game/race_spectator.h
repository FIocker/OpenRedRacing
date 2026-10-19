// SPECTATING: a player who is out of the race watches the others through the circuit's own
// trackside cameras (`fixcam`) - the one nearest the racer followed, cutting to another when it
// is clearly nearer, the racer followed changing at random now and then. A chase camera on the
// target when no trackside camera is near enough.
//
// Out of RaceScreen since 2026-09-28 (code review, item A2). Drawing
// only: its dice are local and nothing it does reaches the simulation. WHO may be followed is
// the race's business and is decided by the screen, which hands the list in each tick.
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "formats/level_script.h"
#include "scene/chase_camera.h"
#include "sim/race_session.h"

namespace bigred {

class RaceSpectator {
public:
    // The level's trackside cameras, minus the odd one the terrain buries (two on the whole
    // game).
    void load(const std::string& level_txt, const SimTerrain& terrain);

    // Once per simulation tick, after the race has stepped. `want`: this view should be
    // spectating now. `racing`: the racers that may be followed, the preferred ones only - an
    // empty list leaves the camera where it is (everyone is home: the results are a tick away).
    void tick(const RaceSession& session, const SimTerrain& terrain, bool want,
              const std::vector<int>& racing);

    bool active() const { return active_; }
    int target() const { return target_; }   // the racer followed, -1 before the first
    // The camera of this tick and the one before, for the display to interpolate between.
    const CameraState& now() const { return now_; }
    const CameraState& prev() const { return prev_; }

private:
    std::vector<FixedCamera> cams_;
    bool active_ = false;
    int target_ = -1;
    int cam_ = -1;                 // index into cams_, -1 = the chase camera on the target
    uint32_t cam_since_ = 0;       // tick of the last cut
    uint32_t target_since_ = 0;
    ChaseCamera chase_;
    CameraState now_{}, prev_{};
};

} // namespace bigred
