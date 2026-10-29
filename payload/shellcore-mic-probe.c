/* ps5-discord - Development payload: the decisive viability test for background
 * voice. SceShellCore is a system process that stays alive across game launches
 * and already has libSceAudioIn mapped. We ptrace-attach to it and call
 * sceAudioInOpen *inside* its address space. If it returns a valid handle (not
 * 0x809b0081, which our own process gets), then hosting the voice engine there
 * gives us a microphone that survives leaving the app.
 * Copyright (C) 2026 Desstann
 * SPDX-License-Identifier: MIT */
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <machine/reg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ps5/kernel.h>

int kernel_dynlib_handle(pid_t pid, const char *basename, uint32_t *handle);
intptr_t kernel_dynlib_dlsym(pid_t pid, uint32_t handle, const char *sym);

int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(int *user);

static FILE *g_log;
#define LG(...) do { fprintf(g_log, __VA_ARGS__); fflush(g_log); } while (0)

/* ptrace with temporary debug-authid escalation (ps5-payload-dev/websrv). */
static long sys_ptrace(int req, pid_t pid, caddr_t addr, int data) {
    uint8_t priv[16]; memset(priv, 0xff, sizeof priv);
    uint8_t caps[16];
    const pid_t me = getpid();
    const uint64_t authid = kernel_get_ucred_authid(me);
    if (!authid || kernel_get_ucred_caps(me, caps)) return -1;
    if (kernel_set_ucred_authid(me, 0x4800000000010003l)) return -1;
    if (kernel_set_ucred_caps(me, priv)) return -1;
    const long ret = syscall(SYS_ptrace, req, pid, addr, data);
    kernel_set_ucred_authid(me, authid);
    kernel_set_ucred_caps(me, caps);
    return ret;
}

static int pt_attach(pid_t pid) {
    if (sys_ptrace(PT_ATTACH, pid, 0, 0) == -1) return -1;
    return waitpid(pid, 0, 0) == -1 ? -1 : 0;
}
static int pt_detach(pid_t pid) { return sys_ptrace(PT_DETACH, pid, 0, 0) == -1 ? -1 : 0; }
static int pt_getregs(pid_t pid, struct reg *r) { return sys_ptrace(PT_GETREGS, pid, (caddr_t)r, 0); }
static int pt_setregs(pid_t pid, const struct reg *r) { return sys_ptrace(PT_SETREGS, pid, (caddr_t)r, 0); }
static int pt_step(pid_t pid) {
    if (sys_ptrace(PT_STEP, pid, (caddr_t)1, 0)) return -1;
    return waitpid(pid, 0, 0) < 0 ? -1 : 0;
}

/* Redirect a stopped thread to call addr(args...), single-step until it returns,
 * then restore the original registers. Returns rax. */
static long pt_call(pid_t pid, intptr_t addr,
                    uint64_t a0, uint64_t a1, uint64_t a2,
                    uint64_t a3, uint64_t a4, uint64_t a5) {
    struct reg bak, jmp;
    if (pt_getregs(pid, &bak)) return -1;
    memcpy(&jmp, &bak, sizeof jmp);
    jmp.r_rip = addr;
    jmp.r_rdi = a0; jmp.r_rsi = a1; jmp.r_rdx = a2;
    jmp.r_rcx = a3; jmp.r_r8 = a4; jmp.r_r9 = a5;
    if (pt_setregs(pid, &jmp)) return -1;
    while (jmp.r_rsp <= bak.r_rsp) {
        if (pt_step(pid)) { pt_setregs(pid, &bak); return -1; }
        if (pt_getregs(pid, &jmp)) { pt_setregs(pid, &bak); return -1; }
    }
    const long rax = jmp.r_rax;
    pt_setregs(pid, &bak);
    return rax;
}

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

static void probe(const char *proc_name) {
    const pid_t pid = find_pid(proc_name);
    if (pid < 0) { LG("%s: not running\n", proc_name); return; }

    uint32_t h_in = 0;
    if (kernel_dynlib_handle(pid, "libSceAudioIn.sprx", &h_in) || !h_in) {
        LG("%s (pid %d): no libSceAudioIn handle\n", proc_name, pid);
        return;
    }
    const intptr_t in_open = kernel_dynlib_dlsym(pid, h_in, "sceAudioInOpen");
    const intptr_t in_close = kernel_dynlib_dlsym(pid, h_in, "sceAudioInClose");
    if (!in_open) { LG("%s (pid %d): sceAudioInOpen not resolved\n", proc_name, pid); return; }

    int uid = -1;
    sceUserServiceGetForegroundUser(&uid);
    LG("%s (pid %d): attaching, foreground user=0x%x\n", proc_name, pid, uid);

    if (pt_attach(pid)) { LG("  attach failed\n"); return; }

    const int users[2] = {uid, 0xff};
    const int types[2] = {0, 1};
    for (int u = 0; u < 2; ++u) {
        for (int t = 0; t < 2; ++t) {
            const long h = pt_call(pid, in_open, (uint64_t)(uint32_t)users[u],
                                   (uint64_t)(uint32_t)types[t], 0, 256, 16000, 0);
            LG("  sceAudioInOpen(user=0x%x type=%d) = 0x%08lx\n",
               users[u], types[t], (unsigned long)(h & 0xffffffff));
            if (h >= 0 && in_close) pt_call(pid, in_close, (uint64_t)h, 0, 0, 0, 0, 0);
        }
    }

    pt_detach(pid);
    LG("  detached\n");
}

int main(void) {
    g_log = fopen("/data/ps5discord/shellcore-mic-probe.log", "w");
    if (!g_log) return 1;
    sceUserServiceInitialize(NULL);
    probe("SceShellCore");
    probe("SceVoiceAndAgent");
    probe("ScePartyDaemon");
    probe("SceAvCapture");
    fclose(g_log);
    return 0;
}
