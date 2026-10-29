/* Stream Discord PCM into ScePartyDaemon MAIN (audible on home / over games).
 *
 * Idle = no MAIN handle and no ptrace. Opening MAIN at deploy (even for a beep)
 * left the port open and crackled the home menu; sceAudioOutClose via ptrace
 * has hard-crashed the box — so we never close, we simply never open until
 * there is audible speech, then abandon the handle + detach on idle.
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
#include <sys/socket.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <machine/reg.h>
#include <ps5/kernel.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { kPort = 9191, kFrames = 256, kBytes = kFrames * 2 * sizeof(int16_t) };

static FILE *g_log;
#define LG(...) do { fprintf(g_log, __VA_ARGS__); fflush(g_log); fsync(fileno(g_log)); } while (0)

/* Orbis APIs return int32; error codes have the high bit set (e.g. 0x80260011).
 * Comparing as long (>=0) falsely accepts those as valid audio handles. */
static int sce_ok(long v) { return (int32_t)v >= 0; }
static int sce_handle_ok(long v) { return (int32_t)v > 0; }

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
    errno = 0;
    if (sys_ptrace(PT_ATTACH, pid, 0, 0) == -1) {
        LG("PT_ATTACH pid=%d errno=%d\n", pid, errno);
        return -1;
    }
    /* Never block forever in waitpid — a wedged target used to hang the pump
     * so bg-voice soft_pace filled up and Discord audio went silent outside. */
    for (int i = 0; i < 100; ++i) {
        int status = 0;
        const pid_t w = waitpid(pid, &status, WUNTRACED | WNOHANG);
        if (w == pid)
            return 0;
        struct reg probe;
        if (sys_ptrace(PT_GETREGS, pid, (caddr_t)&probe, 0) != -1)
            return 0;
        usleep(10 * 1000);
    }
    LG("attach settle failed pid=%d errno=%d\n", pid, errno);
    sys_ptrace(PT_DETACH, pid, 0, 0);
    return -1;
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
    if (sys_ptrace(PT_STEP, pid, (caddr_t)1, 0) == -1)
        return -1;
    for (int i = 0; i < 40; ++i) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG | WUNTRACED) == pid)
            return 0;
        usleep(5 * 1000);
    }
    return -1;
}

static intptr_t g_gate, g_trap;
static int wait_stop_ms(pid_t pid, int timeout_ms);

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

/* Wait until target stops after PT_CONTINUE, up to ~timeout_ms.
 * 0=stop, -1=error, -2=timeout. Do not probe GETREGS here — after CONTINUE
 * the process may still look stopped briefly and would false-trigger. */
static int wait_stop_ms(pid_t pid, int timeout_ms) {
    const int slice = 5;
    for (int waited = 0; waited < timeout_ms; waited += slice) {
        int status = 0;
        const pid_t w = waitpid(pid, &status, WNOHANG | WUNTRACED);
        if (w == pid)
            return 0;
        if (w < 0 && errno != ECHILD && errno != EINTR)
            return -1;
        usleep(slice * 1000);
    }
    return -2;
}

static long pt_call_bp_timeout(pid_t pid, intptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5, int use6, int timeout_ms) {
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
    if (use6)
        jmp.r_r9 = a5;
    if (pt_setregs(pid, &jmp))
        return -1;
    /* Cap wait: Open/Output can stall under a foreground game. */
    for (int guard = 0; guard < 40; ++guard) {
        if (pt_continue(pid, 0)) {
            pt_setregs(pid, &bak);
            return -1;
        }
        const int wr = wait_stop_ms(pid, timeout_ms);
        if (wr == -2) {
            pt_setregs(pid, &bak);
            kill(pid, SIGSTOP);
            wait_stop_ms(pid, 200);
            return -2;
        }
        if (wr < 0 || pt_getregs(pid, &jmp)) {
            pt_setregs(pid, &bak);
            return -1;
        }
        if (jmp.r_rip == g_trap + 1 || jmp.r_rip == g_trap) {
            const long rax = jmp.r_rax;
            pt_setregs(pid, &bak);
            return rax;
        }
        /* Intermediate stop — keep going toward the trap. */
    }
    pt_setregs(pid, &bak);
    return -2;
}

