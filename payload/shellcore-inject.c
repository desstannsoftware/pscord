/* ps5-discord - Stage 1 of the background-voice engine: inject the microphone
 * shim into SceShellCore as a real native thread (not single-stepped), let it
 * capture for a few seconds, then read back how many grains it got and the peak
 * level. This proves full-speed capture from inside a process that survives
 * leaving the app, and is the loader the final audio bridge will build on.
 * Copyright (C) 2026 Desstann
 * SPDX-License-Identifier: MIT */
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/thr.h>
#include <machine/reg.h>
#include <stddef.h>
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
/* fsync after every line: a hard ShellCore panic loses the OS page cache, so
 * fflush alone is not enough to tell us the last step before a crash. */
#define LG(...) do { fprintf(g_log, __VA_ARGS__); fflush(g_log); fsync(fileno(g_log)); } while (0)

/* ---- ptrace with temporary debug authid (ps5-payload-dev/websrv) -------- */
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
/* data is the LWP id; 0 means the thread ptrace stopped. */
static int pt_getregs_lwp(pid_t pid, struct reg *r, int lwpid) {
    return sys_ptrace(PT_GETREGS, pid, (caddr_t)r, lwpid);
}
static int pt_setregs_lwp(pid_t pid, const struct reg *r, int lwpid) {
    return sys_ptrace(PT_SETREGS, pid, (caddr_t)r, lwpid);
}
static int pt_getregs(pid_t pid, struct reg *r) { return pt_getregs_lwp(pid, r, 0); }
static int pt_setregs(pid_t pid, const struct reg *r) { return pt_setregs_lwp(pid, r, 0); }
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

static int pt_continue(pid_t pid, int sig) {
    return sys_ptrace(PT_CONTINUE, pid, (caddr_t)1, sig) == -1 ? -1 : 0;
}

/* Raw syscall inside the target through the libkernel syscall gate. Used only
 * for short, non-blocking syscalls (mmap); single-stepping these is fast and
 * proven. Heavy functions must use pt_call_bp instead. */
