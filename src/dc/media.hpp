// ps5-discord - Pictures from the network: avatars, server icons, emoji,
// attachments and GIFs, fetched and decoded off the render thread.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace dc {

struct Picture {
    std::string url;
    int width = 0; // 0: the picture could not be fetched or decoded
    int height = 0;
    std::vector<std::vector<uint8_t>> frames; // RGBA, straight alpha
    std::vector<int> delays_ms;               // per frame; empty for a still picture
};

// Decodes PNG, JPEG or GIF (every frame) and shrinks it to fit max_w x max_h.
bool decode_picture(const std::string& bytes, int max_w, int max_h, Picture& out);

class Media {
public:
    static Media& instance();

    // Queues a download unless the URL was asked for before. Newest requests go first,
    // so what is on screen now arrives before what scrolled away.
    void request(const std::string& url, int max_w, int max_h);
    // One finished picture, failed ones included (width 0).
    bool take(Picture& out);
    // Lets a dropped picture be requested again.
    void forget(const std::string& url);

private:
    struct Job {
        std::string url;
        int max_w;
        int max_h;
    };
    void run();
    std::string fetch(const std::string& url);

    std::mutex lock_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::deque<Picture> done_;
    std::set<std::string> seen_;
    std::vector<std::thread> workers_;
};

} // namespace dc
