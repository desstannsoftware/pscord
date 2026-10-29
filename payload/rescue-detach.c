/* ps5-discord - Recovery tool: if an injector hangs while ptrace-stopping
 * SceShellCore, the UI freezes. A ptraced process is reparented to its tracer,
 * so SceShellCore's current parent is the stuck injector. Kill that parent and
 * the kernel auto-detaches, resuming SceShellCore.
 * Copyright (C) 2026 Desstann
 * SPDX-License-Identifier: MIT */
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    FILE *log = fopen("/data/ps5discord/rescue.log", "w");
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0)) return 1;
    char *buf = malloc(size);
    if (!buf || sysctl(mib, 4, buf, &size, NULL, 0)) return 1;

    /* A stuck injector sits in RUN while it single-steps the stopped target.
     * Kill every running homebrew payload except ourselves; the kernel then
     * auto-detaches ptrace and SceShellCore resumes. */
    const pid_t self = getpid();
    for (char *p = buf; p < buf + size;) {
        struct kinfo_proc *ki = (struct kinfo_proc *)p;
        p += ki->ki_structsize ? ki->ki_structsize : sizeof(*ki);
        if (ki->ki_pid <= 0 || ki->ki_pid == self) continue;
        if (ki->ki_stat != 2 /* RUN */) continue;
        if (strcmp(ki->ki_comm, "payload.elf")) continue;
        const int r = kill(ki->ki_pid, SIGKILL);
        if (log) fprintf(log, "kill(%d %s) = %d\n", ki->ki_pid, ki->ki_comm, r);
    }
    if (log) fclose(log);
    free(buf);
    return 0;
}
