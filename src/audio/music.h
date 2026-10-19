// The music: the CD tracks of the original, as OGG files in the game's own `MUSIC/` folder.
//
//
// The original plays ONE CD TRACK AT A TIME through MCI (`MCI_PLAY` from track `n` to `n+1`)
// and starts the next one when it is notified the last one ended. There is no CD here, so the
// same thing is done with a playlist of files and a decoder.
//
// ⚠️ THE FILE NUMBERING IS NOT LOAD-BEARING. On a mixed-mode CD track 1 is the DATA track, so
// the music starts at track 2 and a rip named by CD track number starts at `02`. A rip named by
// song number starts at `01`. Both work: the scan takes every numerically-named file it finds,
// sorts it, and plays the list in order. What matters is the ORDER, not the first number - so
// do not "fix" a folder that starts at 01.
//
// It runs on its own thread, feeding a ring buffer that the audio callback drains. That is the
// whole reason for the thread: a level load blocks the main thread for seconds, and music that
// stops during the loading screen is exactly what a player notices.
#pragma once

#include <atomic>
#include <cstdint>
#include <random>
#include <string>
#include <thread>
#include <vector>

struct stb_vorbis;

namespace bigred {

// One entry of the playlist, as found on disk.
struct MusicTrack {
    int number = 0;         // the number in its name: 2 for `02.ogg`, and 2 for `2.ogg`
    std::string path;       // UTF-8, absolute
};

class MusicPlayer {
public:
    ~MusicPlayer();

    // Finds the tracks under `game_dir` and starts the decoder thread. `out_rate` and `device`
    // come from the Mixer: the music is mixed into ITS callback, so `device` is the lock that
    // makes the ring buffer safe (SDL holds it while the callback runs).
    //
    // Returns false when there is no music to play - no folder, no files, no audio device.
    // ⚠️ THAT IS NOT AN ERROR. A copy of the game with no `MUSIC/` folder runs silently, with
    // no message, which is the decision taken with the user on 2026-09-21.
    bool open(const std::string& game_dir, int out_rate, uint32_t device);
    void close();
    bool ready() const { return !tracks_.empty() && thread_.joinable(); }

    int track_count() const { return static_cast<int>(tracks_.size()); }
    const std::vector<MusicTrack>& playlist() const { return tracks_; }
    // The playlist index being decoded, or -1 when stopped. It is what the DECODER is on, which
    // is up to half a second ahead of what is being heard.
    int current() const { return playing_.load() ? index_.load() : -1; }

    void play(int index = 0);   // wraps; out-of-range is taken modulo the playlist
    void next();
    void stop();
    bool paused() const { return !playing_.load(); }

    // ⚠️ THE GAME'S OWN MODEL, and it is not a playlist: each context asks for a CD TRACK
    // NUMBER and that track is what plays. The front end asks for track 2; a circuit asks for
    // the two numbers on its line of `TOURN.TXT` - an **intro** played once, then the
    // **music** looped for as long as the race lasts (formats/tourn.h).
    //
    // `loop_cd_track` of -1 means "loop `cd_track` itself".
    //
    // ⚠️ THE NUMBERS ARE THE ORIGINAL CD'S, and they run up to 38 - the disc has short intro
    // jingles between the music. A folder ripped from that disc matches them exactly; a folder
    // that holds only the music (13 files, which is what a rip off the web gives) cannot.
    //
    // ⚠️ A NUMBER WITH NO FILE PLAYS A RANDOM ONE (user, 2026-09-22): have the piece, play the
    // piece; do not have it, play something. A draw rather than a fixed stand-in, so a circuit
    // the folder cannot serve does not end up welded to the same substitute for ever - and so
    // two circuits are not silently married to one file.
    //
    // ⚠️ THE DRAW NEVER TOUCHES THE SIMULATION. Music is presentation; nothing here is read by
    // anything that has to step identically on two machines.
    void play_cd_track(int cd_track, int loop_cd_track = -1);
    // Which playlist entry a CD track number lands on: the exact file if there is one, a random
    // entry otherwise. -1 only when there is no music at all. ⚠️ NOT a pure function - two
    // calls for a missing number give two different answers.
    int resolve_cd_track(int cd_track);
    // True only for an EXACT match - a file really named after that CD track. It is how a
    // caller tells "I have this piece" from "I have something standing in for it": an intro is
    // played when its own file is there and skipped when it is not, never drawn for.
    bool has_cd_track(int cd_track) const;
    // True while the file really named after that CD track is the one playing.
    bool playing_cd_track(int cd_track) const;

