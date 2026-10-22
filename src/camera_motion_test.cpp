#include "scene/camera_motion.h"
#include "scene/chase_camera.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {
int failures = 0;
void check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; ++failures; }
}
bool near(double a, double b, double tolerance = 0.001) {
    return std::abs(a - b) <= tolerance;
}
bigred::CameraState state(int x, int yaw = 0, int pitch = 0) {
    return {x, -x, 2*x, yaw, pitch};
}
bigred::CameraMotion history(int a, int b, int c, int d) {
    bigred::CameraMotion motion;
    motion.reset(state(a));
    motion.push(state(b)); motion.push(state(c)); motion.push(state(d));
    return motion;
}

void velocity_continuity() {
    auto m = history(0, 7, 15, 31);
    constexpr float h = 0.001f;
    const auto end = m.sample(1);
    const double before = (end.x - m.sample(1-h).x) / h;
    constexpr float a = 0.01f;
    const double acceleration_before =
        (end.x - 2*m.sample(1-a).x + m.sample(1-2*a).x) / (a*a);
    m.push(state(32));
    const auto start = m.sample(0);
    const double after = (m.sample(h).x - start.x) / h;
    const double acceleration_after =
        (m.sample(2*a).x - 2*m.sample(a).x + start.x) / (a*a);
    check(near(end.x, start.x), "position discontinuity at a physics tick");
    check(near(before, after, 0.05), "velocity discontinuity at a physics tick");
    check(near(before, (31.0-7)/2, 0.05), "shared tangent does not match adjacent motion");
    check(near(acceleration_before, acceleration_after, 0.5),
          "acceleration discontinuity at a physics tick");
}

void bounded_extrema() {
    const auto turn = history(0, 20, 10, 30);
    const auto plateau = history(5, 5, 5, 5);
    for (int i = 0; i <= 1000; ++i) {
        const auto p = turn.sample(i / 1000.0f);
        check(p.x >= 0 && p.x <= 30, "position escapes the recent path's bounds");
        check(p.y >= -30 && p.y <= 0, "altitude escapes the recent path's bounds");
        check(near(plateau.sample(i / 1000.0f).x, 5), "stationary interval drifts");
    }
    check(near(turn.sample(-1).x, turn.sample(0).x) &&
          near(turn.sample(2).x, turn.sample(1).x), "alpha is not clamped");
}

void wrap_and_cuts() {
    bigred::CameraMotion m;
    m.reset(state(0, 65520, -20));
    m.push(state(10, 8, -8)); m.push(state(20, 24, 8)); m.push(state(30, 40, 20));
    const auto a = m.sample(0), b = m.sample(1);
    check(b.yaw-a.yaw > 0 && b.yaw-a.yaw < 32, "yaw took the long arc at north");
    check(std::abs(m.sample(0.5f).pitch) < 1, "pitch did not interpolate across zero");
    m.push(state(130), true); // explicit short respawn: under the distance heuristic
    check(near(m.sample(0).x, 130) && near(m.sample(1).x, 130), "explicit cut smeared old motion");
    m.push(state(100000));
    check(near(m.sample(0.5f).x, 100000), "teleport was interpolated across the map");
}

void display_rates() {
    constexpr double step = 0.028;
    for (int fps : {60, 120, 144, 240}) {
        bigred::CameraMotion m;
        m.reset(state(0));
        int tick = 0;
        double previous = 0;
        for (int frame = 1; frame <= fps * 2; ++frame) {
            const double time = double(frame) / fps;
            while ((tick+1)*step <= time + 1e-12) { ++tick; m.push(state(tick*28)); }
            const float alpha = float((time-tick*step)/step);
            const auto p = m.sample(alpha);
            if (tick >= 4) {
                check(near(p.x, 1000*(time-2*step)), "frame rate changed constant-speed motion");
                if (frame > fps/2) check(near(p.x-previous, 1000.0/fps), "constant speed held or jumped between frames");
            }
            previous = p.x;
        }
    }
}
} // namespace
int main() {
    velocity_continuity(); bounded_extrema(); wrap_and_cuts(); display_rates();
    if (failures) return 1;
    std::cout << "camera motion: continuous velocity/acceleration, bounded path, angle wrap, cuts and 60/120/144/240 Hz passed\n";
}
