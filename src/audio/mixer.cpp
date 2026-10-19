#include "mixer.h"

#include <SDL.h>

#include "music.h"

#include <cstdio>
#include <cstring>

namespace bigred {

Mixer::~Mixer() { close(); }

bool Mixer::open() {
    if (device_ != 0) return true;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "audio: SDL_InitSubSystem failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want{};
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    // 1024 frames at 44.1 kHz is ~23 ms of latency - under one simulation tick, so a collision
    // does not audibly lag the bump that caused it.
    want.samples = 1024;
    want.callback = &Mixer::callback;
    want.userdata = this;
    SDL_AudioSpec have{};
    device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (device_ == 0) {
        std::fprintf(stderr, "audio: no device (%s) - the game runs silent\n", SDL_GetError());
        return false;
    }
    out_rate_ = have.freq;
    SDL_PauseAudioDevice(device_, 0);
    return true;
}

void Mixer::close() {
    if (device_ == 0) return;
    SDL_CloseAudioDevice(device_);
    device_ = 0;
}

void Mixer::callback(void* userdata, uint8_t* stream, int len) {
    Mixer* m = static_cast<Mixer*>(userdata);
    std::memset(stream, 0, static_cast<size_t>(len));
    m->mix(reinterpret_cast<int16_t*>(stream), len / static_cast<int>(sizeof(int16_t) * 2));
}

void Mixer::mix(int16_t* out, int frames) {
    // ⚠️ THIS RUNS ON SDL'S AUDIO THREAD, and it must NOT take the device lock: SDL already
    // holds it while the callback runs, and taking it again deadlocks. Every other method here
    // takes it, which is exactly what makes the voice table safe to walk from here.
    //
    // Nothing in this function allocates and nothing blocks. A malloc in an audio callback is a
    // click, and a lock is a stall.
    //
    // The music goes in first, underneath everything: it is a stereo stream already at the
    // device's rate, so it is a copy with a volume applied, and it contributes silence rather
    // than stalling when its decoder has not kept up.
    if (music_) music_->render(out, frames);
    // The duck: every non-speech voice goes down while a line is spoken, ramped across the
    // block from where the last one left it.
    bool speaking = false;
    for (const Voice& v : voices_) if (v.active && v.speech) speaking = true;
    const int32_t duck0 = duck_;
    if (speaking) duck_ = duck_ - kDuckAttack < kDuckGain ? kDuckGain : duck_ - kDuckAttack;
    else duck_ = duck_ + kDuckRelease > 256 ? 256 : duck_ + kDuckRelease;
    const int32_t duck1 = duck_;
    for (Voice& v : voices_) {
        if (!v.active || v.pcm == nullptr) continue;
        const int32_t vol = (v.volume * master_) >> 8;
        // Constant-power would be nicer, but the original pans linearly and these are 8-bit
        // mono samples - the difference is inaudible and the linear form is exact at centre.
        const int32_t lvol0 = (vol * (256 - v.pan)) >> 8;
        const int32_t rvol0 = (vol * v.pan) >> 8;
        const bool ducked = !v.speech && (duck0 < 256 || duck1 < 256);
        int32_t lvol = lvol0, rvol = rvol0;
        for (int i = 0; i < frames; ++i) {
            if (ducked) {
                const int32_t g = duck0 + (duck1 - duck0) * i / frames;
                lvol = (lvol0 * g) >> 8;
                rvol = (rvol0 * g) >> 8;
            }
            size_t idx = static_cast<size_t>(v.pos_fx >> 32);
            if (idx >= v.length) {
                if (!v.loop) { v.active = false; break; }
                idx = v.loop_start;
                v.pos_fx = static_cast<uint64_t>(v.loop_start) << 32;
            }
            if (v.loop && v.loop_end > v.loop_start && idx >= v.loop_end) {
                idx = v.loop_start + (idx - v.loop_end) % (v.loop_end - v.loop_start);
                v.pos_fx = static_cast<uint64_t>(idx) << 32;
            }
            const int32_t s = v.pcm[idx];
            int32_t l = out[i * 2] + ((s * lvol) >> 8);
            int32_t r = out[i * 2 + 1] + ((s * rvol) >> 8);
            if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
            out[i * 2] = static_cast<int16_t>(l);
            out[i * 2 + 1] = static_cast<int16_t>(r);
            v.pos_fx += v.step_fx;
        }
    }
}

VoiceId Mixer::play(const Sample& sample, const VoiceParams& p, bool loop, int32_t loop_start,
                    int32_t loop_end, bool is_engine) {
    VoiceId id;
    if (device_ == 0 || !sample.valid()) return id;
    SDL_LockAudioDevice(device_);
    // The engine cap, which is the original's: three engines at once, however many cars are on
    // screen. Without it a full grid is a wall of drone and nothing else can be heard.
    if (is_engine) {
        int engines = 0;
        for (const Voice& v : voices_) if (v.active && v.engine) ++engines;
        if (engines >= kEngineVoices) { SDL_UnlockAudioDevice(device_); return id; }
    }
    int slot = -1;
    for (int i = 0; i < kVoices; ++i) {
        if (!voices_[i].active) { slot = i; break; }
    }
    if (slot < 0 && (loop || p.speech)) {
        // Every slot busy and this is a continuous voice: take the oldest one-shot. Losing the
        // tail of a bang is far less noticeable than the engine dropping out, which is exactly
        // what the user heard on a heavy impact.
        uint32_t oldest = 0xffffffffu;
        for (int i = 0; i < kVoices; ++i) {
            if (voices_[i].active && !voices_[i].loop && voices_[i].generation < oldest) {
                oldest = voices_[i].generation;
                slot = i;
            }
        }
    }
    if (slot < 0) { SDL_UnlockAudioDevice(device_); return id; } // every voice busy: drop it
    Voice& v = voices_[slot];
    v.pcm = sample.pcm.data();
    v.length = sample.pcm.size();
    v.pos_fx = 0;
    v.loop = loop;
    v.loop_start = (loop_start > 0 && static_cast<size_t>(loop_start) < v.length)
                       ? static_cast<size_t>(loop_start) : 0;
    v.loop_end = (loop_end > 0 && static_cast<size_t>(loop_end) <= v.length)
                     ? static_cast<size_t>(loop_end) : v.length;
    if (v.loop_end <= v.loop_start) { v.loop_start = 0; v.loop_end = v.length; }
    v.engine = is_engine;
    v.speech = p.speech;
    v.generation = next_generation_++;
    // ⚠️ THE PARAMETERS GO IN BEFORE `active`, UNDER THIS SAME LOCK. This used to set `active`,
    // release the lock and then call update() to fill in the rate - and between the two the
    // audio callback could run against a voice whose `step_fx` was 0 or left over from whatever
    // played in that slot before. A step of 0 never advances the read position, so the voice
    // sits on one sample of the waveform and drones for ever: a one-shot that never ends,
    // eating a slot and saturating the mix.
    apply(v, p);
    v.active = true;
    id.slot = slot;
    id.generation = v.generation;
    SDL_UnlockAudioDevice(device_);
    return id;
}

// The parameter half of update(), with the lock ALREADY HELD. Split out so play() can use it
// without releasing and re-taking the lock around a half-initialised voice.
void Mixer::apply(Voice& v, const VoiceParams& p) {
    int32_t rate = p.rate;
    if (rate < 1000) rate = 1000;
    if (rate > 96000) rate = 96000;
    v.step_fx = (static_cast<uint64_t>(rate) << 32) / static_cast<uint32_t>(out_rate_);
    const int32_t cap = v.speech ? kSpeechCap : kVolumeCap;
    v.volume = p.volume < 0 ? 0 : (p.volume > cap ? cap : p.volume);
    v.pan = p.pan < 0 ? 0 : (p.pan > 256 ? 256 : p.pan);
}

void Mixer::update(VoiceId id, const VoiceParams& p) {
    if (device_ == 0 || !id.valid()) return;
    SDL_LockAudioDevice(device_);
    Voice& v = voices_[id.slot];
    if (v.active && v.generation == id.generation) apply(v, p);
    SDL_UnlockAudioDevice(device_);
}

void Mixer::stop(VoiceId id) {
    if (device_ == 0 || !id.valid()) return;
    SDL_LockAudioDevice(device_);
    Voice& v = voices_[id.slot];
    if (v.active && v.generation == id.generation) v.active = false;
    SDL_UnlockAudioDevice(device_);
}

bool Mixer::playing(VoiceId id) const {
    if (device_ == 0 || !id.valid()) return false;
    const Voice& v = voices_[id.slot];
    return v.active && v.generation == id.generation;
}

void Mixer::stop_all() {
    if (device_ == 0) return;
    SDL_LockAudioDevice(device_);
    for (Voice& v : voices_) v.active = false;
    SDL_UnlockAudioDevice(device_);
}

void Mixer::set_music(MusicPlayer* music) {
    if (device_ == 0) { music_ = music; return; }
    // Under the lock: the callback may be walking this pointer right now.
    SDL_LockAudioDevice(device_);
    music_ = music;
    SDL_UnlockAudioDevice(device_);
}

int Mixer::active() const {
    int n = 0;
    for (const Voice& v : voices_) if (v.active) ++n;
    return n;
}

} // namespace bigred