static long pt_call_bp(pid_t pid, intptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4) {
    /* Output can block on the mixer's buffer under a game; 80ms was too tight. */
    return pt_call_bp_timeout(pid, fn, a0, a1, a2, a3, a4, 0, 0, /*timeout_ms=*/200);
}

static long pt_call6(pid_t pid, intptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5) {
    return pt_call_bp_timeout(pid, fn, a0, a1, a2, a3, a4, a5, 1, /*timeout_ms=*/300);
}

/* Recv while PartyDaemon keeps running — never stop just to read the socket.
 * Returns 0 = grain, 1 = idle (no bytes for idle_ms), -1 = disconnect/error.
 * bg-voice skips silence; idle timeout closes MAIN so the menu stays quiet. */
static int recv_grain(int sock, void *buf, size_t need, int idle_ms) {
    size_t got = 0;
    int waited = 0;
    while (got < need) {
        struct pollfd p = {.fd = sock, .events = POLLIN};
        const int slice = 50;
        const int pr = poll(&p, 1, slice);
        if (pr < 0)
            return -1;
        if (pr == 0) {
            if (got == 0) {
                waited += slice;
                if (waited >= idle_ms)
                    return 1;
            }
            continue;
        }
        const ssize_t n = recv(sock, (char *)buf + got, need - got, 0);
        if (n <= 0)
            return -1;
        got += (size_t)n;
        waited = 0;
    }
    return 0;
}

static int stop_for_output(pid_t pid) {
    if (kill(pid, SIGSTOP) != 0)
        return -1;
    return wait_stop_ms(pid, 300) == 0 ? 0 : -1;
}

static pid_t find_pid(const char *name) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0))
        return -1;
    char *b = malloc(size);
    if (!b || sysctl(mib, 4, b, &size, NULL, 0)) {
        free(b);
        return -1;
    }
    pid_t found = -1;
    for (char *p = b; p < b + size;) {
        struct kinfo_proc *ki = (struct kinfo_proc *)p;
        p += ki->ki_structsize ? ki->ki_structsize : sizeof(*ki);
        if (ki->ki_pid > 0 && !strcmp(ki->ki_comm, name)) {
            found = ki->ki_pid;
            break;
        }
    }
    free(b);
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

static int connect_pump(const char *target_name) {
    char hello[64];
    const int hello_n =
        snprintf(hello, sizeof hello, "PUMP %s\n", target_name ? target_name : "unknown");
    if (hello_n <= 0 || hello_n >= (int)sizeof hello)
        return -1;
    for (int attempt = 0; attempt < 50; ++attempt) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kPort);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
            if (send(fd, hello, (size_t)hello_n, 0) == hello_n) {
                char ok[8];
                if (recv(fd, ok, sizeof ok, 0) >= 2 && ok[0] == 'O')
                    return fd;
            }
        }
        close(fd);
        usleep(100 * 1000);
    }
    return -1;
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

/* Only PartyDaemon. ShellUI is audible but STOPPING it freezes game launches.
 * VoiceAndAgent/DiscordDaemon accept Output but are silent on the TV mix. */
static const char *const kAudioTargets[] = {
    "ScePartyDaemon",
    NULL,
};

static void unstick_audio_targets(void) {
    /* CONT only — PT_DETACH from a non-tracer hangs on Orbis when another
     * payload still holds the attach (elfldr names us payload.elf, so old
     * pumps are easy to miss by name). */
    for (int i = 0; kAudioTargets[i]; ++i) {
        const pid_t p = find_pid(kAudioTargets[i]);
        if (p > 1)
            kill(p, SIGCONT);
    }
}

static pid_t read_bg_voice_pid(void) {
    FILE *f = fopen("/data/ps5discord/bg-voice.log", "r");
    if (!f)
        return -1;
    char line[512];
    pid_t last = -1;
    while (fgets(line, sizeof line, f)) {
        char *s = strstr(line, "pid=");
        if (s && strstr(line, "bg-voice")) {
            const int p = atoi(s + 4);
            if (p > 1)
                last = (pid_t)p;
        }
    }
    fclose(f);
    return last;
}

