// ps5-discord - Voice channel listener: voice gateway, UDP media, DAVE and Opus.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>
#include <set>
#include <string>

namespace hui::audio {
class StreamRing;
}

namespace voice {

// What the main gateway hands over after a voice state update.
struct JoinInfo {
    std::string endpoint; // host[:port], without a scheme
    std::string token;
    std::string session_id;
    std::string server_id; // guild id
    std::string channel_id;
    std::string user_id;
};

enum class Phase { idle, connecting, connected, failed };

struct Status {
    Phase phase = Phase::idle;
    std::string text;
    bool e2ee = false;
    std::set<std::string> speaking; // users heard in the last moment, us included
    bool mic = false;                // the microphone is open
    std::string mic_error;           // why it is not
    bool muted = false;
};

class Engine {
public:
    static Engine& instance();

    // Decoded call audio goes here (48 kHz stereo); set before the first start().
    void set_output(hui::audio::StreamRing* ring);
    // Leaves any call in progress, then joins the one described by info.
    void start(const JoinInfo& info);
    void stop();
    // Stops sending the microphone without leaving the call.
    void set_muted(bool muted);
    Status status() const;

    struct Impl;

private:
    Engine();
    ~Engine();
    std::unique_ptr<Impl> impl_;
};

} // namespace voice
