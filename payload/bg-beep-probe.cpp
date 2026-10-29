/* ps5-discord - Force a payload beep onto the focused title's mix.
 *
 * Previous identity-only beeps opened ports but never reached the TV. This
 * round adopts the running eboot's credentials, attaches to it, tries
 * ChangeAppModuleState, and opens main/BGM/voice with the logged-in user.
 *
 * Sequence (each ~1.5 s, rising pitch):
 *   A  eboot creds + attach + main
 *   B  eboot + ChangeAppModuleState(1) + main
 *   C  eboot + BGM
 *   D  eboot + voice
 *   E  PartyDaemon creds + voice + attach eboot
 *
 * Copyright (C) 2026 Desstann
 * SPDX-License-Identifier: MIT */
#include <ps5/kernel.h>

#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
int sceAudioOutInit(void);
int sceAudioOutOpen(int user, int type, int index, unsigned grain, unsigned rate, unsigned format);
int sceAudioOutOutput(int handle, const void* samples);
int sceAudioOutClose(int handle);
int sceAudioOutSetVolume(int handle, int flag, int* volume);
int sceAudioOutSetVolumeDown(int handle, int volume);
int sceAudioOutGetPortState(int handle, void* state);
int sceAudioOutAttachToApplicationByPid(int pid);
int sceAudioOutDetachFromApplicationByPid(int pid);
int sceAudioOutGetFocusEnablePid(int* pid);
int sceAudioOutChangeAppModuleState(int state);
int sceAudioOutSetMainOutput(int handle);
int sceAudioOutSetChatBalance(int user, int balance);
int sceUserServiceInitialize(void*);
int sceUserServiceGetForegroundUser(int*);

typedef struct {
    uint32_t app_id;
    uint64_t unknown1;
    uint32_t app_type;
    char title_id[10];
    char unknown2[0x3c];
} app_info_t;
int sceKernelGetAppInfo(pid_t pid, app_info_t* info);
}

static FILE* g_log;

static void note(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

static int unlock_syscall_filter(pid_t pid) {
    const intptr_t kproc = kernel_get_proc(pid);
    intptr_t filter = 0;
    if (!kproc || kernel_copyout(kproc + 0x3e8, &filter, sizeof(filter)) < 0 || !filter) {
        return -1;
    }
    unsigned long zero = 0;
    unsigned long all = (unsigned long)-1L;
    if (kernel_copyin(&zero, filter + 0xf0, sizeof(zero)) < 0 ||
        kernel_copyin(&all, filter + 0xf8, sizeof(all)) < 0) {
        return -1;
    }
    return 0;
}

static pid_t find_comm(const char* name) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    if (sysctl(mib, 4, nullptr, &size, nullptr, 0) != 0) {
        return -1;
    }
    char* buf = (char*)std::malloc(size);
    if (!buf || sysctl(mib, 4, buf, &size, nullptr, 0) != 0) {
        std::free(buf);
        return -1;
    }
    pid_t found = -1;
    for (char* at = buf; at < buf + size;) {
        auto* ki = (struct kinfo_proc*)at;
        at += ki->ki_structsize ? ki->ki_structsize : sizeof(*ki);
        if (ki->ki_pid > 1 && std::strcmp(ki->ki_comm, name) == 0) {
            found = ki->ki_pid;
        }
    }
    std::free(buf);
    return found;
}

static pid_t find_eboot(char* title_out, size_t title_n) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    if (sysctl(mib, 4, nullptr, &size, nullptr, 0) != 0) {
        return -1;
    }
    char* buf = (char*)std::malloc(size);
    if (!buf || sysctl(mib, 4, buf, &size, nullptr, 0) != 0) {
        std::free(buf);
        return -1;
    }
    pid_t found = -1;
    title_out[0] = 0;
    for (char* at = buf; at < buf + size;) {
        auto* ki = (struct kinfo_proc*)at;
        at += ki->ki_structsize ? ki->ki_structsize : sizeof(*ki);
        if (ki->ki_pid <= 1 || std::strcmp(ki->ki_comm, "eboot.bin") != 0) {
            continue;
        }
        app_info_t info{};
        sceKernelGetAppInfo(ki->ki_pid, &info);
        note("  eboot pid=%d title=%.9s appid=%x type=%u", ki->ki_pid, info.title_id, info.app_id,
             info.app_type);
        found = ki->ki_pid;
        std::snprintf(title_out, title_n, "%.9s", info.title_id);
    }
    std::free(buf);
    return found;
}

static void adopt(pid_t donor) {
    const pid_t me = ::getpid();
    uint8_t caps[16]{};
    uint8_t attrs[32]{};
    kernel_get_ucred_caps(donor, caps);
    kernel_get_ucred_attrs(donor, attrs);
    note("  adopt pid=%d auth=%016lx", donor, (unsigned long)kernel_get_ucred_authid(donor));
    kernel_set_ucred_authid(me, kernel_get_ucred_authid(donor));
    kernel_set_ucred_caps(me, caps);
    kernel_set_ucred_attrs(me, attrs);
    kernel_set_ucred_prison(me, kernel_get_ucred_prison(donor));
}

