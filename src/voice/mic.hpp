// ps5-discord - Microphone capture through the system's libSceAudioIn.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace voice {

// Captures mono S16 in 20 ms frames on its own thread.
class Mic {
public:
    ~Mic();

    // Opens the signed-in user's microphone.
    bool open(std::string& error);
    void close();

    int rate() const { return rate_; }
    int frame_samples() const { return rate_ / 50; }
    // Takes the oldest captured frame; false when none is waiting.
    bool pop(std::vector<int16_t>& frame);
    // sceAudioInGetSilentState: 0 while the microphone is really heard.
    int silent_state() const;

private:
    void run();

    int handle_ = -1;
    int rate_ = 0;
    int grain_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex lock_;
    std::deque<std::vector<int16_t>> frames_;
};

} // namespace voice
