/* Unstick ScePartyDaemon after a killed ptrace pump left it stopped. */
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <ps5/kernel.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

int main(void) {
    FILE *g = fopen("/data/ps5discord/party-cont.log", "w");
    if (!g)
        return 1;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0)) {
        fprintf(g, "sysctl size fail\n");
        fclose(g);
        return 1;
    }
    char buf[size];
    if (sysctl(mib, 4, buf, &size, NULL, 0)) {
        fprintf(g, "sysctl fail\n");
        fclose(g);
        return 1;
    }
    for (char *p = buf; p < buf + size;) {
        struct kinfo_proc *ki = (struct kinfo_proc *)p;
        p += ki->ki_structsize ? ki->ki_structsize : sizeof(*ki);
        if (ki->ki_pid <= 0 || strcmp(ki->ki_comm, "ScePartyDaemon"))
            continue;
        const pid_t pid = ki->ki_pid;
        fprintf(g, "PartyDaemon pid=%d\n", pid);
        fflush(g);
        /* Best-effort detach any prior tracer, then continue. */
        if (sys_ptrace(PT_ATTACH, pid, 0, 0) != -1) {
            waitpid(pid, 0, 0);
            sys_ptrace(PT_DETACH, pid, 0, 0);
            fprintf(g, "detached\n");
        } else {
            fprintf(g, "attach errno path — SIGCONT only\n");
        }
        if (kill(pid, SIGCONT) == 0)
            fprintf(g, "SIGCONT ok\n");
        else
            fprintf(g, "SIGCONT fail\n");
        fflush(g);
        fclose(g);
        return 0;
    }
    fprintf(g, "not found\n");
    fclose(g);
    return 1;
}
