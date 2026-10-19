#include "net_race_driver.h"

#include <cmath>
#include <cstdio>

#include "net/net_protocol.h"
#include "sim/lockstep.h"
#include "sim/snapshot.h"

namespace bigred {

void NetRaceDriver::begin(NetGame& net, uint32_t delay, size_t racers, bool trace) {
    net_ = &net;
    trace_ = trace;
    sent_upto_ = delay;
    per_.assign(racers, VehicleInput());
}

int NetRaceDriver::frame(RaceSession& session, double& accumulator, double step, double dt_ms,
                         const VehicleInput& local, uint32_t stop_at, const Ticker& t) {
    resync_age_ms_ += dt_ms;
    if (resync_banner_ms_ > 0.0) resync_banner_ms_ -= dt_ms;
    // "WAITING FOR THE NETWORK" only once the race has really STOPPED. A machine that is ahead
    // of the others - the host, one hop from everybody, most of all - runs up to the last input
    // it has and waits a few milliseconds for the next one on most frames: that is lockstep
    // doing its job, and printing it made the host read "waiting" all race.
    const int ran = step_ticks(session, accumulator, step, local, stop_at, t);
    if (ran > 0 || !waiting_) {
        // Every stall long enough to show is written down, with whom it waited for: what the
        // next test between two PCs needs to say which machine held the race up.
        if (stall_ms_ >= 300.0 && session.race().started())
            std::printf("[net] waited %.0f ms for %s before tick %u\n", stall_ms_,
                        racer_name(stall_who_).c_str(), session.ticks());
        stall_ms_ = 0.0;
    } else {
        stall_ms_ += dt_ms;
        if (waiting_for_ >= 0) stall_who_ = waiting_for_;
    }
    return ran;
}

// The lockstep loop. The local input sampled this frame is sent
// for the tick it is scheduled on - `delay` ticks ahead - and a tick is run only once EVERY
// racer's input for it is in. A machine that is ahead waits here; one that is behind catches
// up with several ticks in a frame, up to a bound.
int NetRaceDriver::step_ticks(RaceSession& session, double& accumulator, double step,
                              const VehicleInput& local, uint32_t stop_at, const Ticker& t) {
    waiting_ = false;
    waiting_for_ = -1;
    if (!net_->active() || !net_->race_go()) {
        // Before the host's GO, and after the game is gone: nothing runs, and no time is owed.
        accumulator = 0.0;
        waiting_ = net_->active();
        return 0;
    }
    LockstepInputs& lk = net_->inputs();
    int ran = 0;
    if (!handle_resync(session, t)) {
        waiting_ = true;
        if (accumulator > kLockstepWaitBankTicks * step) accumulator = kLockstepWaitBankTicks * step;
        return 0;
    }
    while (accumulator >= step && ran < kLockstepMaxTicksPerFrame &&
           (!stop_at || session.ticks() < stop_at)) {
        const uint32_t next = session.ticks() + 1;
        const uint32_t due = lk.schedule(next);
        if (sent_upto_ < due) {
            sent_upto_ = due;
            net_->send_input(due, pack_input(local));
        }
        const LockstepGate gate = lockstep_gate(lk, next, accumulator / step);
        if (gate == LockstepGate::Wait) {
            waiting_ = true;
            waiting_for_ = lk.missing(next);
            break;
        }
        // ⚠️ ONE TICK IN RESERVE. The machine that is ahead would otherwise run each tick the
        // moment its last input lands, so its race moved to the rhythm of the packets rather
        // than the clock. Holding the last input back for up to one tick is a jitter buffer:
        // the ticks leave on time for as long as the inputs arrive within 28 ms of each other.
        // It changes WHEN a tick runs, never what it computes.
        if (gate == LockstepGate::Hold) break;
        accumulator -= step;
        lk.inputs_for(next, per_.data());
        t.run(next, per_);
        lk.release_before(next);
        if (next % kNetChecksumEvery == 0) net_->report_checksum(next, session.checksum());
        if (!handle_resync(session, t)) {
            waiting_ = true;
            break;
        }
        if (trace_ && next % 250 == 0) {
            std::printf("[autotest] tick %u checksum %08x\n", next, session.checksum());
            std::fflush(stdout);
        }
        ++ran;
    }
    // Waiting for someone: do not bank the time. Racing to catch up on a pile of it later would
    // only make this machine the one everyone else waits for next.
    if (waiting_ && accumulator > kLockstepWaitBankTicks * step)
        accumulator = kLockstepWaitBankTicks * step;
    net_->flush();
    return ran;
}

// ⚠️ "LOADED" ONLY ONCE THE RACE HAS BEEN DRAWN AND SHOWN TWICE. A GL driver compiles its
// shaders and uploads its textures on their first real use, not when they are created, so the
// first frames of a race can take hundreds of milliseconds. Said at the end of enter(), those
// frames fell after the host's GO, and the host sat on "WAITING FOR THE NETWORK" for the
// client's warm-up (user's two-PC test, 2026-09-27). Drawn frames, not time: it is the work of
// drawing that has to be over.
void NetRaceDriver::frame_drawn() {
    if (!loaded_sent_ && ++warm_frames_ > 2) {
        loaded_sent_ = true;
        net_->race_loaded();
    }
}

bool NetRaceDriver::handle_resync(RaceSession& session, const Ticker& t) {
    const uint32_t r = net_->resync_tick();
    if (r == 0 || session.ticks() != r) return true;
    if (net_->is_host()) {
        net_->send_snapshot(r, save_race(session));
        resync_banner_ms_ = 2500.0;
        return true;
    }
    const std::vector<uint8_t>* snap = net_->snapshot_for(r);
    if (!snap) return false;   // not here yet: hold at tick R
    // Where everything is drawn now, before the snapshot moves it.
    std::vector<VehiclePose> before(static_cast<size_t>(session.racer_count()));
    for (int i = 0; i < session.racer_count(); ++i)
        before[static_cast<size_t>(i)] = drawn_pose(i, pose_of(session.car(i)));
    const bool ok = load_race(session, snap->data(), snap->size());
    std::printf("[net] snapshot of tick %u %s (%zu bytes)\n", r, ok ? "loaded" : "REFUSED",
                snap->size());
    std::fflush(stdout);
    net_->snapshot_applied(ok);
    if (!ok) return true;   // another build: the desync stays on the board, the race goes on
    // Both halves of every interpolation pair are the snapshot's now (Ticker::resynced), so
    // nothing is drawn as a movement across the gap; the offset alone carries the old picture
    // and fades it out.
    resync_offset_.assign(static_cast<size_t>(session.racer_count()), DrawOffset());
    for (int i = 0; i < session.racer_count(); ++i) {
        const VehiclePose now = pose_of(session.car(i));
        DrawOffset& o = resync_offset_[static_cast<size_t>(i)];
        o.x = before[static_cast<size_t>(i)].x - now.x;
        o.y = before[static_cast<size_t>(i)].y - now.y;
        o.z = before[static_cast<size_t>(i)].z - now.z;
        float dyaw = before[static_cast<size_t>(i)].yaw - now.yaw;
        while (dyaw > 32768.0f) dyaw -= 65536.0f;
        while (dyaw < -32768.0f) dyaw += 65536.0f;
        o.yaw = dyaw;
        // A car put somewhere else entirely is not smoothed - it would sweep across the map.
        if (o.x * o.x + o.y * o.y + o.z * o.z > 4000.0f * 4000.0f) o = DrawOffset();
    }
    t.resynced();
    resync_age_ms_ = 0.0;
    resync_banner_ms_ = 2500.0;
    return true;
}

VehiclePose NetRaceDriver::drawn_pose(int racer, const VehiclePose& p) const {
    if (racer < 0 || static_cast<size_t>(racer) >= resync_offset_.size()) return p;
    // Half of what is left every 80 ms: 90 % gone in a quarter of a second, nothing left to see
    // after half a second.
    constexpr double kHalfLifeMs = 80.0;
    if (resync_age_ms_ > 12.0 * kHalfLifeMs) return p;
    const float k = static_cast<float>(std::exp2(-resync_age_ms_ / kHalfLifeMs));
    const DrawOffset& o = resync_offset_[static_cast<size_t>(racer)];
    VehiclePose q = p;
    q.x += o.x * k;
    q.y += o.y * k;
    q.z += o.z * k;
    q.yaw += o.yaw * k;
    return q;
}

std::string NetRaceDriver::racer_name(int racer) const {
    const auto& racers = net_->race().racers;
    if (racer < 0 || racer >= static_cast<int>(racers.size())) return "THE NETWORK";
    return racers[static_cast<size_t>(racer)].look.name;
}

} // namespace bigred
