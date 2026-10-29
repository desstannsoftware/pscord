/* Steal one PartyDaemon thread, run the beep shim freely, detach.
 * Proof that in-process MAIN audio is audible without holding ptrace.
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

#include "../inject/beep_shim_abi.h"
#include "../build/inject/beep_shim_blob.h"

static FILE *g_log;
#define LG(...) do { fprintf(g_log, __VA_ARGS__); fflush(g_log); fsync(fileno(g_log)); } while (0)

static long sys_ptrace(int req, pid_t pid, caddr_t addr, int data) {
    uint8_t priv[16];
    memset(priv, 0xff, sizeof priv);
    uint8_t caps[16];
    const pid_t me = getpid();
    const uint64_t authid = kernel_get_ucred_authid(me);
    if (!authid || kernel_get_ucred_caps(me, caps))
        return -1;
    kernel_set_ucred_authid(me, 0x4800000000010003l);
    kernel_set_ucred_caps(me, priv);
    const long ret = syscall(SYS_ptrace, req, pid, addr, data);
    kernel_set_ucred_authid(me, authid);
    kernel_set_ucred_caps(me, caps);
    return ret;
}

static int pt_attach(pid_t pid) {
    return sys_ptrace(PT_ATTACH, pid, 0, 0) == -1 || waitpid(pid, 0, 0) == -1 ? -1 : 0;
}
static int pt_detach(pid_t pid) { return sys_ptrace(PT_DETACH, pid, 0, 0) == -1 ? -1 : 0; }
static int pt_getregs(pid_t pid, struct reg *r) { return sys_ptrace(PT_GETREGS, pid, (caddr_t)r, 0); }
static int pt_setregs(pid_t pid, const struct reg *r) {
    return sys_ptrace(PT_SETREGS, pid, (caddr_t)r, 0);
}
static int pt_step(pid_t pid) {
    return sys_ptrace(PT_STEP, pid, (caddr_t)1, 0) || waitpid(pid, 0, 0) < 0 ? -1 : 0;
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
static long pt_syscall(pid_t pid, int sysno, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5) {
    struct reg bak, jmp;
    if (!g_gate || pt_getregs(pid, &bak))
        return -1;
    memcpy(&jmp, &bak, sizeof jmp);
    jmp.r_rip = g_gate;
    jmp.r_rax = sysno;
    jmp.r_rdi = a0;
    jmp.r_rsi = a1;
    jmp.r_rdx = a2;
    jmp.r_r10 = a3;
    jmp.r_r8 = a4;
    jmp.r_r9 = a5;
    if (pt_setregs(pid, &jmp))
        return -1;
    while (jmp.r_rsp <= bak.r_rsp) {
        if (pt_step(pid) || pt_getregs(pid, &jmp)) {
            pt_setregs(pid, &bak);
            return -1;
        }
    }
    const long rax = jmp.r_rax;
    pt_setregs(pid, &bak);
    return rax;
}

static pid_t find_pid(const char *name) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0))
        return -1;
    char *buf = malloc(size);
    if (!buf || sysctl(mib, 4, buf, &size, NULL, 0)) {
        free(buf);
        return -1;
    }
    pid_t found = -1;
    for (char *p = buf; p < buf + size;) {
        struct kinfo_proc *ki = (struct kinfo_proc *)p;
        p += ki->ki_structsize ? ki->ki_structsize : sizeof(*ki);
        if (ki->ki_pid > 0 && !strcmp(ki->ki_comm, name)) {
            found = ki->ki_pid;
            break;
        }
    }
    free(buf);
    return found;
}

static intptr_t sym(pid_t pid, const char *name, const char *const *libs) {
    for (int i = 0; libs[i]; ++i) {
        uint32_t h = 0;
        if (kernel_dynlib_handle(pid, libs[i], &h) || !h)
            continue;
        const intptr_t a = kernel_dynlib_dlsym(pid, h, name);
        if (a)
            return a;
    }
    return 0;
}

int main(void) {
    g_log = fopen("/data/ps5discord/party-beep-hijack.log", "w");
    if (!g_log)
        return 1;

    const pid_t pid = find_pid("ScePartyDaemon");
    if (pid < 0) {
        LG("no PartyDaemon\n");
        return 1;
    }

    static const char *const kernel_libs[] = {"libkernel.sprx", "libkernel_sys.sprx",
                                              "libkernel_web.sprx", NULL};
    static const char *const audio_libs[] = {"libSceAudioOut.sprx", NULL};
    beep_imports_t imp = {0};
    imp.init = (void *)sym(pid, "sceAudioOutInit", audio_libs);
    imp.open = (void *)sym(pid, "sceAudioOutOpen", audio_libs);
    imp.output = (void *)sym(pid, "sceAudioOutOutput", audio_libs);
    imp.close = (void *)sym(pid, "sceAudioOutClose", audio_libs);
    imp.set_volume = (void *)sym(pid, "sceAudioOutSetVolume", audio_libs);
    imp.set_volume_down = (void *)sym(pid, "sceAudioOutSetVolumeDown", audio_libs);
    imp.set_main = (void *)sym(pid, "sceAudioOutSetMainOutput", audio_libs);
    imp.usleep = (void *)sym(pid, "sceKernelUsleep", kernel_libs);
    for (int i = 0; kernel_libs[i] && !g_gate; ++i) {
        uint32_t h = 0;
        if (!kernel_dynlib_handle(pid, kernel_libs[i], &h) && h) {
            const intptr_t g = kernel_dynlib_resolve(pid, h, "HoLVWNanBBc");
            if (g)
                g_gate = g + 0xa;
        }
    }
    LG("pid=%d open=%p out=%p gate=%lx blob=%u\n", pid, (void *)imp.open, (void *)imp.output,
       (unsigned long)g_gate, beep_shim_blob_len);
    if (!imp.open || !imp.output || !g_gate) {
        LG("missing symbols\n");
        return 1;
    }

    if (pt_attach(pid)) {
        LG("attach failed\n");
        return 1;
    }
    const intptr_t code =
        pt_syscall(pid, SYS_mmap, 0, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                   (uint64_t)-1, 0);
    const intptr_t args =
        pt_syscall(pid, SYS_mmap, 0, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                   (uint64_t)-1, 0);
    const intptr_t stack =
        pt_syscall(pid, SYS_mmap, 0, 0x80000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                   (uint64_t)-1, 0);
    LG("code=%lx args=%lx stack=%lx\n", (unsigned long)code, (unsigned long)args,
       (unsigned long)stack);
    if (code == -1 || args == -1 || stack == -1) {
        pt_detach(pid);
        return 1;
    }
    if (pt_copyin(pid, beep_shim_blob, code, beep_shim_blob_len) ||
        kernel_mprotect(pid, code, 0x1000, PROT_READ | PROT_EXEC)) {
        LG("load failed\n");
        pt_detach(pid);
        return 1;
    }

    beep_args_t sa;
    memset(&sa, 0, sizeof sa);
    sa.imports = imp;
    if (pt_copyin(pid, &sa, args, sizeof sa)) {
        pt_detach(pid);
        return 1;
    }

    struct reg r;
    if (pt_getregs(pid, &r)) {
        pt_detach(pid);
        return 1;
    }
    LG("stealing rip=%lx rsp=%lx\n", (unsigned long)r.r_rip, (unsigned long)r.r_rsp);
    r.r_rip = (uint64_t)code;
    r.r_rdi = (uint64_t)args;
    r.r_rsp = ((stack + 0x80000) & ~0xful) - 8;
    /* Fake return address = park loop is inside shim; no return. */
    if (pt_setregs(pid, &r)) {
        pt_detach(pid);
        return 1;
    }
    pt_detach(pid);
    LG("detached — listen for ~2s beep\n");
    sleep(4);

    if (pt_attach(pid) == 0) {
        beep_args_t out;
        memset(&out, 0, sizeof out);
        pt_copyout(pid, args, &out, sizeof out);
        LG("magic=0x%x open=0x%x port=%d grains=%u errors=%u last=%d done=%d\n", out.magic,
           (unsigned)out.open_rc, out.port, out.grains, out.errors, out.last_out, out.done);
        pt_detach(pid);
    } else {
        LG("re-attach failed (PartyDaemon may have panicked)\n");
    }
    LG("done\n");
    fclose(g_log);
    return 0;
}