static void kill_other_pumps(void) {
    /* elfldr names us payload.elf — kill prior pumps, keep bg-voice. */
    const pid_t keep = read_bg_voice_pid();
    FILE *pf = fopen("/data/ps5discord/party-audio-pump.pid", "r");
    if (pf) {
        int old = 0;
        if (fscanf(pf, "%d", &old) == 1 && old > 1 && old != (int)getpid()) {
            LG("kill previous pump pid=%d = %d\n", old, kill((pid_t)old, SIGKILL));
        }
        fclose(pf);
    }
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0))
        return;
    char *buf = malloc(size);
    if (!buf || sysctl(mib, 4, buf, &size, NULL, 0)) {
        free(buf);
        return;
    }
    for (char *p = buf; p < buf + size;) {
        struct kinfo_proc *ki = (struct kinfo_proc *)p;
        p += ki->ki_structsize ? ki->ki_structsize : sizeof(*ki);
        if (ki->ki_pid <= 1 || ki->ki_pid == getpid())
            continue;
        if (!strncmp(ki->ki_comm, "party-audio", 11) ||
            (!strcmp(ki->ki_comm, "payload.elf") && ki->ki_pid != keep)) {
            LG("kill %s %d = %d (keep_bg=%d)\n", ki->ki_comm, ki->ki_pid,
               kill(ki->ki_pid, SIGKILL), (int)keep);
        }
    }
    free(buf);
    usleep(200 * 1000);
    unstick_audio_targets();
}

/* Ensure we are attached and stopped briefly for a remote call. */
static int ensure_stopped(pid_t pid, int *attached, int *stopped) {
    if (!*attached) {
        if (pt_attach(pid))
            return -2;
        *attached = 1;
        *stopped = 1;
        return 0;
    }
    if (!*stopped) {
        if (stop_for_output(pid))
            return -3;
        *stopped = 1;
    }
    return 0;
}

/* Drop our reference only — do NOT call sceAudioOutClose (ptrace Close panic'd). */
static void abandon_main(long *handle) { *handle = -1; }

static long open_main(pid_t pid, intptr_t fn_open, intptr_t fn_vol, intptr_t fn_main, intptr_t volp,
                      int *attached, int *stopped) {
    if (ensure_stopped(pid, attached, stopped) != 0)
        return -1;
    const long h = pt_call6(pid, fn_open, 0xff, 0 /*MAIN*/, 0, kFrames, 48000, 1);
    if (!sce_handle_ok(h))
        return h;
    if (fn_vol && volp != -1) {
        int vol[8];
        for (int i = 0; i < 8; ++i)
            vol[i] = 32768;
        pt_copyin(pid, vol, volp, sizeof vol);
        pt_call_bp(pid, fn_vol, (uint64_t)h, 0xff, (uint64_t)volp, 0, 0);
    }
    if (fn_main)
        pt_call_bp(pid, fn_main, (uint64_t)h, 0, 0, 0, 0);
    return h;
}

/* Play one grain with a SHORT stop window, then CONT immediately.
 * Staying stopped (old path) wedges PartyDaemon/ShellUI during game launches. */
static int play_grain(pid_t pid, long handle, intptr_t buf, intptr_t fn_out, const int16_t *grain,
                      int *attached, int *stopped, long *raw_rc) {
    if (raw_rc)
        *raw_rc = 0;
    const int er = ensure_stopped(pid, attached, stopped);
    if (er)
        return er;
    if (pt_copyin(pid, grain, buf, kBytes))
        return -4; /* copyin */
    const long rc = pt_call_bp(pid, fn_out, (uint64_t)handle, (uint64_t)buf, 0, 0, 0);
    if (raw_rc)
        *raw_rc = rc;
    if (pt_continue(pid, 0)) {
        *attached = 0;
        *stopped = 0;
        return -7; /* continue */
    }
    *stopped = 0;
    if (rc == -2)
        return -5; /* output timeout */
    if (!sce_ok(rc))
        return -6; /* output error */
    return (int)(int32_t)rc;
}