static intptr_t g_gate;
static long pt_syscall(pid_t pid, int sysno, uint64_t a0, uint64_t a1,
                       uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
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
static intptr_t pt_mmap(pid_t pid, size_t len, int prot, int flags) {
    return pt_syscall(pid, SYS_mmap, 0, len, (uint64_t)prot, (uint64_t)flags,
                      (uint64_t)-1, 0);
}

/* A private executable page in the target holding a single int3 (0xcc). Used as
 * the return trampoline for pt_call_bp so a finished call stops the whole
 * process on a breakpoint instead of us single-stepping through it. */
static intptr_t g_trap;

/* Call fn(args...) inside the target using breakpoint + PT_CONTINUE. Every
 * target thread runs at full speed (no single-stepping), so heavy, internally
 * blocking functions like scePthreadCreate complete normally and never wedge
 * the process. Returns rax, or -1 on failure. */
static long pt_call_bp_lwp(pid_t pid, int lwpid, intptr_t fn, uint64_t a0, uint64_t a1,
                           uint64_t a2, uint64_t a3, uint64_t a4) {
    struct reg bak, jmp;
    if (!g_trap || pt_getregs_lwp(pid, &bak, lwpid)) return -1;
    memcpy(&jmp, &bak, sizeof jmp);

    /* Fresh 16-byte-aligned frame well below the current stack; push the trap
     * address as the return address the SysV ABI expects at [rsp]. */
    intptr_t rsp = (bak.r_rsp - 0x400) & ~0xfUL;
    rsp -= 8;
    if (pt_copyin(pid, &g_trap, rsp, sizeof g_trap)) return -1;

    jmp.r_rsp = rsp;
    jmp.r_rip = fn;
    jmp.r_rdi = a0; jmp.r_rsi = a1; jmp.r_rdx = a2; jmp.r_rcx = a3; jmp.r_r8 = a4;
    if (pt_setregs_lwp(pid, &jmp, lwpid)) return -1;

    int logged = 0;
    for (int guard = 0; guard < 400; ++guard) {
        int status = 0;
        if (pt_continue(pid, 0)) { pt_setregs_lwp(pid, &bak, lwpid); return -1; }
        if (waitpid(pid, &status, 0) < 0) { pt_setregs_lwp(pid, &bak, lwpid); return -1; }
        if (pt_getregs_lwp(pid, &jmp, lwpid)) { pt_setregs_lwp(pid, &bak, lwpid); return -1; }
        /* int3 advances rip by one past the trap byte. */
        if (jmp.r_rip == g_trap + 1 || jmp.r_rip == g_trap) {
            const long rax = jmp.r_rax;
            pt_setregs_lwp(pid, &bak, lwpid);
            return rax;
        }
        /* Unexpected stop: on the first one, dump the faulting instruction and
         * registers so we can see exactly which dereference blew up. */
        if (g_log && logged < 1) {
            const int sig = WIFSTOPPED(status) ? WSTOPSIG(status) : -1;
            unsigned char code16[16] = {0};
            pt_copyout(pid, jmp.r_rip, code16, sizeof code16);
            fprintf(g_log, "  bp: sig=%d rip=%lx bytes=", sig, (unsigned long)jmp.r_rip);
            for (int i = 0; i < 16; ++i) fprintf(g_log, "%02x", code16[i]);
            fprintf(g_log,
                "\n  regs: rax=%lx rbx=%lx rcx=%lx rdx=%lx rsi=%lx rdi=%lx\n"
                "        rbp=%lx rsp=%lx r8=%lx r12=%lx r13=%lx r14=%lx r15=%lx fs=%lx\n",
                (unsigned long)jmp.r_rax, (unsigned long)jmp.r_rbx, (unsigned long)jmp.r_rcx,
                (unsigned long)jmp.r_rdx, (unsigned long)jmp.r_rsi, (unsigned long)jmp.r_rdi,
                (unsigned long)jmp.r_rbp, (unsigned long)jmp.r_rsp, (unsigned long)jmp.r_r8,
                (unsigned long)jmp.r_r12, (unsigned long)jmp.r_r13, (unsigned long)jmp.r_r14,
                (unsigned long)jmp.r_r15, (unsigned long)jmp.r_fs);
            fflush(g_log);
            fsync(fileno(g_log));
            ++logged;
            /* A fault inside the call is not a retry. Restoring and returning
             * keeps ShellCore stopped only for this one attempt. */
            pt_setregs_lwp(pid, &bak, lwpid);
            return -1;
        }
    }
    pt_setregs_lwp(pid, &bak, lwpid);
    return -1;
}

static long pt_call_bp(pid_t pid, intptr_t fn, uint64_t a0, uint64_t a1,
                       uint64_t a2, uint64_t a3, uint64_t a4) {
    return pt_call_bp_lwp(pid, 0, fn, a0, a1, a2, a3, a4);
}

/* ---- target symbol resolution ------------------------------------------ */
/* Clear SceShellCore's per-process syscall allow-list so thr_new / scePthreadCreate
 * and subsequent syscalls from an injected thread are not rejected. Same offsets
 * the public payload SDK and ps5debug-NG use (kproc+0x3e8 -> filter; +0xf0/+0xf8). */
static int unlock_syscall_filter(pid_t pid) {
    const intptr_t kproc = kernel_get_proc(pid);
    intptr_t filter = 0;
    if (!kproc) return -1;
    if (kernel_copyout(kproc + 0x3e8, &filter, sizeof filter) < 0 || !filter) return -1;
    unsigned long zero = 0;
    unsigned long all = (unsigned long)-1L;
    if (kernel_copyin(&zero, filter + 0xf0, sizeof zero) < 0) return -1;
    if (kernel_copyin(&all, filter + 0xf8, sizeof all) < 0) return -1;
    return 0;
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

/* Resolve a symbol by name across several candidate libraries in the target. */
static intptr_t sym(pid_t pid, const char *sym_name, const char *const *libs) {
    for (int i = 0; libs[i]; ++i) {
        uint32_t h = 0;
        if (kernel_dynlib_handle(pid, libs[i], &h) || !h) continue;
        const intptr_t a = kernel_dynlib_dlsym(pid, h, sym_name);
        if (a) return a;
    }
    return 0;
}

int main(void) {
    g_log = fopen("/data/ps5discord/shellcore-inject.log", "w");
    if (!g_log) return 1;
    sceUserServiceInitialize(NULL);

    const pid_t pid = find_pid("SceShellCore");
    if (pid < 0) { LG("SceShellCore not running\n"); return 1; }
    LG("SceShellCore pid=%d blob=%u bytes\n", pid, mic_shim_blob_len);

    if (unlock_syscall_filter(pid) != 0) {
        LG("syscall_filter unlock failed (FW offset mismatch?); continuing anyway\n");
    } else {
        LG("syscall_filter unlocked\n");
    }

    static const char *const kernel_libs[] = {
        "libkernel.sprx", "libkernel_sys.sprx", "libkernel_web.sprx", NULL};
    static const char *const audioin_libs[] = {"libSceAudioIn.sprx", NULL};

    shim_imports_t imp = {0};
    imp.audio_in_open  = (void *)sym(pid, "sceAudioInOpen", audioin_libs);
    imp.audio_in_input = (void *)sym(pid, "sceAudioInInput", audioin_libs);
    imp.audio_in_close = (void *)sym(pid, "sceAudioInClose", audioin_libs);
    imp.usleep         = (void *)sym(pid, "sceKernelUsleep", kernel_libs);
    const intptr_t getpid_fn = sym(pid, "getpid", kernel_libs);
    const intptr_t pthread_create_fn = sym(pid, "scePthreadCreate", kernel_libs);
    const intptr_t attr_init_fn      = sym(pid, "scePthreadAttrInit", kernel_libs);

    /* syscall gate for pt_syscall: NID of the libkernel __syscall thunk. */
    for (int i = 0; kernel_libs[i] && !g_gate; ++i) {
        uint32_t h = 0;
        if (!kernel_dynlib_handle(pid, kernel_libs[i], &h) && h) {
            const intptr_t g = kernel_dynlib_resolve(pid, h, "HoLVWNanBBc");
            if (g) g_gate = g + 0xa;
        }
    }

    LG("resolved: open=%p input=%p close=%p usleep=%p getpid=%lx create=%lx gate=%lx\n",
       (void *)imp.audio_in_open, (void *)imp.audio_in_input, (void *)imp.audio_in_close,
       (void *)imp.usleep, (unsigned long)getpid_fn, (unsigned long)pthread_create_fn,
       (unsigned long)g_gate);
    if (!imp.audio_in_open || !imp.audio_in_input || !imp.audio_in_close ||
        !imp.usleep || !g_gate) {
        LG("missing symbols, aborting\n");
        return 1;
    }

    int uid = -1;
    sceUserServiceGetForegroundUser(&uid);

    if (pt_attach(pid)) { LG("attach failed\n"); return 1; }

    const size_t page = 0x1000;
    const intptr_t code = pt_mmap(pid, page, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS);
    const intptr_t args = pt_mmap(pid, page, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS);
    LG("code=%lx args=%lx\n", (unsigned long)code, (unsigned long)args);
    if (code == -1 || args == -1) { LG("mmap failed\n"); pt_detach(pid); return 1; }

    if (pt_copyin(pid, mic_shim_blob, code, mic_shim_blob_len) ||
        kernel_mprotect(pid, code, page, PROT_READ | PROT_EXEC)) {
        LG("loading shim failed\n"); pt_detach(pid); return 1;
    }

    /* Trap page: one int3, executable, for pt_call_bp's return trampoline. */
    const intptr_t trap = pt_mmap(pid, page, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS);
    if (trap == -1) { LG("trap mmap failed\n"); pt_detach(pid); return 1; }
    const unsigned char int3 = 0xcc;
    if (pt_copyin(pid, &int3, trap, 1) ||
        kernel_mprotect(pid, trap, page, PROT_READ | PROT_EXEC)) {
        LG("trap setup failed\n"); pt_detach(pid); return 1;
    }
    g_trap = trap;

    /* Self-test the breakpoint call path on a harmless function before we use
     * it on the heavy thread-creation call. getpid() in the target must return
     * the target's own pid. */
    if (getpid_fn) {
        const long who = pt_call_bp(pid, getpid_fn, 0, 0, 0, 0, 0);
        LG("self-test getpid() in target = %ld (expect %d)\n", who, pid);
        if (who != pid) {
            LG("breakpoint call path unverified, aborting before thread create\n");
            pt_detach(pid);
            return 1;
        }
    }

    /* Isolate the fault: does a heap-using leaf (malloc) survive the trampoline,
     * or is it anything beyond a bare syscall that breaks here? */
    const intptr_t malloc_fn = sym(pid, "malloc", kernel_libs);
    if (malloc_fn) {
        const long p = pt_call_bp(pid, malloc_fn, 0x40, 0, 0, 0, 0);
        LG("self-test malloc(0x40) = 0x%lx\n", (unsigned long)p);
    } else {
        LG("malloc unresolved\n");
    }

    shim_args_t sa; memset(&sa, 0, sizeof sa);
    sa.imports = imp;
    sa.foreground_user = uid;
    sa.probe_only = 1;   /* Stage A: run the thread without any library call. */
    if (pt_copyin(pid, &sa, args, sizeof sa)) {
        LG("args copyin failed\n"); pt_detach(pid); return 1;
    }

    /* Create the worker with scePthreadCreate executed on the hijacked thread
     * (a real ShellCore thread with a valid pthread context). With attr=NULL,
     * libthr allocates the stack and TLS and registers the new thread in its
     * own bookkeeping -- so it does not share the caller's pthread/dtv the way a
     * raw thr_new thread does, which is what paniced ShellCore. */
    if (!pthread_create_fn) { LG("scePthreadCreate unresolved\n"); pt_detach(pid); return 1; }

    const char name[] = "pscordmic";
    const intptr_t name_addr = args + 0x280;
    const intptr_t thr_out   = args + 0x2c0;   /* ScePthread handle out */
    const intptr_t attr_slot = args + 0x240;   /* ScePthreadAttr handle */
    const int64_t zero = 0;
    if (pt_copyin(pid, name, name_addr, sizeof name) ||
        pt_copyin(pid, &zero, thr_out, sizeof zero) ||
        pt_copyin(pid, &zero, attr_slot, sizeof zero)) {
        LG("name/handle copyin failed\n"); pt_detach(pid); return 1;
    }

    /* The thread ptrace stops by default is often inside libthr and faults if
     * we re-enter it. Walk the LWP list and call scePthreadAttrInit on each
     * until one returns 0; that thread has a usable pthread context. Faults
     * are caught and the original registers restored, so ShellCore stays up. */
    int chosen = 0;
    intptr_t attr_arg = 0;
    const int nlwp = (int)sys_ptrace(PT_GETNUMLWPS, pid, 0, 0);
    int ids[24];
    const int nlist = nlwp > 24 ? 24 : (nlwp > 0 ? nlwp : 0);
    LG("lwps=%d listing=%d\n", nlwp, nlist);
    if (nlist && sys_ptrace(PT_GETLWPLIST, pid, (caddr_t)ids, nlist) < 0) {
        LG("lwplist failed\n");
    }
    const int ntry = nlist > 8 ? 8 : nlist;
    for (int i = 0; i < ntry; ++i) {
        struct ptrace_lwpinfo li;
        memset(&li, 0, sizeof li);
        li.pl_lwpid = ids[i];
        sys_ptrace(PT_LWPINFO, pid, (caddr_t)&li, sizeof li);
        if (!attr_init_fn) break;
        const long ai = pt_call_bp_lwp(pid, ids[i], attr_init_fn, attr_slot, 0, 0, 0, 0);
        LG("  lwp %d (%s) scePthreadAttrInit = %ld\n", ids[i], li.pl_tdname, ai);
        if (ai == 0) { chosen = ids[i]; attr_arg = attr_slot; break; }
    }
    if (!chosen) {
        LG("no thread accepted scePthreadAttrInit\n");
        pt_detach(pid);
        return 1;
    }

    LG("about to scePthreadCreate on lwp %d: entry=%lx arg=%lx name=%lx attr=%lx\n",
       chosen, (unsigned long)code, (unsigned long)args, (unsigned long)name_addr,
       (unsigned long)attr_arg);
    /* int scePthreadCreate(ScePthread*, attr, entry, arg, const char *name) */
    const long cr = pt_call_bp_lwp(pid, chosen, pthread_create_fn, thr_out, (uint64_t)attr_arg,
                                   (uint64_t)code, (uint64_t)args, (uint64_t)name_addr);
    int64_t thr_handle = 0;
    pt_copyout(pid, thr_out, &thr_handle, sizeof thr_handle);
    LG("scePthreadCreate = %ld (handle=0x%lx user=0x%x)\n",
       cr, (unsigned long)thr_handle, uid);
    if (cr != 0) {
        pt_detach(pid);
        LG("thread not created\n");
        return 1;
    }
    pt_detach(pid);

    sleep(4);

    if (pt_attach(pid)) { LG("re-attach failed\n"); return 1; }
    shim_args_t out; memset(&out, 0, sizeof out);
    pt_copyout(pid, args, &out, sizeof out);
    LG("after 4s: magic=0x%x mic_handle=0x%08x reads=%u errors=%u peak=%d last_error=0x%08x\n",
       out.magic, (unsigned)out.mic_handle, out.reads, out.errors, out.peak,
       (unsigned)out.last_error);

    const int32_t one = 1;
    pt_copyin(pid, &one, args + offsetof(shim_args_t, stop), sizeof one);
    pt_detach(pid);
    LG("asked shim to stop; done\n");

    fclose(g_log);
    return 0;
}
