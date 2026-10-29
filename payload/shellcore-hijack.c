/* ps5-discord - Run the mic shim on a ShellCore thread that already exists.
 * Creating a new thread panics the console. Calling sceAudioInOpen on one of
 * ShellCore's own threads returns a real handle. This payload points that
 * thread at the shim and lets it go; it does not create a thread.
 * Copyright (C) 2026 Desstann
 * SPDX-License-Identifier: MIT */
#include <sys/types.h>
#include <sys/mman.h>
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

#include "../inject/shim_abi.h"
#include "../build/inject/mic_shim_blob.h"

int kernel_dynlib_handle(pid_t pid, const char *basename, uint32_t *handle);
intptr_t kernel_dynlib_dlsym(pid_t pid, uint32_t handle, const char *sym);
intptr_t kernel_dynlib_resolve(pid_t pid, uint32_t handle, const char *nid);
int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(int *user);

static FILE *g_log;
#define LG(...) do { fprintf(g_log, __VA_ARGS__); fflush(g_log); fsync(fileno(g_log)); } while (0)

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
static int pt_copyin(pid_t pid, const void *buf, intptr_t addr, size_t len) {
    struct ptrace_io_desc io = {.piod_op = PIOD_WRITE_D, .piod_offs = (void *)addr,
                                .piod_addr = (void *)buf, .piod_len = len};
    return sys_ptrace(PT_IO, pid, (caddr_t)&io, 0);
}
static int pt_copyout(pid_t pid, intptr_t addr, void *buf, size_t len) {
    struct ptrace_io_desc io = {.piod_op = PIOD_READ_D, .piod_offs = (void *)addr,
                                .piod_addr = buf, .piod_len = len};
    return sys_ptrace(PT_IO, pid, (caddr_t)&io, 0);
}

static intptr_t g_gate;
static long pt_syscall(pid_t pid, int sysno, uint64_t a0, uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4, uint64_t a5) {
    struct reg bak, jmp;
    if (!g_gate || pt_getregs(pid, &bak)) return -1;
    memcpy(&jmp, &bak, sizeof jmp);
    jmp.r_rip = g_gate;
    jmp.r_rax = sysno;
    jmp.r_rdi = a0; jmp.r_rsi = a1; jmp.r_rdx = a2;
    jmp.r_r10 = a3; jmp.r_r8 = a4; jmp.r_r9 = a5;
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

static intptr_t sym(pid_t pid, const char *name, const char *const *libs) {
    for (int i = 0; libs[i]; ++i) {
        uint32_t h = 0;
        if (kernel_dynlib_handle(pid, libs[i], &h) || !h) continue;
        const intptr_t a = kernel_dynlib_dlsym(pid, h, name);
        if (a) return a;
    }
    return 0;
}

int main(void) {
    g_log = fopen("/data/ps5discord/shellcore-hijack.log", "w");
    if (!g_log) return 1;
    sceUserServiceInitialize(NULL);

    const pid_t pid = find_pid("SceShellCore");
    if (pid < 0) { LG("SceShellCore not running\n"); return 1; }

    static const char *const kernel_libs[] = {
        "libkernel.sprx", "libkernel_sys.sprx", "libkernel_web.sprx", NULL};
    static const char *const audioin_libs[] = {"libSceAudioIn.sprx", NULL};
    shim_imports_t imp = {0};
    imp.audio_in_open = (void *)sym(pid, "sceAudioInOpen", audioin_libs);
    imp.audio_in_input = (void *)sym(pid, "sceAudioInInput", audioin_libs);
    imp.audio_in_close = (void *)sym(pid, "sceAudioInClose", audioin_libs);
    imp.usleep = (void *)sym(pid, "sceKernelUsleep", kernel_libs);
    for (int i = 0; kernel_libs[i] && !g_gate; ++i) {
        uint32_t h = 0;
        if (!kernel_dynlib_handle(pid, kernel_libs[i], &h) && h) {
            const intptr_t g = kernel_dynlib_resolve(pid, h, "HoLVWNanBBc");
            if (g) g_gate = g + 0xa;
        }
    }
    LG("pid=%d open=%p usleep=%p gate=%lx\n", pid, (void *)imp.audio_in_open,
       (void *)imp.usleep, (unsigned long)g_gate);
    if (!imp.audio_in_open || !imp.audio_in_input || !imp.usleep || !g_gate) {
        LG("missing symbols\n");
        return 1;
    }

    int uid = -1;
    sceUserServiceGetForegroundUser(&uid);
    if (pt_attach(pid)) { LG("attach failed\n"); return 1; }

    const intptr_t code = pt_syscall(pid, SYS_mmap, 0, 0x1000, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, (uint64_t)-1, 0);
    const intptr_t args = pt_syscall(pid, SYS_mmap, 0, 0x1000, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, (uint64_t)-1, 0);
    const intptr_t stack = pt_syscall(pid, SYS_mmap, 0, 0x80000, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, (uint64_t)-1, 0);
    LG("code=%lx args=%lx stack=%lx\n", (unsigned long)code, (unsigned long)args, (unsigned long)stack);
    if (code == -1 || args == -1 || stack == -1) {
        LG("mmap failed\n");
        pt_detach(pid);
        return 1;
    }
    if (pt_copyin(pid, mic_shim_blob, code, mic_shim_blob_len) ||
        kernel_mprotect(pid, code, 0x1000, PROT_READ | PROT_EXEC)) {
        LG("shim load failed\n");
        pt_detach(pid);
        return 1;
    }

    shim_args_t sa;
    memset(&sa, 0, sizeof sa);
    sa.imports = imp;
    sa.foreground_user = uid;
    sa.probe_only = 0;
    if (pt_copyin(pid, &sa, args, sizeof sa)) {
        LG("args copy failed\n");
        pt_detach(pid);
        return 1;
    }

    struct reg r;
    if (pt_getregs(pid, &r)) { LG("getregs failed\n"); pt_detach(pid); return 1; }
    LG("stealing thread rip=%lx rsp=%lx\n", (unsigned long)r.r_rip, (unsigned long)r.r_rsp);
    r.r_rip = (uint64_t)code;
    r.r_rdi = (uint64_t)args;
    r.r_rsp = ((stack + 0x80000) & ~0xful) - 8;
    if (pt_setregs(pid, &r)) { LG("setregs failed\n"); pt_detach(pid); return 1; }
    pt_detach(pid);
    LG("detached, shim should be running\n");

    sleep(4);
    if (pt_attach(pid)) { LG("re-attach failed\n"); return 1; }
    shim_args_t out;
    memset(&out, 0, sizeof out);
    pt_copyout(pid, args, &out, sizeof out);
    LG("magic=0x%x handle=0x%08x reads=%u errors=%u peak=%d last=0x%08x\n",
       out.magic, (unsigned)out.mic_handle, out.reads, out.errors, out.peak, (unsigned)out.last_error);
    pt_detach(pid);
    LG("done\n");
    fclose(g_log);
    return 0;
}
