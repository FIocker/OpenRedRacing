// Render-only camera history. The integer chase controller still runs at 28 ms.
#pragma once

#include <array>

namespace bigred {
struct CameraState;

struct CameraSample {
    double x = 0, y = 0, z = 0;
    double yaw = 0, pitch = 0; // BAM16; angles in the history are unwrapped
};

// Four ticks give a cubic B-spline reconstruction of the middle interval. This adds
// one tick of camera presentation delay versus linear interpolation. Input and
// simulation retain their original timing.
// Velocity and acceleration stay continuous; the curve remains inside the
// samples' convex hull. Reset at camera cuts, never smooth across a respawn.
class CameraMotion {
public:
    void reset(const CameraState& state);
    void push(const CameraState& state, bool cut = false);
    CameraSample sample(float alpha) const;

private:
    std::array<CameraSample, 4> history_{};
    bool initialized_ = false;
};
} // namespace bigred
