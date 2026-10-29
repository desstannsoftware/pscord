// ps5-discord - Background picture downloads over the same DPI-safe HTTPS path.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT

#include "dc/media.hpp"

#include <cstdio>

#include "dc/net/http.hpp"

namespace dc
{

namespace
{

constexpr int kWorkers = 3;
constexpr size_t kMaxQueue = 256;

} // namespace

Media &Media::instance()
{
    static Media media;
    return media;
}

void Media::request(const std::string &url, int max_w, int max_h)
{
    if (url.empty())
    {
        return;
    }
    std::lock_guard<std::mutex> hold(lock_);
    if (!seen_.insert(url).second)
    {
        return;
    }
    jobs_.push_front({url, max_w, max_h});
    // Requests that waited this long have scrolled away: let them be asked again.
    while (jobs_.size() > kMaxQueue)
    {
        seen_.erase(jobs_.back().url);
        jobs_.pop_back();
    }
    if (workers_.empty())
    {
        for (int i = 0; i < kWorkers; ++i)
        {
            workers_.emplace_back([this] { run(); });
        }
    }
    wake_.notify_one();
}

bool Media::take(Picture &out)
{
    std::lock_guard<std::mutex> hold(lock_);
    if (done_.empty())
    {
        return false;
    }
    out = std::move(done_.front());
    done_.pop_front();
    return true;
}

void Media::forget(const std::string &url)
{
    std::lock_guard<std::mutex> hold(lock_);
    seen_.erase(url);
}

std::string Media::fetch(const std::string &url)
{
    net::Response r =
        net::request("GET", url,
                     {{"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                                     "(KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36"},
                      {"Accept", "image/png,image/jpeg,image/gif,*/*;q=0.5"}});
    if (r.status != 200)
    {
        std::printf("[media] %s -> %d %s\n", url.c_str(), r.status, r.error.c_str());
        return {};
    }
    return std::move(r.body);
}

void Media::run()
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> hold(lock_);
            wake_.wait(hold, [this] { return !jobs_.empty(); });
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        Picture picture;
        picture.url = job.url;
        std::string bytes = fetch(job.url);
        if (bytes.empty() || !decode_picture(bytes, job.max_w, job.max_h, picture))
        {
            picture.width = picture.height = 0;
            picture.frames.clear();
        }
        std::lock_guard<std::mutex> hold(lock_);
        done_.push_back(std::move(picture));
    }
}

} // namespace dc
