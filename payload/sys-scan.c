/* ps5-discord - Development payload: list the running processes and, for each,
 * report whether libSceAudioIn / libSceAudioOut are already mapped and what the
 * process credentials are. This tells us which long-lived system process could
 * host the voice engine so it survives switching games.
 * Copyright (C) 2026 Desstann
 * SPDX-License-Identifier: MIT */
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ps5/kernel.h>

int kernel_dynlib_handle(pid_t pid, const char *basename, uint32_t *handle);

static int has_lib(pid_t pid, const char *name) {
    uint32_t handle = 0;
    return kernel_dynlib_handle(pid, name, &handle) == 0 && handle != 0;
}

int main(void) {
    FILE *log = fopen("/data/ps5discord/sys-scan.log", "w");
    if (!log) return 1;

    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0)) { fclose(log); return 1; }
    char *buf = malloc(size);
    if (!buf || sysctl(mib, 4, buf, &size, NULL, 0)) { free(buf); fclose(log); return 1; }

    static const char *st[] = {"", "START", "RUN", "SLEEP", "STOP", "ZOMB", "WAIT", "LOCK"};
    fprintf(log, "%6s  %-24s %-6s %4s %4s\n", "pid", "name", "state", "in", "out");
    for (char *p = buf; p < buf + size;) {
        struct kinfo_proc *ki = (struct kinfo_proc *)p;
        p += ki->ki_structsize ? ki->ki_structsize : sizeof(struct kinfo_proc);
        const pid_t pid = ki->ki_pid;
        if (pid <= 0) continue;
        const int in = has_lib(pid, "libSceAudioIn.sprx");
        const int out = has_lib(pid, "libSceAudioOut.sprx");
        const int s = (ki->ki_stat >= 0 && ki->ki_stat < 8) ? ki->ki_stat : 0;
        fprintf(log, "%6u  %-24s %-6s    %s    %s\n",
                pid, ki->ki_comm, st[s], in ? "Y" : "-", out ? "Y" : "-");
    }
    free(buf);
    fclose(log);
    return 0;
}
