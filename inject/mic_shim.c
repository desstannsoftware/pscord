/* ps5-discord - Position-independent shim that runs as a native thread inside
 * SceShellCore. It opens the microphone (which only a system process is allowed
 * to do) and tracks the peak level so the injector can confirm real capture at
 * full speed. This is the capture half of the eventual audio bridge; it calls
 * nothing but the function pointers handed to it, so it needs no relocation.
 * Copyright (C) 2026 Desstann
 * SPDX-License-Identifier: MIT */
#include "shim_abi.h"

#define MIC_GRAIN 256
#define MIC_RATE  16000

/* Spin forever without returning; a thr_new thread that returns would crash. */
static void park(const shim_imports_t *imp) {
    for (;;) imp->usleep(200000);
}

/* Raw FreeBSD syscall from the flat blob (no libc, no TLS needed). */
static inline long raw_syscall2(long n, long a0, long a1) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a0), "S"(a1)
                     : "rcx", "r11", "memory");
    return ret;
}
#define SYS_nanosleep_ 240
static inline void raw_sleep_ms(long ms) {
    long ts[2] = {ms / 1000, (ms % 1000) * 1000000L};
    raw_syscall2(SYS_nanosleep_, (long)ts, 0);
}

/* The only entry point. Must be the first thing in the blob (see shim.ld).
 * Started by thr_new, so it takes one void* arg and must never return. */
__attribute__((section(".text.entry"), used))
void shim_entry(void *arg) {
    shim_args_t *a = (shim_args_t *)arg;
    a->magic = SHIM_MAGIC;

    const shim_imports_t imp = a->imports;

    /* Stage A: prove the injected thread runs with valid TLS without calling
     * any library at all (a bad TLS would crash ShellCore on the first libc or
     * Sony call). Pure loop: only touch our own args page. */
    if (a->probe_only) {
        a->reads = 1;
        while (!a->stop) { raw_sleep_ms(100); a->reads++; }
        for (;;) raw_sleep_ms(1000);
    }

    int handle = -1;
    const int users[2] = {a->foreground_user, 0xff};
    const uint32_t types[2] = {0u, 1u};
    for (int u = 0; u < 2 && handle < 0; ++u) {
        for (int t = 0; t < 2 && handle < 0; ++t) {
            const int h = imp.audio_in_open((int)users[u], types[t], 0,
                                            MIC_GRAIN, MIC_RATE, 0);
            a->mic_handle = h;
            if (h >= 0) handle = h;
        }
    }
    if (handle < 0) park(&imp);

    int16_t grain[MIC_GRAIN];
    while (!a->stop) {
        const int r = imp.audio_in_input(handle, grain);
        if (r < 0) {
            a->errors++;
            a->last_error = r;
            imp.usleep(5000);
            continue;
        }
        a->reads++;
        int peak = a->peak;
        for (int i = 0; i < MIC_GRAIN; ++i) {
            int v = grain[i] < 0 ? -grain[i] : grain[i];
            if (v > peak) peak = v;
        }
        a->peak = peak;
    }

    imp.audio_in_close(handle);
    park(&imp);
}
