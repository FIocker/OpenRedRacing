#include "camera_motion.h"
#include "chase_camera.h"

#include <algorithm>
#include <cmath>

namespace bigred {
namespace {
CameraSample snapshot(const CameraState& s) {
    return {double(s.x), double(s.y), double(s.z), double(s.yaw), double(s.pitch)};
}

double curve(double a, double b, double c, double d, double t) {
    const double t2 = t * t, t3 = t2 * t;
    const double u = 1-t;
    // Uniform cubic B-spline: position, velocity and acceleration meet at every
    // tick boundary. Positive weights summing to one bound the result inside
    // the four samples, unlike an interpolating spline that can overshoot them.
    return (u*u*u*a + (3*t3-6*t2+4)*b + (-3*t3+3*t2+3*t+1)*c + t3*d) / 6;
}
} // namespace

void CameraMotion::reset(const CameraState& state) {
    history_.fill(snapshot(state));
    initialized_ = true;
}

void CameraMotion::push(const CameraState& state, bool cut) {
    CameraSample next = snapshot(state);
    const CameraSample& last = history_.back();
    const double dx = next.x - last.x, dy = next.y - last.y, dz = next.z - last.z;
    if (!initialized_ || cut || dx * dx + dy * dy + dz * dz > 4000.0 * 4000.0) {
        reset(state);
        return;
    }
    next.yaw = last.yaw + std::remainder(next.yaw - last.yaw, 65536.0);
    next.pitch = last.pitch + std::remainder(next.pitch - last.pitch, 65536.0);
    for (size_t i = 0; i + 1 < history_.size(); ++i) history_[i] = history_[i + 1];
    history_.back() = next;
}

CameraSample CameraMotion::sample(float alpha) const {
    const double t = std::clamp(double(alpha), 0.0, 1.0);
    const auto& a = history_[0]; const auto& b = history_[1];
    const auto& c = history_[2]; const auto& d = history_[3];
    return {curve(a.x,b.x,c.x,d.x,t), curve(a.y,b.y,c.y,d.y,t),
            curve(a.z,b.z,c.z,d.z,t), curve(a.yaw,b.yaw,c.yaw,d.yaw,t),
            curve(a.pitch,b.pitch,c.pitch,d.pitch,t)};
}
} // namespace bigred
