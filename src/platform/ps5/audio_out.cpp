// ps5-homebrew-ui - Audio output thread through sceAudioOut.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/audio_out.hpp"

#include "platform/ps5/system.hpp"

#include <cmath>
#include <cstring>
#include <vector>

#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>

extern "C"
{
    int sceAudioOutInit(void);
    int sceAudioOutOpen(int user, int type, int index, unsigned int grain_frames,
                        unsigned int frequency, unsigned int format);
    int sceAudioOutOutput(int handle, const void *samples);
    int sceAudioOutClose(int handle);
    // A payload port is not on the TV mix until it is attached to the focused
    // application and its volume is raised. Both symbols live in libSceAudioOut.
    int sceAudioOutAttachToApplicationByPid(int pid);
    int sceAudioOutGetFocusEnablePid(int *pid);
    int sceAudioOutSetVolume(int handle, int flag, int *volume);
    int sceAudioOutSetVolumeDown(int handle, int volume);
    int sceUserServiceInitialize(void *params);
    int sceUserServiceGetForegroundUser(int *user);
}

namespace hui::ps5
{

namespace
{

constexpr int kAlreadyInitialized = static_cast<int>(0x8026000e);
constexpr int kSystemUser = 0xff;
constexpr unsigned int kFormatS16Stereo = 1;
constexpr int kVolume0dB = 32768;

int find_comm(const char *name)
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    if (sysctl(mib, 4, nullptr, &size, nullptr, 0) != 0 || size == 0)
        return -1;
    std::vector<char> buf(size);
    if (sysctl(mib, 4, buf.data(), &size, nullptr, 0) != 0)
        return -1;
    for (char *p = buf.data(); p < buf.data() + size;)
    {
        auto *info = reinterpret_cast<kinfo_proc *>(p);
        p += info->ki_structsize ? info->ki_structsize : sizeof(*info);
        if (info->ki_pid > 0 && std::strcmp(info->ki_comm, name) == 0)
            return info->ki_pid;
    }
    return -1;
}

void raise_volume(int handle)
{
    int volume[8];
    for (int &channel : volume)
        channel = kVolume0dB;
    const int set = sceAudioOutSetVolume(handle, 0xff, volume);
    const int down = sceAudioOutSetVolumeDown(handle, 0);
    sys::log("[HUI] audio volume set=0x%08x down=0x%08x", static_cast<unsigned>(set),
             static_cast<unsigned>(down));
}

void attach_focus()
{
    int focus = 0;
    const int focus_rc = sceAudioOutGetFocusEnablePid(&focus);
    int target = focus > 1 ? focus : find_comm("SceShellUI");
    if (target <= 1)
        target = find_comm("SceShellCore");
    const int attach = target > 1 ? sceAudioOutAttachToApplicationByPid(target) : -1;
    sys::log("[HUI] audio focus rc=0x%08x pid=%d attach pid=%d rc=0x%08x",
             static_cast<unsigned>(focus_rc), focus, target, static_cast<unsigned>(attach));
}

} // namespace

AudioOut::~AudioOut()
{
    stop();
}

bool AudioOut::start(audio::Mixer &mixer, int port_type, bool external)
{
    const int init = sceAudioOutInit();
    if (init != 0 && init != kAlreadyInitialized)
    {
        sys::log("[HUI] audio sceAudioOutInit=0x%08x", static_cast<unsigned>(init));
        return false;
    }
    int user = kSystemUser;
    if (external)
    {
        attach_focus();
        sceUserServiceInitialize(nullptr);
        sceUserServiceGetForegroundUser(&user);
    }
    const int users[] = {external && user > 0 ? user : kSystemUser, kSystemUser};
    const int attempts = external ? 2 : 1;
    handle_ = -1;
    for (int i = 0; i < attempts; ++i)
    {
        handle_ = sceAudioOutOpen(users[i], port_type, 0, kGrainFrames, audio::kSampleRate, kFormatS16Stereo);
        sys::log("[HUI] audio sceAudioOutOpen user=%d port=%d = 0x%08x", users[i], port_type,
                 static_cast<unsigned>(handle_));
        if (handle_ >= 0)
            break;
    }
    if (handle_ < 0)
    {
        handle_ = -1;
        return false;
    }
    port_ = port_type;
    raise_volume(handle_);
    mixer_ = &mixer;
    stop_.store(false);
    // Never name threads: pthread_setname_np hangs on this platform.
    if (pthread_create(&thread_, nullptr, &AudioOut::thread_main, this) != 0)
    {
        sys::log("[HUI] audio thread creation failed");
        sceAudioOutClose(handle_);
        handle_ = -1;
        return false;
    }
    running_ = true;
    sys::log("[HUI] audio open handle=%d grain=%d rate=%d", handle_, kGrainFrames,
             audio::kSampleRate);
    return true;
}

void *AudioOut::thread_main(void *self)
{
    static_cast<AudioOut *>(self)->run();
    return nullptr;
}

void AudioOut::run()
{
    alignas(64) std::int16_t grain[kGrainFrames * 2];
    int content_peak = 0;
    int last = 0;
    int since_log = 0;
    const int log_every = (audio::kSampleRate / kGrainFrames) * 3;
    while (!stop_.load(std::memory_order_relaxed))
    {
        mixer_->render(grain, kGrainFrames);
        int peak = 0;
        for (int i = 0; i < kGrainFrames * 2; ++i)
        {
            const int sample = grain[i] < 0 ? -grain[i] : grain[i];
            if (sample > peak)
                peak = sample;
        }
        if (peak > content_peak)
            content_peak = peak;
        if (tap_)
            tap_(grain, kGrainFrames, tap_context_);
        if (soft_pace_.load(std::memory_order_relaxed))
        {
            // ~5.33 ms at 48 kHz / 256 frames — keep the mix clock without HW.
            sys::sleep_us((kGrainFrames * 1000000) / audio::kSampleRate);
            last = 0;
            grains_.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            last = sceAudioOutOutput(handle_, grain);
            if (last < 0)
            {
                errors_.fetch_add(1, std::memory_order_relaxed);
                sys::sleep_us(5000);
            }
            else
                grains_.fetch_add(1, std::memory_order_relaxed);
        }
        if (++since_log >= log_every)
        {
            sys::log("[HUI] audio play port=%d grains=%llu errors=%llu peak=%d last=0x%08x", port_,
                     static_cast<unsigned long long>(grains_.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(errors_.load(std::memory_order_relaxed)), content_peak,
                     static_cast<unsigned>(last));
            content_peak = 0;
            since_log = 0;
        }
    }
    sceAudioOutOutput(handle_, nullptr); // drain the queued grain
}

void AudioOut::stop()
{
    if (running_)
    {
        stop_.store(true);
        pthread_join(thread_, nullptr);
        running_ = false;
    }
    if (handle_ >= 0)
        sceAudioOutClose(handle_);
    handle_ = -1;
}

} // namespace hui::ps5