static void beep(int handle, float hz, float seconds) {
    constexpr int kFrames = 256;
    constexpr int kRate = 48000;
    const int blocks = int(seconds * kRate / kFrames);
    int16_t grain[kFrames * 2];
    int last = 0;
    for (int block = 0; block < blocks; ++block) {
        for (int i = 0; i < kFrames; ++i) {
            const float t = float(block * kFrames + i) / float(kRate);
            // Near-full scale so a quiet mix is still obvious.
            const int16_t v = int16_t(20000.0f * std::sin(2.0f * 3.14159265f * hz * t));
            grain[i * 2] = v;
            grain[i * 2 + 1] = v;
        }
        last = sceAudioOutOutput(handle, grain);
    }
    note("  output rc=0x%08x", unsigned(last));
}

static void attempt(const char* label, float hz, int user, int port, pid_t attach_pid, int module_state) {
    note("# %s (%.0f Hz) user=%d port=%d attach=%d module=%d", label, hz, user, port, int(attach_pid),
         module_state);
    if (module_state >= 0) {
        note("  ChangeAppModuleState=%d", sceAudioOutChangeAppModuleState(module_state));
    }
    if (attach_pid > 1) {
        note("  Attach=%d", sceAudioOutAttachToApplicationByPid(attach_pid));
    }
    const int handle = sceAudioOutOpen(user, port, 0, 256, 48000, 1);
    note("  open=0x%08x", unsigned(handle));
    if (handle < 0) {
        return;
    }
    int volume[8];
    for (int& channel : volume) {
        channel = 32768;
    }
    note("  vol set=0x%08x down=0x%08x main=0x%08x", unsigned(sceAudioOutSetVolume(handle, 0xff, volume)),
         unsigned(sceAudioOutSetVolumeDown(handle, 0)), unsigned(sceAudioOutSetMainOutput(handle)));
    uint8_t state[32]{};
    note("  portstate=0x%08x", unsigned(sceAudioOutGetPortState(handle, state)));
    std::fprintf(g_log, "  state:");
    for (int i = 0; i < 16; ++i) {
        std::fprintf(g_log, " %02x", state[i]);
    }
    std::fputc('\n', g_log);
    std::fflush(g_log);
    beep(handle, hz, 1.5f);
    sceAudioOutClose(handle);
    if (attach_pid > 1) {
        sceAudioOutDetachFromApplicationByPid(attach_pid);
    }
    note("  closed");
    ::usleep(400 * 1000);
}

int main() {
    g_log = std::fopen("/data/ps5discord/beep-probe.log", "w");
    if (!g_log) {
        return 1;
    }
    const pid_t self = ::getpid();
    note("--- beep probe 4 pid=%d ---", int(self));
    note("unlock=%d", unlock_syscall_filter(self));
    sceUserServiceInitialize(nullptr);
    int user = 0xff;
    sceUserServiceGetForegroundUser(&user);
    note("foreground user=0x%08x", unsigned(user));
    int focus = 0;
    note("focus rc=0x%08x pid=%d", unsigned(sceAudioOutGetFocusEnablePid(&focus)), focus);

    char title[16]{};
    const pid_t eboot = find_eboot(title, sizeof title);
    const pid_t party = find_comm("ScePartyDaemon");
    const pid_t shell_ui = find_comm("SceShellUI");
    note("targets eboot=%d title=%s party=%d shellui=%d", int(eboot), title, int(party), int(shell_ui));

    note("init=0x%08x", unsigned(sceAudioOutInit()));

    if (eboot > 1) {
        adopt(eboot);
        const int u = user > 0 ? user : 0xff;
        const pid_t target = focus > 1 ? pid_t(focus) : eboot;
        attempt("A eboot+attach main", 440.0f, u, 0, target, -1);
        attempt("B eboot+module1 main", 554.0f, u, 0, target, 1);
        attempt("C eboot+bgm", 659.0f, u, 1, target, 1);
        attempt("D eboot+voice", 784.0f, u, 2, target, 1);
    } else {
        note("no eboot running — open PSCord or a game, then retry");
    }
    if (party > 1 && eboot > 1) {
        adopt(party);
        attempt("E party+voice+attach eboot", 988.0f, user > 0 ? user : 0xff, 2, eboot, 1);
    }
    if (shell_ui > 1) {
        adopt(shell_ui);
        attempt("F shellui+main+attach self-ui", 1175.0f, 0xff, 0, shell_ui, 1);
    }
    note("done");
    std::fclose(g_log);
    return 0;
}
