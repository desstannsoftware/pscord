/* ps5-discord - Can our own process open the microphone once its credentials
 * match SceShellCore? If yes, a separate daemon can keep voice alive while the
 * app itself is suspended. No thread is injected into any system process.
 * Copyright (C) 2026 Desstann
 * SPDX-License-Identifier: MIT */
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ps5/kernel.h>

int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(int *user);
int sceAudioInInit(void);
int sceAudioInOpen(int user, uint32_t type, uint32_t index, uint32_t grain, uint32_t rate, uint32_t param);
int sceAudioInClose(int handle);

static FILE *g_log;
#define LG(...) do { fprintf(g_log, __VA_ARGS__); fflush(g_log); fsync(fileno(g_log)); } while (0)

static pid_t find_pid(const char *name) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0)) return -1;
    char *buf = malloc(size);
    if (!buf || sysctl(mib, 4, buf, &size, NULL, 0)) { free(buf); return -1; }
    pid_t found = -1;
    for (char *p = buf; p < buf + size;) {
        struct kinfo_proc *ki = (struct kinfo_proc *)p;
        p += ki->ki_structsize ? ki->ki_structsize : sizeof(*ki);
        if (ki->ki_pid > 0 && !strcmp(ki->ki_comm, name)) { found = ki->ki_pid; break; }
    }
    free(buf);
    return found;
}

static int try_open(int user, const char *label) {
    const int h = sceAudioInOpen(user, 0, 0, 256, 16000, 0);
    LG("open %s user=0x%x -> 0x%08x\n", label, user, (unsigned)h);
    if (h >= 0) sceAudioInClose(h);
    return h;
}

int main(void) {
    g_log = fopen("/data/ps5discord/mic-auth-probe.log", "w");
    if (!g_log) return 1;
    sceUserServiceInitialize(NULL);

    const pid_t me = getpid();
    const uint64_t mine = kernel_get_ucred_authid(me);
    const pid_t shell = find_pid("SceShellCore");
    const uint64_t shell_auth = shell > 0 ? kernel_get_ucred_authid(shell) : 0;
    uint8_t caps[16];
    memset(caps, 0, sizeof caps);
    kernel_get_ucred_caps(me, caps);
    LG("self pid=%d auth=0x%lx shellcore pid=%d auth=0x%lx\n",
       me, (unsigned long)mine, shell, (unsigned long)shell_auth);

    int user = -1;
    sceUserServiceGetForegroundUser(&user);
    LG("sceAudioInInit = 0x%08x\n", (unsigned)sceAudioInInit());
    try_open(user, "own-auth");

    uint8_t all[16];
    memset(all, 0xff, sizeof all);
    uint8_t my_attrs[32], shell_attrs[32];
    memset(my_attrs, 0, sizeof my_attrs);
    memset(shell_attrs, 0, sizeof shell_attrs);
    const intptr_t my_prison = kernel_get_ucred_prison(me);
    const intptr_t shell_prison = shell > 0 ? kernel_get_ucred_prison(shell) : 0;
    if (shell > 0) kernel_get_ucred_attrs(shell, shell_attrs);
    kernel_get_ucred_attrs(me, my_attrs);

    if (shell_auth && !kernel_set_ucred_authid(me, shell_auth) && !kernel_set_ucred_caps(me, all)) {
        try_open(user, "shellcore-auth");
        if (!kernel_set_ucred_attrs(me, shell_attrs))
            try_open(user, "shellcore-auth-attrs");
        if (shell_prison && !kernel_set_ucred_prison(me, shell_prison))
            try_open(user, "shellcore-auth-attrs-prison");
    } else {
        LG("could not adopt shellcore auth\n");
    }

    kernel_set_ucred_authid(me, mine);
    kernel_set_ucred_caps(me, caps);
    kernel_set_ucred_attrs(me, my_attrs);
    if (my_prison) kernel_set_ucred_prison(me, my_prison);
    LG("restored\n");
    fclose(g_log);
    return 0;
}
