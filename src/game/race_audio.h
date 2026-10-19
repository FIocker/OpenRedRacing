// Everything a race SOUNDS like: every car's engine, skid, roll and nitro voices, the contacts,
// the race's own events (checkpoint, lap, wrong way, the countdown tones), and the commentator.
//
// Out of RaceScreen since 2026-09-28 (code review, A2). It is
// PRESENTATION: it reads the simulated race once per tick and writes nothing back, and its own
// dice are its own - a line said or not said never moves the simulation.
//
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "audio/mixer.h"
#include "audio/sound_bank.h"
#include "audio/vehicle_audio.h"
#include "formats/level_script.h"
#include "scene/chase_camera.h"
#include "scene/race_level.h"
#include "sim/race_session.h"

namespace bigred {

class RaceAudio {
public:
    struct Config {
        std::string game_dir, level, level_txt;
        const RaceField* field = nullptr;       // outlives this object
        const RaceSession* session = nullptr;   // outlives this object
        int player = 0;                         // the first local player's racer
        // Per racer: 0 for the first local player, 1..3 for a split-screen guest, -1 otherwise.
        // Only this machine's players are commented on (see race_audio.cpp).
        std::vector<int> seats;
        // Per racer, which colour a wreck is announced by (RED GREEN YELLOW BLUE ORANGE PURPLE).
        std::vector<int> wreck_colours;
        bool commentary = true;   // the profile's COMMENTARY
        bool alone = true;        // no guest and no network: FINISH may be said
        bool trace = false;       // -DEBUG: every line said, to the console
    };

    // Nothing is fatal: no device, or no files, and the race runs silent.
    void configure(Mixer* mixer, const Config& c);
    bool ready() const { return mixer_ && mixer_->ready(); }

    // The starter's line, once, before the countdown winds down, and only with COMMENTARY on.
    void on_race_start();
    // Once per simulation tick, after the race has stepped. `listener` is the camera the sound
    // is heard from - the chase camera, or the spectator's.
    void on_tick(const CameraState& listener);
    // This race's voices, not the device: the music plays on through it.
    void stop();

    // The DEAD line (or the colour's) 175 timer ticks after the wreck. In 28 ms ticks.
    static constexpr uint32_t kWreckSayTicks = 88;

private:
    void play(const char* dir, const char* name, int32_t volume, bool speech = false);
    // One of `count` numbered lines, never `last` again.
    void say(const char* name, int count, int& last);
    void commentary();
    // Distance and pan of a car from the listener.
    void listener_to(const CameraState& cam, const VehicleSim& c, int32_t& distance,
                     int32_t& pan) const;
    int seat_of(int racer) const {
        return racer >= 0 && static_cast<size_t>(racer) < cfg_.seats.size()
                   ? cfg_.seats[static_cast<size_t>(racer)] : -1;
    }

    Mixer* mixer_ = nullptr;
    Config cfg_;
    SoundBank sounds_;
    VehicleAudio car_audio_;
    // THE OTHER RACERS - ghosts and remote humans alike. The original runs the same per-car pass
    // over every car: the player's first, then the others in grid order, each with
    // its own distance and pan from the camera. One bank per MODEL, since the `vhc` prefix is the
    // vehicle's own folder; one VehicleAudio per racer, empty for the player's slot.
    std::unordered_map<std::string, std::unique_ptr<SoundBank>> other_banks_;
    std::vector<VehicleAudio> other_audio_;

    // THE COMMENTARY - the circuit's own spoken lines.
    struct Commentator {
        int32_t general_in = 0;   // ticks until a GENRAL line, 0 = none pending
        bool jump_said = false;   // pushed to 1000 in the original: this flight has been judged
        bool rolled = false;      // ROLL already said for this roll-over
        bool dropping = false;    // put back on the track, not landed yet
    };
    std::vector<Commentator> commentators_;   // one per racer, only the local humans used
    LevelCommentary lines_{};
    // The last line said of each kind - the original's three globals, shared by every car: the same line never comes twice in a row.
    int last_general_ = -1, last_jump_ = -1, last_roll_ = -1;
    bool finish_said_ = false;   // FINISH, once, when the player crosses the line alone
    uint32_t dice_ = 0x2545f491u;
    // The spoken line playing now, if any: a new one is simply not said while this one lasts
    // (user, 2026-09-28).
    VoiceId speech_voice_;
    // Per racer: was it put back on the track last tick? ALT HELD puts it back EVERY tick, and
    // EXTRA3 must go off once per press, not 36 times a second on top of itself.
    std::vector<uint8_t> was_respawned_;
};

} // namespace bigred
