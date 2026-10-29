/* Flat PIC: plays a PCM ring through sceAudioOut MAIN inside PartyDaemon.
 * No libc. Feeder fills the ring from outside without stopping this process.
 * Copyright (C) 2026 Desstann
 * SPDX-License-Identifier: MIT */
#include "pump_shim_abi.h"

static inline long raw_syscall2(long n, long a0, long a1) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a0), "S"(a1) : "rcx", "r11", "memory");
    return ret;
}
#define SYS_nanosleep_ 240
static void sleep_us(long us) {
    long ts[2] = {us / 1000000L, (us % 1000000L) * 1000L};
    raw_syscall2(SYS_nanosleep_, (long)ts, 0);
}

__attribute__((section(".text.entry"), used)) void shim_entry(void *arg) {
    pump_args_t *a = (pump_args_t *)arg;
    a->magic = PUMP_MAGIC;
    const pump_imports_t imp = a->imports;
    int16_t *const ring = a->ring;
    const int slots = a->slots > 0 ? a->slots : PUMP_SLOTS;

    if (imp.init)
        a->init_rc = imp.init();

    int handle = -1;
    const int ports[3] = {0, 1, 2};
    for (int i = 0; i < 3 && handle < 0; ++i) {
        const int h = imp.open(0xff, ports[i], 0, PUMP_FRAMES, 48000, 1);
        a->open_rc = h;
        a->port = ports[i];
        if (h >= 0)
            handle = h;
    }
    a->handle = handle;
    if (handle < 0 || !ring) {
        for (;;)
            sleep_us(1000 * 1000);
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

    int16_t silence[PUMP_SAMPLES];
    for (int i = 0; i < PUMP_SAMPLES; ++i)
        silence[i] = 0;

    uint32_t ticks = 0;
    while (!a->stop) {
        const uint32_t w = a->write_seq;
        uint32_t r = a->read_seq;
        const int16_t *src = silence;
        if ((int32_t)(w - r) > 0) {
            src = ring + (unsigned long)(r % (uint32_t)slots) * PUMP_SAMPLES;
            a->read_seq = r + 1;
            int peak = 0;
            for (int i = 0; i < PUMP_SAMPLES; ++i) {
                const int v = src[i] < 0 ? -src[i] : src[i];
                if (v > peak)
                    peak = v;
            }
            if (peak > a->peak)
                a->peak = peak;
        } else {
            a->underruns++;
        }
        const int out = imp.output(handle, src);
        a->last_out = out;
        if (out >= 0)
            a->played++;
        if (++ticks >= 200) { /* ~1 s at 5.33 ms/grain */
            a->alive++;
            ticks = 0;
        }
    }

    if (imp.close)
        imp.close(handle);
    for (;;)
        sleep_us(1000 * 1000);
}
