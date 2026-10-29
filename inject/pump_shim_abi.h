/* Shared ABI: party-audio-pump injector ↔ PartyDaemon PCM shim.
 * Copyright (C) 2026 Desstann
 * SPDX-License-Identifier: MIT */
#ifndef PSCORD_PUMP_SHIM_ABI_H
#define PSCORD_PUMP_SHIM_ABI_H

#include <stdint.h>

#define PUMP_MAGIC 0x50554d50u /* "PUMP" */
#define PUMP_FRAMES 256
#define PUMP_SAMPLES (PUMP_FRAMES * 2)
#define PUMP_BYTES (PUMP_SAMPLES * sizeof(int16_t))
#define PUMP_SLOTS 16

typedef struct {
    int (*init)(void);
    int (*open)(int user, int type, int index, unsigned grain, unsigned rate, unsigned format);
    int (*output)(int handle, const void *samples);
    int (*close)(int handle);
    int (*set_volume)(int handle, int flag, int *volume);
    int (*set_volume_down)(int handle, int volume);
    int (*set_main)(int handle);
    int (*usleep)(unsigned usec);
} pump_imports_t;

/* Header lives in one mmap; PCM ring in a second. The injector fills imports
 * and ring, then starts the shim with scePthreadCreate and detaches. The
 * feeder writes grains with kernel_proc_copyin — PartyDaemon never stops. */
typedef struct {
    uint32_t magic;
    volatile int32_t stop;
    pump_imports_t imports;

    volatile uint32_t write_seq; /* feeder bumps after filling slots[write % N] */
    volatile uint32_t read_seq;  /* shim bumps after playing slots[read % N] */

    int16_t *ring; /* target VA of PUMP_SLOTS * PUMP_SAMPLES samples */
    int32_t slots; /* = PUMP_SLOTS */

    volatile int32_t init_rc;
    volatile int32_t open_rc;
    volatile int32_t port;
    volatile int32_t handle;
    volatile int32_t last_out;
    volatile uint32_t played;
    volatile uint32_t underruns;
    volatile int32_t peak;
    volatile int32_t alive; /* shim increments ~every second */
} pump_args_t;

#endif
