// ps5-homebrew-ui - Audio output thread through sceAudioOut.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "audio/mixer.hpp"

#include <atomic>
#include <cstdint>
#include <pthread.h>

namespace hui::ps5
{

// Opens the main 48 kHz stereo port (ProsperoLight's parameters: system
// user, 256-frame grains) and feeds it from the mixer on its own thread.
// sceAudioOutOutput blocks for each grain, which paces the thread.
class AudioOut
{
  public:
    static constexpr int kGrainFrames = 256;

    AudioOut() = default;
    AudioOut(const AudioOut &) = delete;
    AudioOut &operator=(const AudioOut &) = delete;
    ~AudioOut();

    // port_type: 0 main, 1 background music, 2 voice. Voice/BGM stay audible
    // while a game owns the main port. external is the payload path: it has no
    // title audio device of its own, so it attaches to whoever is focused.
    bool start(audio::Mixer &mixer, int port_type = 0, bool external = false);
    void stop();

    // Sees every rendered grain (interleaved s16 stereo) before it is output.
    // Set before start(); runs on the audio thread and must not block.
    using Tap = void (*)(const std::int16_t *grain, int frames, void *context);
    void set_tap(Tap tap, void *context)
    {
        tap_ = tap;
        tap_context_ = context;
    }

    // When true, skip sceAudioOutOutput and sleep one grain instead. Used while
    // the party pump owns the TV — another game's audio focus starves Output
    // into silence/glitches if we keep calling it.
    void set_soft_pace(bool on)
    {
        soft_pace_.store(on, std::memory_order_relaxed);
    }

    std::uint64_t grains() const
    {
        return grains_.load(std::memory_order_relaxed);
    }
    std::uint64_t errors() const
    {
        return errors_.load(std::memory_order_relaxed);
    }

  private:
    static void *thread_main(void *self);
    void run();

    audio::Mixer *mixer_ = nullptr;
    int handle_ = -1;
    pthread_t thread_{};
    bool running_ = false;
    std::atomic<bool> stop_{false};
    std::atomic<bool> soft_pace_{false};
    std::atomic<std::uint64_t> grains_{0};
    std::atomic<std::uint64_t> errors_{0};
    int port_ = -1;
    Tap tap_ = nullptr;
    void *tap_context_ = nullptr;
};

} // namespace hui::ps5
