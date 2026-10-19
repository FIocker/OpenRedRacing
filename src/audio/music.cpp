#include "music.h"

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "formats/game_files.h"

// The decoder. One translation unit, so the implementation is included here rather than built
// as a target of its own - stb_vorbis is a single public-domain file and this is how it is
// meant to be used. See src/third_party/README.md for why it lives in the repo.
#include "third_party/stb_vorbis.c"

namespace bigred {
namespace {

// How much decoded audio is kept ahead of the callback. Half a second: long enough that a
// stutter needs the decoder thread to be starved for half a second (it is not), short enough
// that "next track" is heard almost at once - the ring is flushed on a track change, but the
// frames already inside the device's own buffer are not.
constexpr double kBufferSeconds = 0.5;
// Frames pulled out of stb_vorbis per pass. ~46 ms at 44.1 kHz.
constexpr int kDecodeFrames = 2048;

// Both spellings, because the shipped folder is `MUSIC` on a Windows install and this is meant
// to keep working the day it is read on a case-sensitive filesystem.
constexpr const char* kFolders[] = {"MUSIC", "music"};

} // namespace

MusicPlayer::~MusicPlayer() { close(); }

bool MusicPlayer::open(const std::string& game_dir, int out_rate, uint32_t device) {
    close();
    if (device == 0 || out_rate <= 0) return false;

    // ⚠️ NO DIRECTORY SCAN, ON PURPOSE. The names are numbers, so probing them is both simpler
    // and immune to the path-encoding trap that bit the archive loader and the sound bank (see
    // bigred_archive.h): `read_file_utf8`'s conversion is already correct, and a directory API
    // would need its own. 00..99 covers every numbering a rip of a 14-track CD can use.
    for (const char* folder : kFolders) {
        for (int n = 0; n <= 99; ++n) {
            char padded[8], bare[8];
            std::snprintf(padded, sizeof padded, "%02d", n);
            std::snprintf(bare, sizeof bare, "%d", n);
            // Zero-padded first: `02.ogg` is the naming the project settled on, and if both
            // spellings of the same number are there we must not queue the track twice.
            for (const char* stem : {padded, bare}) {
                for (const char* ext : {".ogg", ".OGG"}) {
                    const std::string path =
                        game_path(game_path(game_dir, folder), std::string(stem) + ext);
                    if (!file_exists_utf8(path)) continue;
                    MusicTrack t;
                    t.number = n;
                    t.path = path;
                    tracks_.push_back(std::move(t));
                    goto found;   // this number is taken; on to the next
                }
            }
        found:;
        }
        if (!tracks_.empty()) break;
    }
    if (tracks_.empty()) return false;   // no music: not an error, see the header

    std::stable_sort(tracks_.begin(), tracks_.end(),
                     [](const MusicTrack& a, const MusicTrack& b) { return a.number < b.number; });

    device_ = device;
    out_rate_ = out_rate;
    ring_frames_ = static_cast<size_t>(out_rate * kBufferSeconds);
    ring_.assign(ring_frames_ * 2, 0);
    read_ = write_ = 0;
    decoded_.assign(static_cast<size_t>(kDecodeFrames) * 2, 0);
    decoded_fill_ = decoded_used_ = 0;

    quit_.store(false);
    thread_ = std::thread(&MusicPlayer::decode_loop, this);
    return true;
}

void MusicPlayer::close() {
    quit_.store(true);
    playing_.store(false);
    if (thread_.joinable()) thread_.join();
    close_track();
    tracks_.clear();
    ring_.clear();
    ring_frames_ = 0;
    read_ = write_ = 0;
    device_ = 0;
}

void MusicPlayer::play(int index) {
    if (tracks_.empty()) return;
    const int n = track_count();
    loop_index_.store(-1);   // walk the list, which is what the diagnostic sweep wants
    request_.store(((index % n) + n) % n);
    playing_.store(true);
}

int MusicPlayer::resolve_cd_track(int cd_track) {
    const int n = track_count();
    if (n == 0) return -1;
    for (int i = 0; i < n; ++i)
        if (tracks_[static_cast<size_t>(i)].number == cd_track) return i;
    // No file carries that number: draw one. See the header - the alternative, a fixed
    // stand-in, welds a circuit to one substitute for ever and marries two circuits to the
    // same file.
    std::uniform_int_distribution<int> pick(0, n - 1);
    return pick(rng_);
}

bool MusicPlayer::has_cd_track(int cd_track) const {
    for (const MusicTrack& t : tracks_)
        if (t.number == cd_track) return true;
    return false;
}

bool MusicPlayer::playing_cd_track(int cd_track) const {
    const int i = current();
    return i >= 0 && static_cast<size_t>(i) < tracks_.size() &&
           tracks_[static_cast<size_t>(i)].number == cd_track;
}

void MusicPlayer::play_cd_track(int cd_track, int loop_cd_track) {
    if (tracks_.empty()) return;
    const int first = resolve_cd_track(cd_track);
    // ⚠️ ONE DRAW, NOT TWO, when both numbers are the same - which is the usual case, a
    // circuit whose intro this folder does not have. Resolving twice would draw twice, and the
    // music would change to a third piece the moment the first one ended.
    const int loop = (loop_cd_track < 0 || loop_cd_track == cd_track)
                         ? first
                         : resolve_cd_track(loop_cd_track);
    if (first < 0) return;
    // Already on the right piece, and already looping it: leave it alone. Restarting the same
    // track between two screens of the same menu would be heard as a stutter, and the original
    // guards the same case (it compares against the track already playing).
    if (playing_.load() && index_.load() == first && loop_index_.load() == loop) return;
    loop_index_.store(loop);
    request_.store(first);
    playing_.store(true);
    // One line per change of music, and it names BOTH numbers: which CD track was asked for and
    // which file answered. When a folder does not hold the original's numbering those differ,
    // and that is exactly the thing a listener cannot tell from a wrong mapping.
    std::printf("Music: CD track %d -> %s%s\n", cd_track,
                tracks_[static_cast<size_t>(first)].path.c_str(),
                tracks_[static_cast<size_t>(first)].number == cd_track
                    ? ""
                    : "  (no file for it: drawn at random)");
    std::fflush(stdout);
}

void MusicPlayer::next() {
    if (tracks_.empty()) return;
    play(index_.load() + 1);
}

void MusicPlayer::stop() {
    playing_.store(false);
    flush_pending_.store(true);
}

void MusicPlayer::set_volume(int v) {
    volume_.store(v < 0 ? 0 : (v > 256 ? 256 : v));
}

// --- the audio thread -----------------------------------------------------------------------

void MusicPlayer::render(int16_t* out, int frames) {
    if (ring_frames_ == 0) return;
    const int32_t vol = volume_.load(std::memory_order_relaxed);
    for (int i = 0; i < frames; ++i) {
        if (read_ == write_) return;   // the decoder has not kept up: silence, never a stall
        delivered_.fetch_add(1, std::memory_order_relaxed);
        const size_t slot = (read_ % ring_frames_) * 2;
        ++read_;
        if (vol == 0) continue;
        int32_t l = out[i * 2] + ((ring_[slot] * vol) >> 8);
        int32_t r = out[i * 2 + 1] + ((ring_[slot + 1] * vol) >> 8);
        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        out[i * 2] = static_cast<int16_t>(l);
        out[i * 2 + 1] = static_cast<int16_t>(r);
    }
}

// --- the decoder thread ---------------------------------------------------------------------

size_t MusicPlayer::free_frames() const {
    SDL_LockAudioDevice(device_);
    const size_t used = write_ - read_;
    SDL_UnlockAudioDevice(device_);
    return ring_frames_ - used;
}

void MusicPlayer::flush() {
    SDL_LockAudioDevice(device_);
    read_ = write_ = 0;
    SDL_UnlockAudioDevice(device_);
}

void MusicPlayer::close_track() {
    if (vorbis_) {
        stb_vorbis_close(vorbis_);
        vorbis_ = nullptr;
    }
    file_.clear();
    file_.shrink_to_fit();
    decoded_fill_ = decoded_used_ = 0;
    primed_ = false;
    frac_ = 0;
}

bool MusicPlayer::open_track(int index) {
    close_track();
    if (index < 0 || index >= track_count()) return false;
    Blob data = read_file_utf8(tracks_[index].path);
    if (data.empty()) return false;
    file_ = std::move(data);
    int err = 0;
    vorbis_ = stb_vorbis_open_memory(file_.data(), static_cast<int>(file_.size()), &err, nullptr);
    if (!vorbis_) {
        std::fprintf(stderr, "music: %s is not readable Ogg Vorbis (error %d)\n",
                     tracks_[index].path.c_str(), err);
        file_.clear();
        return false;
    }
    const stb_vorbis_info info = stb_vorbis_get_info(vorbis_);
    file_rate_ = info.sample_rate > 0 ? info.sample_rate : out_rate_;
    // 32.32 source frames per output frame. Equal rates give exactly 1 and the interpolation
    // below then reduces to a copy.
    step_ = (static_cast<uint64_t>(file_rate_) << 32) / static_cast<uint32_t>(out_rate_);
    return true;
}

// Pulls one source frame out of the decoder. False at the end of the track.
bool MusicPlayer::pull_source(int16_t frame[2]) {
    if (decoded_used_ >= decoded_fill_) {
        const int got = stb_vorbis_get_samples_short_interleaved(
            vorbis_, 2, decoded_.data(), static_cast<int>(decoded_.size()));
        if (got <= 0) return false;
        decoded_fill_ = static_cast<size_t>(got) * 2;
        decoded_used_ = 0;
    }
    frame[0] = decoded_[decoded_used_];
    frame[1] = decoded_[decoded_used_ + 1];
    decoded_used_ += 2;
    return true;
}

// Fills `dst` with up to `want` OUTPUT frames, resampling from the file's rate. Returns how
// many it made; fewer than asked means the track ended.
size_t MusicPlayer::produce(int16_t* dst, size_t want) {
    size_t made = 0;
    if (!primed_) {
        if (!pull_source(prev_) || !pull_source(cur_)) return 0;
        primed_ = true;
        frac_ = 0;
    }
    while (made < want) {
        while (frac_ >= (1ull << 32)) {
            prev_[0] = cur_[0];
            prev_[1] = cur_[1];
            if (!pull_source(cur_)) return made;
            frac_ -= (1ull << 32);
        }
        const int32_t f = static_cast<int32_t>(frac_ >> 20);        // 0..4095
        dst[made * 2] = static_cast<int16_t>(
            prev_[0] + (((cur_[0] - prev_[0]) * f) >> 12));
        dst[made * 2 + 1] = static_cast<int16_t>(
            prev_[1] + (((cur_[1] - prev_[1]) * f) >> 12));
        frac_ += step_;
        ++made;
    }
    return made;
}

void MusicPlayer::decode_loop() {
    std::vector<int16_t> chunk(static_cast<size_t>(kDecodeFrames) * 2, 0);
    int failures = 0;   // consecutive tracks that would not open: stop rather than spin

    while (!quit_.load()) {
        const int wanted = request_.exchange(-1);
        if (wanted >= 0) {
            index_.store(wanted);
            if (!open_track(wanted)) ++failures; else failures = 0;
            flush();
        }
        if (flush_pending_.exchange(false)) flush();

        if (!playing_.load() || failures > track_count()) {
            SDL_Delay(10);
            continue;
        }
        if (!vorbis_) {
            if (!open_track(index_.load())) {
                ++failures;
                index_.store((index_.load() + 1) % track_count());
                continue;
            }
            failures = 0;
        }

        size_t space = free_frames();
        if (space < static_cast<size_t>(kDecodeFrames)) {
            SDL_Delay(5);   // the ring is full: nothing to do until the callback drains it
            continue;
        }

        const size_t made = produce(chunk.data(), static_cast<size_t>(kDecodeFrames));
        if (made > 0) {
            SDL_LockAudioDevice(device_);
            for (size_t i = 0; i < made; ++i) {
                const size_t slot = (write_ % ring_frames_) * 2;
                ring_[slot] = chunk[i * 2];
                ring_[slot + 1] = chunk[i * 2 + 1];
                ++write_;
            }
            SDL_UnlockAudioDevice(device_);
        }
        if (made < static_cast<size_t>(kDecodeFrames)) {
            // End of track. The original is notified the CD track ended and starts one again:
            // the circuit's MUSIC track, which is how an intro plays once and the music then
            // loops behind the whole race (formats/tourn.h). `loop_index_` of -1 means nobody
            // asked for a particular one, so walk the list - the `--music` sweep.
            close_track();
            const int loop = loop_index_.load();
            index_.store(loop >= 0 ? loop : (index_.load() + 1) % track_count());
        }
    }
}

} // namespace bigred