    void set_volume(int v);     // 0..256, applied to the stream alone
    int volume() const { return volume_.load(); }

    // ⚠️ AUDIO THREAD, called by Mixer::mix with the device lock held. Adds the stream into
    // `out` (interleaved stereo) and never blocks: when the decoder has not kept up it simply
    // contributes silence for those frames.
    void render(int16_t* out, int frames);

    // How many frames have actually reached the device. It is the only way to tell "the music
    // is playing" from "the decoder thinks it is playing" without ears - see `--music`.
    uint64_t frames_delivered() const { return delivered_.load(); }

private:
    void decode_loop();
    bool open_track(int index);              // decoder thread only
    void close_track();                      // decoder thread only
    bool pull_source(int16_t frame[2]);      // decoder thread only
    size_t produce(int16_t* dst, size_t want); // decoder thread only
    void flush();                            // decoder thread only, takes the device lock
    size_t free_frames() const;              // decoder thread only, takes the device lock

    std::vector<MusicTrack> tracks_;
    uint32_t device_ = 0;
    int out_rate_ = 44100;

    // The ring, in interleaved stereo frames. `read_` and `write_` are monotonic frame counters
    // taken modulo `ring_frames_`; both are only ever touched under the device lock.
    std::vector<int16_t> ring_;
    size_t ring_frames_ = 0;
    size_t read_ = 0, write_ = 0;

    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> playing_{false};
    std::atomic<int> index_{0};        // playlist index the decoder is on
    std::atomic<int> request_{-1};     // a track the main thread asked for; -1 = nothing pending
    // What to play when the current track ends: a playlist index to LOOP, or -1 for "the next
    // one in the list". The original only ever loops - the list only walks itself under
    // `--music`, which is a diagnostic, not the game.
    std::atomic<int> loop_index_{-1};
    std::atomic<int> volume_{192};     // 0..256. Below the sound effects on purpose
    std::atomic<bool> flush_pending_{false};
    std::atomic<uint64_t> delivered_{0};
    // The draw for a CD track this folder does not carry. Seeded once; presentation only.
    std::mt19937 rng_{std::random_device{}()};

    // Decoder thread state. The whole file is held in memory - the biggest track of the
    // original is 7.7 MB - because stb_vorbis' file API goes through fopen(), which on Windows
    // reads its path through the ANSI codepage and cannot open this project's own install path.
    stb_vorbis* vorbis_ = nullptr;
    std::vector<uint8_t> file_;
    int file_rate_ = 44100;
    std::vector<int16_t> decoded_;     // source frames waiting to be resampled, interleaved
    size_t decoded_fill_ = 0, decoded_used_ = 0;   // in int16s, not frames
    // Linear resampling, for the day the device does not run at the file's rate - which is not
    // hypothetical: SDL opens with ALLOW_FREQUENCY_CHANGE and Windows commonly hands back
    // 48 kHz for a 44.1 kHz request. `step_` is 32.32 source frames per output frame, so equal
    // rates give exactly 1 and the interpolation reduces to a copy.
    int16_t prev_[2]{}, cur_[2]{};
    bool primed_ = false;
    uint64_t frac_ = 0, step_ = 0;
};

} // namespace bigred
