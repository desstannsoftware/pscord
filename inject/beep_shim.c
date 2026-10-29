/* PSCord - Flat PIC beep player for injection into ShellUI. No libc.
 * Copyright (C) 2026 Desstann
 * SPDX-License-Identifier: MIT
 */
#include "beep_shim_abi.h"

static inline long raw_syscall2(long n, long a0, long a1) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a0), "S"(a1) : "rcx", "r11", "memory");
    return ret;
}
#define SYS_nanosleep_ 240
static void sleep_ms(long ms) {
    long ts[2] = {ms / 1000, (ms % 1000) * 1000000L};
    raw_syscall2(SYS_nanosleep_, (long)ts, 0);
}

__attribute__((section(".text.entry"), used)) void shim_entry(void *arg) {
    beep_args_t *a = (beep_args_t *)arg;
    a->magic = BEEP_MAGIC;
    const beep_imports_t imp = a->imports;

    if (imp.init)
        a->init_rc = imp.init();

    int handle = -1;
    const int ports[3] = {0, 1, 2};
    for (int i = 0; i < 3 && handle < 0; ++i) {
        const int h = imp.open(0xff, ports[i], 0, 256, 48000, 1);
        a->open_rc = h;
        a->port = ports[i];
        if (h >= 0)
            handle = h;
    }
    if (handle < 0) {
        for (;;)
            sleep_ms(1000);
    }

    if (imp.set_volume) {
        int vol[8];
        for (int i = 0; i < 8; ++i)
            vol[i] = 32768;
        imp.set_volume(handle, 0xff, vol);
    }
    if (imp.set_volume_down)
        imp.set_volume_down(handle, 0);
    if (imp.set_main)
        imp.set_main(handle);

    int16_t grain[256 * 2];
    /* ~2 seconds at 48 kHz / 256 */
    for (int block = 0; block < (48000 / 256) * 2 && !a->stop; ++block) {
        for (int i = 0; i < 256; ++i) {
            const int sample = block * 256 + i;
            const int16_t v = ((sample / 18) & 1) ? 30000 : -30000;
            grain[i * 2] = v;
            grain[i * 2 + 1] = v;
        }
        const int r = imp.output(handle, grain);
        a->last_out = r;
        a->grains++;
        if (r < 0)
            a->errors++;
    }
    if (imp.close)
        imp.close(handle);
    a->done = 1;
    for (;;)
        sleep_ms(1000);
}