int main(void) {
    g_log = fopen("/data/ps5discord/party-audio-pump.log", "w");
    if (!g_log)
        return 1;
    {
        FILE *pf = fopen("/data/ps5discord/party-audio-pump.pid", "w");
        if (pf) {
            fprintf(pf, "%d\n", (int)getpid());
            fclose(pf);
        }
    }
    LG("party-audio-pump starting (ptrace, no periodic detach) pid=%d\n", (int)getpid());
    LG("kill_other_pumps begin\n");
    kill_other_pumps();
    LG("kill_other_pumps done\n");

    static const char *const kernel_libs[] = {"libkernel.sprx", "libkernel_sys.sprx",
                                              "libkernel_web.sprx", NULL};
    static const char *const audio_libs[] = {"libSceAudioOut.sprx", NULL};

    pid_t pid = -1;
    const char *target = NULL;
    intptr_t fn_init = 0, fn_open = 0, fn_out = 0, fn_vol = 0, fn_main = 0, getpid_fn = 0;
    intptr_t buf = -1, volp = -1;
    long handle = -1;
    int attached = 0, stopped = 0;
    const size_t page = 0x1000;

    for (int t = 0; kAudioTargets[t]; ++t) {
      /* PartyDaemon vanishes briefly while launching a game — wait, don't ptrace ShellUI. */
      for (int wait = 0; wait < 40 && find_pid(kAudioTargets[t]) < 0; ++wait) {
          LG("waiting for %s (%d)\n", kAudioTargets[t], wait);
          usleep(250 * 1000);
      }
      for (int attempt = 0; attempt < 4 && pid < 0; ++attempt) {
        const pid_t cand = find_pid(kAudioTargets[t]);
        if (cand < 0) {
            LG("missing %s\n", kAudioTargets[t]);
            break;
        }
        LG("try %s pid=%d attempt=%d\n", kAudioTargets[t], cand, attempt);
        kill(cand, SIGCONT);
        usleep(100 * 1000);
        unlock_syscall_filter(cand);
        if (pt_attach(cand) != 0) {
            LG("attach %s failed\n", kAudioTargets[t]);
            kill(cand, SIGCONT);
            continue;
        }

        g_gate = 0;
        g_trap = 0;
        for (int i = 0; kernel_libs[i] && !g_gate; ++i) {
            uint32_t h = 0;
            if (!kernel_dynlib_handle(cand, kernel_libs[i], &h) && h) {
                const intptr_t g = kernel_dynlib_resolve(cand, h, "HoLVWNanBBc");
                if (g)
                    g_gate = g + 0xa;
            }
        }
        fn_init = sym(cand, "sceAudioOutInit", audio_libs);
        fn_open = sym(cand, "sceAudioOutOpen", audio_libs);
        fn_out = sym(cand, "sceAudioOutOutput", audio_libs);
        fn_vol = sym(cand, "sceAudioOutSetVolume", audio_libs);
        fn_main = sym(cand, "sceAudioOutSetMainOutput", audio_libs);
        getpid_fn = sym(cand, "getpid", kernel_libs);
        if (!fn_open || !fn_out || !g_gate) {
            LG("missing symbols on %s (open=%p out=%p gate=%lx)\n", kAudioTargets[t],
               (void *)fn_open, (void *)fn_out, (unsigned long)g_gate);
            pt_detach(cand);
            kill(cand, SIGCONT);
            continue;
        }

        const intptr_t trap = pt_mmap(cand, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
        buf = pt_mmap(cand, page * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
        volp = pt_mmap(cand, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
        LG("mmap trap=%ld buf=%ld volp=%ld gate=%lx\n", (long)trap, (long)buf, (long)volp,
           (unsigned long)g_gate);
        const unsigned char int3 = 0xcc;
        if (trap == -1 || buf == -1 || volp == -1 || pt_copyin(cand, &int3, trap, 1) ||
            kernel_mprotect(cand, trap, page, PROT_READ | PROT_EXEC)) {
            LG("setup failed on %s — next target\n", kAudioTargets[t]);
            pt_detach(cand);
            kill(cand, SIGCONT);
            continue;
        }
        g_trap = trap;
        if (getpid_fn) {
            const long gp = pt_call_bp(cand, getpid_fn, 0, 0, 0, 0, 0);
            LG("getpid via bp=%ld expect=%d\n", gp, cand);
        }
        /* Init AudioOut lib only — do NOT open MAIN or beep (idle MAIN crackles menu). */
        if (fn_init)
            pt_call_bp(cand, fn_init, 0, 0, 0, 0, 0);

        pid = cand;
        target = kAudioTargets[t];
        attached = 1;
        stopped = 1;
        LG("using %s pid=%d (no MAIN until speech — quiet menu)\n", target, pid);
        /* ALWAYS detach after setup. Holding ptrace across a title launch hard-crashes. */
        if (attached) {
            pt_continue(cand, 0);
            pt_detach(cand);
            attached = 0;
            stopped = 0;
            LG("detached after setup (safe for app launch / quiet menu)\n");
        }
        break; /* attempt */
      } /* attempt */
      if (pid >= 0)
          break; /* target */
    }
    if (pid < 0 || !target || !fn_open || !fn_out) {
        LG("no usable audible audio target (PartyDaemon)\n");
        return 1;
    }
    LG("pumping target=%s idle (no MAIN until speech) attached=%d\n", target, attached);

    const int sock = connect_pump(target);
    if (sock < 0) {
        LG("cannot connect to bg-voice PUMP (rejected or down)\n");
        return 1;
    }
    LG("PUMP connected target=%s\n", target);

    uint64_t grains = 0, played = 0, dropped = 0, fails = 0;
    int16_t grain[kFrames * 2];
    for (;;) {
        const int rr = recv_grain(sock, grain, kBytes, /*idle_ms=*/200);
        if (rr < 0) {
            LG("stream ended grains=%llu played=%llu dropped=%llu fails=%llu\n",
               (unsigned long long)grains, (unsigned long long)played, (unsigned long long)dropped,
               (unsigned long long)fails);
            break;
        }
        if (rr == 1) {
            if (attached || sce_handle_ok(handle)) {
                abandon_main(&handle);
                if (attached) {
                    if (stopped)
                        pt_continue(pid, 0);
                    pt_detach(pid);
                    attached = 0;
                    stopped = 0;
                }
                LG("abandon MAIN + detach after idle (quiet menu)\n");
            }
            continue;
        }
        ++grains;
        int peak = 0;
        for (int i = 0; i < kFrames * 2; ++i) {
            const int a = grain[i] < 0 ? -grain[i] : grain[i];
            if (a > peak)
                peak = a;
        }
        if (grains <= 20 || (grains % 100) == 0)
            LG("grains=%llu played=%llu dropped=%llu fails=%llu peak=%d handle=0x%lx attached=%d\n",
               (unsigned long long)grains, (unsigned long long)played, (unsigned long long)dropped,
               (unsigned long long)fails, peak, (unsigned long)handle, attached);
        if (peak <= 64)
            continue;
        if (find_pid("ScePartyDaemon") != pid) {
            LG("PartyDaemon pid changed — exit for relaunch\n");
            abandon_main(&handle);
            if (attached) {
                if (stopped)
                    pt_continue(pid, 0);
                pt_detach(pid);
            }
            break;
        }
        /* Stay attached across speech grains (attach/detach every grain = crackle).
         * Re-open MAIN only when we actually have something to play. */
        if (!sce_handle_ok(handle)) {
            handle = open_main(pid, fn_open, fn_vol, fn_main, volp, &attached, &stopped);
            LG("reopen MAIN=0x%lx for speech\n", (unsigned long)handle);
            if (!sce_handle_ok(handle)) {
                ++fails;
                if (attached && stopped) {
                    pt_continue(pid, 0);
                    stopped = 0;
                }
                continue;
            }
            if (attached && stopped) {
                pt_continue(pid, 0);
                stopped = 0;
            }
        }
        long raw = 0;
        const int rc = play_grain(pid, handle, buf, fn_out, grain, &attached, &stopped, &raw);
        if (rc < 0) {
            ++fails;
            if (fails <= 15 || (fails % 25) == 0)
                LG("play failed step=%d raw=0x%lx fails=%llu\n", rc, (unsigned long)raw,
                   (unsigned long long)fails);
            if (fails >= 30) {
                LG("too many failures — abandon and exit\n");
                abandon_main(&handle);
                if (attached) {
                    if (stopped)
                        pt_continue(pid, 0);
                    pt_detach(pid);
                    attached = 0;
                }
                break;
            }
            continue;
        }
        fails = 0;
        ++played;
        if (played <= 15 || (played % 100) == 0)
            LG("played=%llu peak=%d out_rc=%d\n", (unsigned long long)played, peak, rc);
    }
    abandon_main(&handle);
    if (attached) {
        if (stopped)
            pt_continue(pid, 0);
        pt_detach(pid);
    }
    kill(pid, SIGCONT);
    close(sock);
    LG("done\n");
    fclose(g_log);
    return 0;
}
