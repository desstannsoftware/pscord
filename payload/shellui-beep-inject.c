/* ps5-discord - Force an audible beep from inside ShellUI / PartyDaemon.
 *
 * Round 1 (BGM in ShellUI) opened and wrote successfully but was inaudible.
 * This round tries MAIN + SetMainOutput inside ShellUI, then VOICE inside
 * PartyDaemon. Listen for a loud square wave (~1.5 s each).
 *
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    if (kernel_set_ucred_authid(me, 0x4800000000010003l) || kernel_set_ucred_caps(me, priv))
        return -1;
    const long ret = syscall(SYS_ptrace, req, pid, addr, data);
    kernel_set_ucred_authid(me, authid);
    kernel_set_ucred_caps(me, caps);
    return ret;
}

static int pt_attach(pid_t pid) {
    if (sys_ptrace(PT_ATTACH, pid, 0, 0) == -1)
        return -1;
    return waitpid(pid, 0, 0) == -1 ? -1 : 0;
}
static int pt_detach(pid_t pid) { return sys_ptrace(PT_DETACH, pid, 0, 0) == -1 ? -1 : 0; }
static int pt_getregs(pid_t pid, struct reg *r) { return sys_ptrace(PT_GETREGS, pid, (caddr_t)r, 0); }
static int pt_setregs(pid_t pid, const struct reg *r) {
    return sys_ptrace(PT_SETREGS, pid, (caddr_t)r, 0);
}
static int pt_continue(pid_t pid, int sig) {
    return sys_ptrace(PT_CONTINUE, pid, (caddr_t)1, sig) == -1 ? -1 : 0;
}
static int pt_copyin(pid_t pid, const void *buf, intptr_t addr, size_t len) {
    struct ptrace_io_desc io = {.piod_op = PIOD_WRITE_D, .piod_offs = (void *)addr,
                                .piod_addr = (void *)buf, .piod_len = len};
    return sys_ptrace(PT_IO, pid, (caddr_t)&io, 0);
}
static int pt_step(pid_t pid) {
    if (sys_ptrace(PT_STEP, pid, (caddr_t)1, 0))
        return -1;
    return waitpid(pid, 0, 0) < 0 ? -1 : 0;
}

static intptr_t g_gate;
static intptr_t g_trap;

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

static long pt_call_bp(pid_t pid, intptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4) {
    struct reg bak, jmp;
    if (!g_trap || pt_getregs(pid, &bak))
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
    if (pt_setregs(pid, &jmp))
        return -1;
    for (int guard = 0; guard < 800; ++guard) {
        int status = 0;
        if (pt_continue(pid, 0) || waitpid(pid, &status, 0) < 0 || pt_getregs(pid, &jmp)) {
            pt_setregs(pid, &bak);
            return -1;
        }
        if (jmp.r_rip == g_trap + 1 || jmp.r_rip == g_trap) {
            const long rax = jmp.r_rax;
            pt_setregs(pid, &bak);
            return rax;
        }
        if (guard == 0) {
            LG("  stop sig=%d rip=%lx\n", WIFSTOPPED(status) ? WSTOPSIG(status) : -1,
               (unsigned long)jmp.r_rip);
            pt_setregs(pid, &bak);
            return -1;
        }
    }
    pt_setregs(pid, &bak);
    return -1;
}

static long pt_call6(pid_t pid, intptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5) {
    struct reg bak, jmp;
    if (!g_trap || pt_getregs(pid, &bak))
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
    jmp.r_r9 = a5;
    if (pt_setregs(pid, &jmp))
        return -1;
    for (int guard = 0; guard < 800; ++guard) {
        int status = 0;
        if (pt_continue(pid, 0) || waitpid(pid, &status, 0) < 0 || pt_getregs(pid, &jmp)) {
            pt_setregs(pid, &bak);
            return -1;
        }
        if (jmp.r_rip == g_trap + 1 || jmp.r_rip == g_trap) {
            const long rax = jmp.r_rax;
            pt_setregs(pid, &bak);
            return rax;
        }
        if (guard == 0) {
            pt_setregs(pid, &bak);
            return -1;
        }
    }
    pt_setregs(pid, &bak);
    return -1;
}

static int unlock_syscall_filter(pid_t pid) {
    const intptr_t kproc = kernel_get_proc(pid);
    intptr_t filter = 0;
    if (!kproc || kernel_copyout(kproc + 0x3e8, &filter, sizeof filter) < 0 || !filter)
        return -1;
    unsigned long zero = 0, all = (unsigned long)-1L;
    return kernel_copyin(&zero, filter + 0xf0, sizeof zero) < 0 ||
                   kernel_copyin(&all, filter + 0xf8, sizeof all) < 0
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

static int setup_target(pid_t pid) {
    static const char *const kernel_libs[] = {"libkernel.sprx", "libkernel_sys.sprx",
                                              "libkernel_web.sprx", NULL};
    g_gate = 0;
    for (int i = 0; kernel_libs[i] && !g_gate; ++i) {
        uint32_t h = 0;
        if (!kernel_dynlib_handle(pid, kernel_libs[i], &h) && h) {
            const intptr_t g = kernel_dynlib_resolve(pid, h, "HoLVWNanBBc");
            if (g)
                g_gate = g + 0xa;
        }
    }
    if (!g_gate)
        return -1;
    if (pt_attach(pid))
        return -1;
    const size_t page = 0x1000;
    const intptr_t trap = pt_mmap(pid, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    if (trap == -1)
        return -1;
    const unsigned char int3 = 0xcc;
    if (pt_copyin(pid, &int3, trap, 1) || kernel_mprotect(pid, trap, page, PROT_READ | PROT_EXEC))
        return -1;
    g_trap = trap;
    const intptr_t getpid_fn = sym(pid, "getpid", kernel_libs);
    if (getpid_fn) {
        const long who = pt_call_bp(pid, getpid_fn, 0, 0, 0, 0, 0);
        LG("  getpid=%ld expect=%d\n", who, pid);
        if (who != pid)
            return -1;
    }
    return 0;
}

static int play_in(pid_t pid, const char *label, int port) {
    static const char *const audio_libs[] = {"libSceAudioOut.sprx", NULL};
    LG("# %s port=%d\n", label, port);
    unlock_syscall_filter(pid);
    if (setup_target(pid)) {
        LG("  setup failed\n");
        pt_detach(pid);
        return -1;
    }

    const intptr_t fn_init = sym(pid, "sceAudioOutInit", audio_libs);
    const intptr_t fn_open = sym(pid, "sceAudioOutOpen", audio_libs);
    const intptr_t fn_out = sym(pid, "sceAudioOutOutput", audio_libs);
    const intptr_t fn_close = sym(pid, "sceAudioOutClose", audio_libs);
    const intptr_t fn_vol = sym(pid, "sceAudioOutSetVolume", audio_libs);
    const intptr_t fn_down = sym(pid, "sceAudioOutSetVolumeDown", audio_libs);
    const intptr_t fn_main = sym(pid, "sceAudioOutSetMainOutput", audio_libs);
    const intptr_t fn_mod = sym(pid, "sceAudioOutChangeAppModuleState", audio_libs);
    LG("  syms open=%lx out=%lx main=%lx mod=%lx\n", (unsigned long)fn_open, (unsigned long)fn_out,
       (unsigned long)fn_main, (unsigned long)fn_mod);
    if (!fn_open || !fn_out || !fn_close) {
        pt_detach(pid);
        return -1;
    }

    const size_t page = 0x1000;
    const intptr_t buf = pt_mmap(pid, page * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    const intptr_t volp = pt_mmap(pid, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    if (buf == -1 || volp == -1) {
        LG("  mmap failed\n");
        pt_detach(pid);
        return -1;
    }

    if (fn_init)
        LG("  init=%ld\n", pt_call_bp(pid, fn_init, 0, 0, 0, 0, 0));
    if (fn_mod) {
        for (int st = 0; st <= 2; ++st)
            LG("  ChangeAppModuleState(%d)=%ld\n", st, pt_call_bp(pid, fn_mod, (uint64_t)st, 0, 0, 0, 0));
    }

    const long handle = pt_call6(pid, fn_open, 0xff, (uint64_t)port, 0, 256, 48000, 1);
    LG("  open=0x%lx\n", (unsigned long)handle);
    if (handle < 0) {
        pt_detach(pid);
        return -1;
    }

    if (fn_vol) {
        int volumes[8];
        for (int i = 0; i < 8; ++i)
            volumes[i] = 32768;
        pt_copyin(pid, volumes, volp, sizeof volumes);
        LG("  SetVolume=%ld\n", pt_call_bp(pid, fn_vol, (uint64_t)handle, 0xff, (uint64_t)volp, 0, 0));
    }
    if (fn_down)
        LG("  SetVolumeDown=%ld\n", pt_call_bp(pid, fn_down, (uint64_t)handle, 0, 0, 0, 0));
    if (fn_main)
        LG("  SetMainOutput=%ld\n", pt_call_bp(pid, fn_main, (uint64_t)handle, 0, 0, 0, 0));

    enum { kFrames = 256, kBlocks = 48000 / 256 * 3 / 2 }; /* 1.5 s */
    int16_t grain[kFrames * 2];
    int last = 0;
    for (int block = 0; block < kBlocks; ++block) {
        for (int i = 0; i < kFrames; ++i) {
            const int sample = block * kFrames + i;
            const int16_t v = ((sample / 20) & 1) ? 28000 : -28000;
            grain[i * 2] = v;
            grain[i * 2 + 1] = v;
        }
        pt_copyin(pid, grain, buf, sizeof grain);
        last = (int)pt_call_bp(pid, fn_out, (uint64_t)handle, (uint64_t)buf, 0, 0, 0);
    }
    LG("  last_output=%d close=%ld\n", last, pt_call_bp(pid, fn_close, (uint64_t)handle, 0, 0, 0, 0));
    pt_detach(pid);
    return 0;
}

int main(void) {
    g_log = fopen("/data/ps5discord/shellui-beep.log", "w");
    if (!g_log)
        return 1;

    const pid_t ui = find_pid("SceShellUI");
    const pid_t party = find_pid("ScePartyDaemon");
    LG("--- inject beep 2 ui=%d party=%d ---\n", ui, party);

    if (ui > 0) {
        play_in(ui, "ShellUI MAIN", 0);
        sleep(1);
        play_in(ui, "ShellUI VOICE", 2);
        sleep(1);
    }
    if (party > 0) {
        play_in(party, "PartyDaemon VOICE", 2);
        sleep(1);
        play_in(party, "PartyDaemon MAIN", 0);
    }

    LG("done\n");
    fclose(g_log);
    return 0;
}
