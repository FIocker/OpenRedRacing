#include "race_spectator.h"

#include <algorithm>
#include <cstdio>
#include <random>

#include "sim/fixed.h"

namespace bigred {

void RaceSpectator::load(const std::string& level_txt, const SimTerrain& terrain) {
    cams_.clear();
    for (const FixedCamera& c : parse_fixed_cameras(level_txt))
        if ((terrain.height_fx(c.x << 9, c.z << 9) >> 9) - c.y > 0) cams_.push_back(c);
}

void RaceSpectator::tick(const RaceSession& session, const SimTerrain& terrain, bool want,
                         const std::vector<int>& racing) {
    // Kept BEFORE this tick moves it, so the pair always holds two consecutive ticks.
    prev_ = now_;
    if (!want) {
        active_ = false;
        return;
    }
    if (racing.empty()) return;
    const uint32_t now = session.ticks();
    // The racer followed: kept while it races, changed at random every ten seconds or so when
    // there is a choice. Drawing only, so a local random draw is fine.
    constexpr uint32_t kTargetHold = 10000 / kPhysicsTickMs;
    const bool still = std::find(racing.begin(), racing.end(), target_) != racing.end();
    if (!active_ || !still || (racing.size() > 1 && now - target_since_ > kTargetHold)) {
        static std::mt19937 rng{std::random_device{}()};
        int pick = racing[rng() % racing.size()];
        if (racing.size() > 1)
            while (pick == target_) pick = racing[rng() % racing.size()];
        target_ = pick;
        target_since_ = now;
        cam_since_ = 0;   // a new target may cut at once
    }
    const VehicleSim& t = session.car(target_);
    const int32_t tx = t.world_x(), ty = t.world_y() - 100, tz = t.world_z();   // aim above it
    // The nearest trackside camera; a cut only when another is clearly nearer (by a fifth) and
    // the current one has been up for two seconds - no flicker between two equal ones.
    constexpr int64_t kFar = 6000;
    constexpr uint32_t kCutHold = 2000 / kPhysicsTickMs;
    auto dist2 = [&](const FixedCamera& c) {
        const int64_t dx = c.x - tx, dy = c.y - ty, dz = c.z - tz;
        return dx * dx + dy * dy + dz * dz;
    };
    int best = -1;
    for (size_t k = 0; k < cams_.size(); ++k)
        if (best < 0 || dist2(cams_[k]) < dist2(cams_[static_cast<size_t>(best)]))
            best = static_cast<int>(k);
    if (best >= 0 && dist2(cams_[static_cast<size_t>(best)]) > kFar * kFar) best = -1;
    const bool first = !active_ || cam_since_ == 0;
    if (first) {
        cam_ = best;
        cam_since_ = now;
        if (cam_ < 0) chase_.reset(t, terrain);
    } else if (best != cam_ && now - cam_since_ > kCutHold) {
        const int64_t cur = cam_ >= 0 ? dist2(cams_[static_cast<size_t>(cam_)]) : kFar * kFar;
        if (best < 0 || dist2(cams_[static_cast<size_t>(best)]) * 25 < cur * 16) {
            cam_ = best;
            cam_since_ = now;
            if (cam_ < 0) chase_.reset(t, terrain);
        }
    }
    if (cam_ >= 0) {
        const FixedCamera& c = cams_[static_cast<size_t>(cam_)];
        now_.x = c.x;
        now_.y = c.y;
        now_.z = c.z;
        const int32_t dx = tx - c.x, dz = tz - c.z;
        now_.yaw = fx::atan2_bam16(dx, dz) & 0xFFFF;
        int32_t pitch = fx::atan2_bam16(ty - c.y, fx::hypot32(dx, dz)) & 0xFFFF;
        if (pitch > 0x8000) pitch -= 0x10000;
        if (pitch > 0x3000) pitch = 0x3000;
        if (pitch < -0x3000) pitch = -0x3000;
        now_.pitch = pitch & 0xFFFF;
    } else {
        chase_.step(t, terrain);
        now_ = chase_.now;
    }
    if (!active_ || first) prev_ = now_;   // a cut is a cut, not a pan
    if (cam_since_ == now) {
        std::printf("[spectator] tick %u: racer %d, %s\n", now, target_,
                    cam_ >= 0 ? cams_[static_cast<size_t>(cam_)].name.c_str() : "chase camera");
        std::fflush(stdout);
    }
    active_ = true;
}

} // namespace bigred
