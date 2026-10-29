/* ps5-discord - ABI shared between the injector (runs in our payload) and the
 * shim (runs inside SceShellCore). The injector resolves every function the
 * shim needs inside the target with kernel_dynlib_dlsym and writes the pointers
 * here, so the shim itself references no external symbols and needs no
 * relocation once copied into the target.
 * Copyright (C) 2026 Desstann
 * SPDX-License-Identifier: MIT */
#ifndef PSCORD_SHIM_ABI_H
#define PSCORD_SHIM_ABI_H

#include <stdint.h>

#define SHIM_MAGIC 0x50534331u /* "PSC1" */

/* Imports the shim calls, all resolved against the target process. */
typedef struct {
    int   (*audio_in_open)(int user, uint32_t type, uint32_t index,
                           uint32_t grain, uint32_t rate, uint32_t param);
    int   (*audio_in_input)(int handle, void *samples);
    int   (*audio_in_close)(int handle);
    int   (*usleep)(unsigned usec);
} shim_imports_t;

/* Filled by the injector before the shim starts; the shim writes results back
 * so the injector can read them after detaching. */
typedef struct {
    uint32_t       magic;        /* SHIM_MAGIC once the shim has started */
    volatile int32_t stop;       /* injector sets to 1 to ask the shim to exit */
    shim_imports_t imports;
    int32_t        foreground_user;
    int32_t        probe_only;   /* 1 = just write magic and park, no audio */

    /* status written by the shim */
    volatile int32_t  mic_handle;   /* open result or error code */
    volatile uint32_t reads;        /* successful sceAudioInInput calls */
    volatile uint32_t errors;       /* failed reads */
    volatile int32_t  peak;         /* loudest sample seen so far */
    volatile int32_t  last_error;   /* last read error code */
} shim_args_t;

#endif
