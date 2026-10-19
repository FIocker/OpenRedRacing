#include "race_audio.h"

#include <cmath>
#include <cstdio>

#include "formats/level_script.h"   // env_name_for_level

namespace bigred {

void RaceAudio::configure(Mixer* mixer, const Config& c) {
    mixer_ = mixer;
    cfg_ = c;
    const RaceSession& s = *cfg_.session;
    const RaceField& field = *cfg_.field;
    const int n = s.racer_count();
    const std::string env = env_name_for_level(cfg_.level);
    const std::string& vehicle = field.vehicle[static_cast<size_t>(cfg_.player)];
    const VehicleTuning& tuning = field.tunings[static_cast<size_t>(cfg_.player)];

    // SOUND. The samples are loose `.WAV` files in folders next to the game, not in an archive,
    // and which folder to look in comes from the `hnd fx` line's own directory prefix - see
    // audio/sound_bank.h.
    sounds_.configure(cfg_.game_dir, vehicle, cfg_.level, env);
    lines_ = parse_commentary(cfg_.level_txt);
    commentators_.assign(static_cast<size_t>(n), Commentator());
    was_respawned_.assign(static_cast<size_t>(n), 0);
    cfg_.wreck_colours.resize(static_cast<size_t>(n), 0);
    if (cfg_.trace)
        std::printf("[comment] %s: GENRAL x%d, JUMP x%d, ROLL x%d - commentary %s\n",
                    cfg_.level.c_str(), lines_.general, lines_.jump, lines_.roll,
                    cfg_.commentary ? "on" : "off");
    // Read now rather than on the first line said: a WAV decoded mid-race is a hitch.
    if (cfg_.commentary && ready()) {
        const auto preload = [&](const char* name, int count) {
            for (int k = 1; k <= count; ++k) sounds_.get("lev", std::string(name) + std::to_string(k));
        };
        preload("GENRAL", lines_.general);
        preload("JUMP", lines_.jump);
        preload("ROLL", lines_.roll);
    }
    if (ready()) car_audio_.configure(&tuning, &sounds_, mixer_);
    other_audio_.assign(static_cast<size_t>(n), VehicleAudio());
    other_banks_.clear();
    if (ready()) {
        for (int i = 0; i < n; ++i) {
            if (i == cfg_.player || !s.car(i).active) continue;
            const std::string& name = field.vehicle[static_cast<size_t>(i)];
            std::unique_ptr<SoundBank>& bank = other_banks_[name];
            if (!bank) {
                bank = std::make_unique<SoundBank>();
                bank->configure(cfg_.game_dir, name, cfg_.level, env);
            }
            other_audio_[static_cast<size_t>(i)].configure(&field.tunings[static_cast<size_t>(i)],
                                                          bank.get(), mixer_);
        }
    }
    int declared = 0, resolved = 0;
    for (int k = 0; k < kFxCategoryCount; ++k) {
        for (int i = 0; i < kFxIndexCount; ++i) {
            const FxSound& e = tuning.fx[k][i];
            if (!e.declared()) continue;
            ++declared;
            if (!sounds_.resolve(e.dir, e.name).empty()) ++resolved;
        }
    }
    std::printf("%d `hnd fx` line(s), %d resolved to a file%s.\n", declared, resolved,
                ready() ? "" : " (no audio device: silent)");
}

void RaceAudio::on_race_start() {
    if (cfg_.commentary) play("lev", "READYGO", 256, true);
}

void RaceAudio::stop() {
    car_audio_.stop();
    for (VehicleAudio& a : other_audio_) a.stop();
    // ⚠️ NOT close(): the device belongs to the App and the music is still playing through it.
    // Only this race's voices go.
    if (mixer_) mixer_->stop_all();
}

void RaceAudio::listener_to(const CameraState& cam, const VehicleSim& c, int32_t& distance,
                            int32_t& pan) const {
    // As in the original: the 3D distance from the camera, and the car's bearing relative to where
    // the camera looks. The original turns the bearing into a pan with sin(angle) >> 24, i.e.
    // +/-128 about the centre; the same here, from the camera's right-hand vector.
    //
    // ⚠️ The game frame is X-mirrored AND Y-down, which together are a rotation (det +1), so
    // "right" keeps its handedness: with forward = (sin yaw, cos yaw), right = (cos yaw, -sin yaw)
    // - forward x (0, -1, 0), -Y being up here.
    const double dx = static_cast<double>(c.px >> 9) - static_cast<double>(cam.x);
    const double dy = static_cast<double>(c.py >> 9) - static_cast<double>(cam.y);
    const double dz = static_cast<double>(c.pz >> 9) - static_cast<double>(cam.z);
    const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
    distance = d > 0x7fffffff ? 0x7fffffff : static_cast<int32_t>(d);
    pan = 128;
    const double flat = std::sqrt(dx * dx + dz * dz);
    if (distance <= 99 || flat < 1.0) return;
    const double yaw = (cam.yaw & 0xFFFF) * (6.283185307179586 / 65536.0);
    const double side = (dx * std::cos(yaw) - dz * std::sin(yaw)) / flat;
    pan = 128 + static_cast<int32_t>(side * 128.0);
    if (pan < 0) pan = 0;
    if (pan > 256) pan = 256;
}

void RaceAudio::play(const char* dir, const char* name, int32_t volume, bool speech) {
    // The race's own sounds are NOT `hnd fx` lines: `READYGO`, `TONE1` and `TONE2` are string
    // literals in the executable, played through the same path-building idiom as everything
    // else. `READYGO` carries the `lev` prefix - one per circuit, 24 of them
    // shipped - and the tones live in `SAMPLES`, which is what `front` resolves to.
    if (!ready()) return;
    const Sample* sample = sounds_.get(dir, name);
    if (!sample) return;
    VoiceParams vp;
    vp.rate = sample->rate;
    vp.volume = volume;
    vp.speech = speech;   // louder than the cap, and ducks the rest - see audio/mixer.h
    const VoiceId id = mixer_->play(*sample, vp, false, 0, -1, false);
    if (speech) speech_voice_ = id;
}

void RaceAudio::say(const char* name, int count, int& last) {
    if (!cfg_.commentary || count <= 0) return;
    // ⚠️ ONE LINE AT A TIME - ours, not the original's (user, 2026-09-28): a line due while
    // another is still being said is dropped, not queued - late, it would comment on something
    // already over. Before the dice, so a dropped line does not use up a "no repeat".
    if (mixer_ && mixer_->playing(speech_voice_)) return;
    // The original draws from the C runtime's rand(); ours is a private LCG, the same shape,
    // because the simulation's own dice must not move when a line is said.
    dice_ = dice_ * 1103515245u + 12345u;
    int v = static_cast<int>((dice_ >> 16) & 0x7fff) % count;
    if (v == last) v = (last + 1) % count;
    last = v;
    const std::string line = std::string(name) + std::to_string(v + 1);
    if (cfg_.trace) {
        std::printf("[comment] %s (tick %u)\n", line.c_str(), cfg_.session->ticks());
        std::fflush(stdout);
    }
    play("lev", line.c_str(), 256, true);
}

// SOUND, on the tick and not on the frame. A landing, an impact and a refused boost each exist
// for exactly ONE tick; a frame that happens to span two of them would swallow one, and a frame
// that spans none would fire it twice. See audio/vehicle_audio.h.
void RaceAudio::on_tick(const CameraState& listener) {
    if (!cfg_.session) return;
    const RaceSession& s = *cfg_.session;
    const RaceState& race = s.race();
    const int player = cfg_.player;
    if (ready()) {
        // How far the listener is from the car, in world units. The listener is the camera, so
        // this is the chase rope - around 600 units, i.e. comfortably inside the original's
        // 800-unit full-volume radius.
        int32_t dist = 0, pan = 128;
        listener_to(listener, s.car(player), dist, pan);
        // Centred: the chase camera keeps the car in front of it, and the original's own
        // listener code zeroes the angle of anything nearer than 100 units anyway.
        car_audio_.on_tick(s.car(player), dist, 128);
        // Then everyone else, in grid order, as the original's second loop does. The mixer's caps (8
        // voices, 3 engines) do the rest: first come, first served, the player being first.
        for (int i = 0; i < s.racer_count(); ++i) {
            if (i == player || !s.car(i).active) continue;
            int32_t d = 0, p = 128;
            listener_to(listener, s.car(i), d, p);
            other_audio_[static_cast<size_t>(i)].on_tick(s.car(i), d, p);
        }
        // Car against car: the sound belongs to the car that was hit, as in the original.
        const RaceField& field = *cfg_.field;
        for (int i = 0; i < s.racer_count(); ++i) {
            const int j = s.car(i).vehicle_hit_sound;
            if (j < 0 || j >= s.racer_count()) continue;
            const bool same = field.vehicle[static_cast<size_t>(i)] == field.vehicle[static_cast<size_t>(j)];
            int32_t d = 0, p = 128;
            listener_to(listener, s.car(j), d, p);
            if (j == player) car_audio_.on_car_contact(same, d, 128);
            else other_audio_[static_cast<size_t>(j)].on_car_contact(same, d, p);
        }
        // `hnd fx check`: 0 = checkpoint, 1 = lap completed, 2 = checkpoint missed (a line
        // touched out of order), 3 = wrong way, 4 = a line driven PAST. One of them at most per
        // tick.
        const RacerProgress& me = race.progress(player);
        if (me.wrong_way_now && !me.finished) car_audio_.on_race_event(3, dist, 128);
        else if (me.missed_window_now) car_audio_.on_race_event(4, dist, 128);
        else if (me.missed_check_now) car_audio_.on_race_event(2, dist, 128);
        else if (me.lap_crossed) car_audio_.on_race_event(1, dist, 128);
        else if (me.checkpoint_crossed) car_audio_.on_race_event(0, dist, 128);
        commentary();
    }
    // The count itself: TONE1 on three, two, one and TONE2 on the green light - the original's
    // own two samples, fired at 210, 140, 70 and 0 ticks left.
    if (race.countdown_beeped()) play("front", race.countdown_number() == 0 ? "TONE2" : "TONE1", 256);
}

// Three triggers, all per car:
//   GENRAL  a checkpoint, a lap or a missed line of a HUMAN's car: one time in
//           four (`rand() & 3 == 0`) a line is armed 105 ticks (2.9 s) ahead; the physics tick
//           counts it down and says it at zero. A new event re-arms it, it never stacks.
//   JUMP    airborne 20 ticks, more than 220 units up, not falling back from a
//           recovery: one flight in two (`rand() & 1`), once per flight.
//   ROLL    the moment the body rolls past 90 degrees (its rolled-over flag rising).
// ⚠️ ONLY THIS MACHINE'S PLAYERS. The original tests for a human player (1 or 2) for GENRAL and nothing
// for the other two, which in solo is the same thing (the ghosts are tapes, they run no tick).
// In a network race the other humans' cars do tick here, and their lines would come out of
// our speakers, unplaced: kept to the local seats.
void RaceAudio::commentary() {
    const RaceSession& s = *cfg_.session;
    const RaceState& race = s.race();
    // Not commentary, but the same kind of string literal: EXTRA3 from SAMPLES, every time a
    // car is put back on the track, COMMENTARY or not.
    // Once per press: holding ALT puts the car back on every tick (the input is a held key),
    // and a sample restarted every 28 ms is a pile of copies of itself (user, 2026-09-28).
    for (int i = 0; i < s.racer_count() && static_cast<size_t>(i) < was_respawned_.size(); ++i) {
        const bool now = s.respawned(i);
        if (now && !was_respawned_[static_cast<size_t>(i)] && seat_of(i) >= 0)
            play("front", "EXTRA3", 256);
        was_respawned_[static_cast<size_t>(i)] = now ? 1 : 0;
    }
    if (!cfg_.commentary || commentators_.empty()) return;
    // A WRECK IS ANNOUNCED, whoever it is - the one line that is about the race rather than the
    // player (the original runs it over every car). A circuit with DEAD lines says one of them;
    // otherwise the car's colour, "RED1".."RED2" - only the first when `fx` says one. The count
    // is the simulation's own (VehicleSim::wrecked_ticks), like the race's end rules.
    for (int i = 0; i < s.racer_count(); ++i) {
        const VehicleSim& c = s.car(i);
        if (c.damage >= 0 || c.wrecked_ticks != static_cast<int32_t>(kWreckSayTicks)) continue;
        static constexpr const char* kColourWord[6] = {"RED", "GREEN", "YELLOW", "BLUE", "ORANGE", "PURPLE"};
        int no_repeat = -1;   // a local in the original: nothing to avoid
        if (lines_.dead > 1) say("DEAD", lines_.dead, no_repeat);
        else say(kColourWord[cfg_.wreck_colours[static_cast<size_t>(i)] % 6], lines_.dead == 1 ? 1 : 2, no_repeat);
    }
    // FINISH: the player over the line in a race of one's own - the single race and the
    // championship. Only RIG3 declares one (`fx &010003`).
    if (!finish_said_ && cfg_.alone && race.progress(cfg_.player).finished) {
        finish_said_ = true;
        int no_repeat = -1;
        say("FINISH", lines_.finish, no_repeat);
    }
    for (int i = 0; i < s.racer_count(); ++i) {
        if (seat_of(i) < 0) continue;
        Commentator& c = commentators_[static_cast<size_t>(i)];
        const VehicleSim& v = s.car(i);
        const RacerProgress& p = race.progress(i);
        if (p.checkpoint_crossed || p.lap_crossed || p.missed_check_now) {
            dice_ = dice_ * 1103515245u + 12345u;
            if ((((dice_ >> 16) & 0x7fff) & 3) == 0) c.general_in = 105;
        }
        if (c.general_in > 0 && --c.general_in == 0) say("GENRAL", lines_.general, last_general_);

        if (s.respawned(i)) c.dropping = true;
        if (v.near_ground != 0) {
            c.jump_said = false;
            c.dropping = false;
        } else if (!c.jump_said && v.air_ticks >= 20 && v.height_above > 220 && !c.dropping) {
            c.jump_said = true;
            dice_ = dice_ * 1103515245u + 12345u;
            if ((((dice_ >> 16) & 0x7fff) & 1) != 0) say("JUMP", lines_.jump, last_jump_);
        }

        if (v.rolled_over) {
            if (!c.rolled) say("ROLL", lines_.roll, last_roll_);
            c.rolled = true;
        } else {
            c.rolled = false;
        }
    }
}

} // namespace bigred
