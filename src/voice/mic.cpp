// ps5-discord - Microphone capture through the system's libSceAudioIn.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
//
// Linked through tools/stubs/libSceAudioIn.c, as the SDK has no stub for it.

#include "voice/mic.hpp"
#include "i18n/i18n.hpp"

#include <unistd.h>

#include <cstdio>

extern "C"
{
    int sceAudioInOpen(int user, uint32_t type, uint32_t index, uint32_t grain, uint32_t rate,
                       uint32_t param);
    int sceAudioInInput(int handle, void *samples);
    int sceAudioInClose(int handle);
    __attribute__((weak)) int sceAudioInGetSilentState(int handle);
    int sceUserServiceGetInitialUser(int *user);
}

namespace voice
{

namespace
{

constexpr uint32_t kFormatS16Mono = 0;
constexpr int kSystemUser = 255;
constexpr size_t kMaxFrames = 25; // half a second waiting to be sent

} // namespace

Mic::~Mic()
{
    close();
}

bool Mic::open(std::string &error)
{
    int user = -1;
    sceUserServiceGetInitialUser(&user);

    // Voice chat input first (16 kHz), then general and raw input at 48 kHz.
    struct Mode
    {
        uint32_t type;
        uint32_t rate;
        uint32_t grain;
    };
    const Mode modes[] = {
        {0, 16000, 256}, {1, 48000, 256}, {2, 48000, 256}, {2, 16000, 256}, {1, 16000, 256}};
    for (const int who : {user, kSystemUser})
    {
        for (const Mode &mode : modes)
        {
            const int handle =
                sceAudioInOpen(who, mode.type, 0, mode.grain, mode.rate, kFormatS16Mono);
            std::printf("[voice] sceAudioInOpen(user=%d type=%u rate=%u) = 0x%08x\n", who,
                        mode.type, mode.rate, unsigned(handle));
            if (handle >= 0)
            {
                handle_ = handle;
                rate_ = int(mode.rate);
                grain_ = int(mode.grain);
                running_ = true;
                thread_ = std::thread([this] { run(); });
                return true;
            }
        }
    }
    error = i18n::T("Mikrofon bulunamadı");
    return false;
}

void Mic::close()
{
    running_ = false;
    if (handle_ >= 0)
    {
        sceAudioInClose(handle_);
    }
    if (thread_.joinable())
    {
        thread_.join();
    }
    handle_ = -1;
}

int Mic::silent_state() const
{
    // Weak: an import the console lacks resolves to null instead of crashing the call.
    return handle_ >= 0 && sceAudioInGetSilentState ? sceAudioInGetSilentState(handle_) : -1;
}

bool Mic::pop(std::vector<int16_t> &frame)
{
    std::lock_guard<std::mutex> hold(lock_);
    if (frames_.empty())
    {
        return false;
    }
    frame = std::move(frames_.front());
    frames_.pop_front();
    return true;
}

void Mic::run()
{
    std::vector<int16_t> grain(static_cast<size_t>(grain_));
    std::vector<int16_t> pending;
    const size_t frame = size_t(frame_samples());
    int failures = 0;
    while (running_)
    {
        const int got = sceAudioInInput(handle_, grain.data());
        if (got < 0)
        {
            if (++failures == 1)
            {
                std::printf("[voice] sceAudioInInput = 0x%08x\n", unsigned(got));
            }
            usleep(10000);
            continue;
        }
        pending.insert(pending.end(), grain.begin(), grain.end());
        while (pending.size() >= frame)
        {
            std::vector<int16_t> out(pending.begin(), pending.begin() + long(frame));
            pending.erase(pending.begin(), pending.begin() + long(frame));
            std::lock_guard<std::mutex> hold(lock_);
            if (frames_.size() == kMaxFrames)
            {
                frames_.pop_front();
            }
            frames_.push_back(std::move(out));
        }
    }
}

} // namespace voice
