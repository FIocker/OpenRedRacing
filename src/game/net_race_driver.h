// THE NETWORK RACE's clockwork: the lockstep loop that runs a tick only once every racer's input
// for it is in, the waits it leaves on screen, and the resync - the host's snapshot sent or
// loaded at tick R, and the drawn cars sliding onto it rather than jumping.
//
// Out of RaceScreen since 2026-09-28 (code review, A2). It owns
// no race: the screen hands it the session, the frame's banked time and its local input, and
// runs each tick through a callback. What ends a network race (the finish countdown, the humans
// home) is a race rule and stays with the screen until the rules move into the session (A3).
//
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "net/net_game.h"
#include "scene/vehicle_render.h"
#include "sim/race_session.h"
#include "sim/vehicle.h"

namespace bigred {

class NetRaceDriver {
public:
    // What the driver asks of the screen.
    struct Ticker {
        // Runs tick `tick` with every network racer's input, in lockstep order.
        std::function<void(uint32_t tick, const std::vector<VehicleInput>& per)> run;
        // A snapshot has just replaced the race: every interpolation pair restarts from it.
        std::function<void()> resynced;
    };

    // Before the race: the network game (the App's, borrowed), the input delay of its start
    // message and the number of network racers. `trace`: -AUTODRIVE's checksum every 250 ticks.
    void begin(NetGame& net, uint32_t delay, size_t racers, bool trace);

    // One frame. Runs the ticks whose inputs are all in, `step` seconds each, out of
    // `accumulator`, never past `stop_at` (0 = no limit), and keeps count of how long the race
    // has been held up and by whom. Returns the number of ticks run.
    int frame(RaceSession& session, double& accumulator, double step, double dt_ms,
              const VehicleInput& local, uint32_t stop_at, const Ticker& t);
    // A race frame has been drawn: after the third, the host is told this machine has loaded.
    void frame_drawn();

    // Where racer `racer` is DRAWN, pose `p` plus what is left of the resync's offset.
    VehiclePose drawn_pose(int racer, const VehiclePose& p) const;

    // For the line under the board (RaceScreen::network_status).
    bool waiting() const { return waiting_; }            // this frame ended short of an input
    double stall_ms() const { return stall_ms_; }        // how long no tick has run
    bool resync_banner() const { return resync_banner_ms_ > 0.0; }
    // A network racer's name; "THE NETWORK" when unknown.
    std::string racer_name(int racer) const;
    std::string stall_name() const { return racer_name(stall_who_); }
    bool stalled_on_someone() const { return stall_who_ >= 0; }

private:
    int step_ticks(RaceSession& session, double& accumulator, double step,
                   const VehicleInput& local, uint32_t stop_at, const Ticker& t);
    // THE RESYNC (NetGame::resync_tick). False while this machine has to wait at tick R for the
    // host's snapshot; the host sends it from here, a client loads it from here.
    bool handle_resync(RaceSession& session, const Ticker& t);

    NetGame* net_ = nullptr;
    bool trace_ = false;
    uint32_t sent_upto_ = 0;          // the last tick the local input was sent for
    std::vector<VehicleInput> per_;   // one per network racer, lockstep order
    bool waiting_ = false;            // this frame ended waiting for someone's input
    double stall_ms_ = 0.0;           // how long no tick has run for want of an input
    int waiting_for_ = -1;            // the network racer whose input is missing, or -1
    int stall_who_ = -1;              // ... as it stood during the current stall
    int warm_frames_ = 0;             // race frames drawn before saying "loaded"
    bool loaded_sent_ = false;
    // What the eye sees of a resync: every car's drawn pose is offset by where it WAS drawn
    // minus where the snapshot put it, and the offset melts away over a few hundred ms instead
    // of the car jumping. Drawing only - the simulation took the snapshot whole.
    struct DrawOffset {
        float x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f;
    };
    std::vector<DrawOffset> resync_offset_;
    double resync_age_ms_ = 1e9;      // since the last resync, wall clock
    double resync_banner_ms_ = 0.0;   // "RESYNCHRONISED" stays up this long
};

} // namespace bigred
