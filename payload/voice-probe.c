/* ps5-discord - Can a background payload play sound and hear the microphone?
 * Plays a one second beep, then opens and reads the microphone, and writes
 * every result to /data/ps5discord/probe.log.
 * Copyright (C) 2026 Desstann
 * SPDX-License-Identifier: MIT */
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>
#include <stdlib.h>
#include <ps5/kernel.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int sceAudioOutInit(void);
int sceAudioOutOpen(int user, int type, int index, unsigned grain, unsigned rate, unsigned format);
int sceAudioOutOutput(int handle, const void *samples);
int sceAudioOutClose(int handle);
int sceAudioInOpen(int user, uint32_t type, uint32_t index, uint32_t grain, uint32_t rate, uint32_t param);
int sceAudioInInput(int handle, void *samples);
int sceAudioInClose(int handle);
int sceUserServiceInitialize(void *params);
int sceUserServiceGetInitialUser(int *user);
int sceUserServiceGetForegroundUser(int *user);
int sceUserServiceGetLoginUserIdList(int *list);
int sceAudioInInit(void);

static FILE *out;

typedef struct
{
    uint32_t app_id;
    uint64_t unknown1;
    uint32_t app_type;
    char title_id[10];
    char unknown2[0x3c];
} app_info_t;
int sceKernelGetAppInfo(pid_t pid, app_info_t *info);

static void note(const char *format, ...);
static pid_t find_title(const char *title)
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0))
        return -1;
    char *buf = malloc(size);
    if (!buf || sysctl(mib, 4, buf, &size, NULL, 0))
        return -1;
    pid_t found = -1;
    for (char *at = buf; at < buf + size;)
    {
        struct kinfo_proc *ki = (struct kinfo_proc *)at;
        at += ki->ki_structsize;
        app_info_t info;
        memset(&info, 0, sizeof info);
        sceKernelGetAppInfo(ki->ki_pid, &info);
        if (!strncmp(info.title_id, title, 9) || !strcmp(ki->ki_comm, "eboot.bin"))
        {
            note("candidate pid=%d comm=%s title=%.9s appid=%x", ki->ki_pid, ki->ki_comm, info.title_id, info.app_id);
            found = ki->ki_pid;
        }
    }
    free(buf);
    return found;
}

static void hex(const char *label, const uint8_t *bytes, int n)
{
    fprintf(out, "%s:", label);
    for (int i = 0; i < n; ++i)
        fprintf(out, " %02x", bytes[i]);
    fputc('\n', out);
    fflush(out);
}

static void note(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(out, format, args);
    va_end(args);
    fputc('\n', out);
    fflush(out);
}

int main(void)
{
    out = fopen("/data/ps5discord/probe.log", "w");
    if (!out)
        return 1;

    note("init=0x%08x", (unsigned)sceAudioOutInit());
    const int speaker = sceAudioOutOpen(0xff, 0, 0, 256, 48000, 1);
    note("audio out open=0x%08x", (unsigned)speaker);
    if (speaker >= 0)
    {
        int16_t grain[256 * 2];
        int result = 0;
        for (int block = 0; block < 48000 / 256; ++block)
        {
            for (int i = 0; i < 256; ++i)
            {
                const float t = (float)(block * 256 + i) / 48000.0f;
                const int16_t v = (int16_t)(8000.0f * sinf(2.0f * 3.14159f * 660.0f * t));
                grain[i * 2] = v;
                grain[i * 2 + 1] = v;
            }
            result = sceAudioOutOutput(speaker, grain);
        }
        note("audio out last output=0x%08x", (unsigned)result);
        sceAudioOutClose(speaker);
    }

    note("user service init=0x%08x", (unsigned)sceUserServiceInitialize(NULL));
    int user = -1;
    note("initial user=0x%08x result=0x%08x", (unsigned)user, (unsigned)sceUserServiceGetInitialUser(&user));
    note("initial user=0x%08x", (unsigned)user);
    int fg = -1;
    note("foreground result=0x%08x user=0x%08x", (unsigned)sceUserServiceGetForegroundUser(&fg), (unsigned)fg);
    int list[4] = {-1, -1, -1, -1};
    note("login list result=0x%08x %x %x %x %x", (unsigned)sceUserServiceGetLoginUserIdList(list), list[0], list[1], list[2],
         list[3]);
    if (user < 0)
        user = fg >= 0 ? fg : list[0];
    note("audio in init=0x%08x", (unsigned)sceAudioInInit());

    const pid_t self = getpid();
    pid_t app = find_title("PPSA99190");
    if (app <= 0)
        app = 265;
    note("self pid=%d authid=%016lx, app pid=%d authid=%016lx", self, kernel_get_ucred_authid(self), app,
         app > 0 ? kernel_get_ucred_authid(app) : 0);
    uint8_t caps[16], attrs[32];
    kernel_get_ucred_caps(self, caps);
    kernel_get_ucred_attrs(self, attrs);
    hex("self caps", caps, 16);
    hex("self attrs", attrs, 32);
    for (int stage = 0; stage < 2; ++stage)
    {
        if (stage == 1)
        {
            if (app <= 0)
                break;
            kernel_get_ucred_caps(app, caps);
            kernel_get_ucred_attrs(app, attrs);
            hex("app caps", caps, 16);
            hex("app attrs", attrs, 32);
            note("set authid=%d caps=%d attrs=%d", kernel_set_ucred_authid(self, kernel_get_ucred_authid(app)),
                 kernel_set_ucred_caps(self, caps), kernel_set_ucred_attrs(self, attrs));
        }
        const uint32_t types[] = {0, 1, 2, 5};
        for (unsigned t = 0; t < 4; ++t)
        {
            const int mic = sceAudioInOpen(user, types[t], 0, 256, t == 0 ? 16000 : 48000, 0);
            note("stage %d open type=%u = 0x%08x", stage, types[t], (unsigned)mic);
            if (mic < 0)
                continue;
            int16_t samples[256];
            double peak = 0;
            int got = 0;
            for (int i = 0; i < 150; ++i)
            {
                got = sceAudioInInput(mic, samples);
                for (int k = 0; k < 256; ++k)
                    peak = fmax(peak, fabs((double)samples[k]));
            }
            note("  input last=0x%08x peak=%.0f", (unsigned)got, peak);
            sceAudioInClose(mic);
        }
    }
    note("done");
    fclose(out);
    return 0;
}
