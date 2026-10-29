/* Inject a freestanding beep thread into SceShellUI, then DETACH so the UI
 * runs at full speed while the beep plays through the audio server.
 * Copyright (C) 2026 Desstann
 * SPDX-License-Identifier: MIT */
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <machine/reg.h>
#include <ps5/kernel.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
static int pt_getregs_lwp(pid_t pid, struct reg *r, int lwpid) {
    return sys_ptrace(PT_GETREGS, pid, (caddr_t)r, lwpid);
}
static int pt_setregs_lwp(pid_t pid, const struct reg *r, int lwpid) {
    return sys_ptrace(PT_SETREGS, pid, (caddr_t)r, lwpid);
}
static int pt_getregs(pid_t pid, struct reg *r) { return pt_getregs_lwp(pid, r, 0); }
static int pt_setregs(pid_t pid, const struct reg *r) { return pt_setregs_lwp(pid, r, 0); }
static int pt_continue(pid_t pid, int sig) {
    return sys_ptrace(PT_CONTINUE, pid, (caddr_t)1, sig) == -1 ? -1 : 0;
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
static int pt_step(pid_t pid) {
    return sys_ptrace(PT_STEP, pid, (caddr_t)1, 0) || waitpid(pid, 0, 0) < 0 ? -1 : 0;
}

static intptr_t g_gate, g_trap;

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
static intptr_t pt_mmap(pid_t pid, size_t len, int prot, int flags) {
    return pt_syscall(pid, SYS_mmap, 0, len, (uint64_t)prot, (uint64_t)flags, (uint64_t)-1, 0);
}

static long pt_call_bp_lwp(pid_t pid, int lwpid, intptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4) {
    struct reg bak, jmp;
    if (!g_trap || pt_getregs_lwp(pid, &bak, lwpid))
        return -1;
    memcpy(&jmp, &bak, sizeof jmp);
    intptr_t rsp = (bak.r_rsp - 0x400) & ~0xfUL;
    rsp -= 8;
    if (pt_copyin(pid, &g_trap, rsp, sizeof g_trap))
        return -1;
    jmp.r_rsp = rsp;
    jmp.r_rip = fn;
    jmp.r_rdi = a0;
    jmp.r_rsi = a1;
    jmp.r_rdx = a2;
    jmp.r_rcx = a3;
    jmp.r_r8 = a4;
    if (pt_setregs_lwp(pid, &jmp, lwpid))
        return -1;
    for (int guard = 0; guard < 400; ++guard) {
        int status = 0;
        if (pt_continue(pid, 0) || waitpid(pid, &status, 0) < 0 ||
            pt_getregs_lwp(pid, &jmp, lwpid)) {
            pt_setregs_lwp(pid, &bak, lwpid);
            return -1;
        }
        if (jmp.r_rip == g_trap + 1 || jmp.r_rip == g_trap) {
            const long rax = jmp.r_rax;
            pt_setregs_lwp(pid, &bak, lwpid);
            return rax;
        }
        if (guard == 0) {
            LG("  bp stop rip=%lx\n", (unsigned long)jmp.r_rip);
            pt_setregs_lwp(pid, &bak, lwpid);
            return -1;
        }
    }
    pt_setregs_lwp(pid, &bak, lwpid);
    return -1;
}
static long pt_call_bp(pid_t pid, intptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4) {
    return pt_call_bp_lwp(pid, 0, fn, a0, a1, a2, a3, a4);
}

static int unlock_syscall_filter(pid_t pid) {
    const intptr_t kproc = kernel_get_proc(pid);
    intptr_t filter = 0;
    if (!kproc || kernel_copyout(kproc + 0x3e8, &filter, sizeof filter) < 0 || !filter)
        return -1;
    unsigned long zero = 0, all = (unsigned long)-1L;
    return kernel_copyin(&zero, filter + 0xf0, sizeof zero) ||
                   kernel_copyin(&all, filter + 0xf8, sizeof all)
               ? -1
               : 0;
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
    g_log = fopen("/data/ps5discord/party-beep-thread.log", "w");
    if (!g_log)
        return 1;

    const pid_t pid = find_pid("ScePartyDaemon");
    if (pid < 0) {
        LG("no ShellUI\n");
        return 1;
    }
    LG("PartyDaemon pid=%d blob=%u\n", pid, beep_shim_blob_len);
    unlock_syscall_filter(pid);

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
    const intptr_t getpid_fn = sym(pid, "getpid", kernel_libs);
    const intptr_t pthread_create_fn = sym(pid, "scePthreadCreate", kernel_libs);
    const intptr_t attr_init_fn = sym(pid, "scePthreadAttrInit", kernel_libs);
    for (int i = 0; kernel_libs[i] && !g_gate; ++i) {
        uint32_t h = 0;
        if (!kernel_dynlib_handle(pid, kernel_libs[i], &h) && h) {
            const intptr_t g = kernel_dynlib_resolve(pid, h, "HoLVWNanBBc");
            if (g)
                g_gate = g + 0xa;
        }
    }
    LG("open=%p out=%p create=%lx gate=%lx\n", (void *)imp.open, (void *)imp.output,
       (unsigned long)pthread_create_fn, (unsigned long)g_gate);
    if (!imp.open || !imp.output || !pthread_create_fn || !g_gate) {
        LG("missing symbols\n");
        return 1;
    }

    if (pt_attach(pid)) {
        LG("attach failed\n");
        return 1;
    }
    const size_t page = 0x1000;
    const intptr_t code = pt_mmap(pid, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    const intptr_t args = pt_mmap(pid, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    const intptr_t trap = pt_mmap(pid, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    LG("code=%lx args=%lx trap=%lx\n", (unsigned long)code, (unsigned long)args, (unsigned long)trap);
    if (code == -1 || args == -1 || trap == -1) {
        pt_detach(pid);
        return 1;
    }
    if (pt_copyin(pid, beep_shim_blob, code, beep_shim_blob_len) ||
        kernel_mprotect(pid, code, page, PROT_READ | PROT_EXEC)) {
        LG("load shim failed\n");
        pt_detach(pid);
        return 1;
    }
    const unsigned char int3 = 0xcc;
    if (pt_copyin(pid, &int3, trap, 1) || kernel_mprotect(pid, trap, page, PROT_READ | PROT_EXEC)) {
        LG("trap failed\n");
        pt_detach(pid);
        return 1;
    }
    g_trap = trap;

    if (getpid_fn) {
        const long who = pt_call_bp(pid, getpid_fn, 0, 0, 0, 0, 0);
        LG("getpid=%ld\n", who);
        if (who != pid) {
            pt_detach(pid);
            return 1;
        }
    }

    beep_args_t sa;
    memset(&sa, 0, sizeof sa);
    sa.imports = imp;
    if (pt_copyin(pid, &sa, args, sizeof sa)) {
        LG("args failed\n");
        pt_detach(pid);
        return 1;
    }

    const char name[] = "pscordbeep";
    const intptr_t name_addr = args + 0x280;
    const intptr_t thr_out = args + 0x2c0;
    const intptr_t attr_slot = args + 0x240;
    const int64_t zero = 0;
    pt_copyin(pid, name, name_addr, sizeof name);
    pt_copyin(pid, &zero, thr_out, sizeof zero);
    pt_copyin(pid, &zero, attr_slot, sizeof zero);

    int chosen = 0;
    intptr_t attr_arg = 0;
    const int nlwp = (int)sys_ptrace(PT_GETNUMLWPS, pid, 0, 0);
    int ids[24];
    const int nlist = nlwp > 24 ? 24 : (nlwp > 0 ? nlwp : 0);
    if (nlist)
        sys_ptrace(PT_GETLWPLIST, pid, (caddr_t)ids, nlist);
    const int ntry = nlist > 8 ? 8 : nlist;
    for (int i = 0; i < ntry; ++i) {
        if (!attr_init_fn)
            break;
        const long ai = pt_call_bp_lwp(pid, ids[i], attr_init_fn, attr_slot, 0, 0, 0, 0);
        LG("  lwp %d attrInit=%ld\n", ids[i], ai);
        if (ai == 0) {
            chosen = ids[i];
            attr_arg = attr_slot;
            break;
        }
    }
    if (!chosen) {
        LG("no usable lwp\n");
        pt_detach(pid);
        return 1;
    }

    const long cr = pt_call_bp_lwp(pid, chosen, pthread_create_fn, thr_out, (uint64_t)attr_arg,
                                   (uint64_t)code, (uint64_t)args, (uint64_t)name_addr);
    LG("scePthreadCreate=%ld\n", cr);
    if (cr != 0) {
        pt_detach(pid);
        return 1;
    }
    pt_detach(pid);
    LG("detached — beep thread should run freely for ~3s\n");
    sleep(4);

    if (pt_attach(pid) == 0) {
        beep_args_t out;
        memset(&out, 0, sizeof out);
        pt_copyout(pid, args, &out, sizeof out);
        LG("magic=0x%x open=0x%x port=%d grains=%u errors=%u last=%d done=%d\n", out.magic,
           (unsigned)out.open_rc, out.port, out.grains, out.errors, out.last_out, out.done);
        const int32_t one = 1;
        pt_copyin(pid, &one, args + offsetof(beep_args_t, stop), sizeof one);
        pt_detach(pid);
    }
    LG("done\n");
    fclose(g_log);
    return 0;
}
