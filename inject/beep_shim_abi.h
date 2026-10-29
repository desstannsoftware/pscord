/* PSCord - ABI for the ShellUI beep shim.
 * Copyright (C) 2026 Desstann
 * SPDX-License-Identifier: MIT
 */
#ifndef PSCORD_BEEP_SHIM_ABI_H
#define PSCORD_BEEP_SHIM_ABI_H
#include <stdint.h>

#define BEEP_MAGIC 0x42454550u /* BEEP */

typedef struct {
    int (*init)(void);
    int (*open)(int user, int type, int index, unsigned grain, unsigned rate, unsigned format);
    int (*output)(int handle, const void *samples);
    int (*close)(int handle);
    int (*set_volume)(int handle, int flag, int *volume);
    int (*set_volume_down)(int handle, int volume);
    int (*set_main)(int handle);
    int (*usleep)(unsigned usec);
} beep_imports_t;

typedef struct {
    uint32_t magic;
    volatile int32_t stop;
    beep_imports_t imports;
    volatile int32_t init_rc;
    volatile int32_t open_rc;
    volatile int32_t port;
    volatile int32_t last_out;
    volatile uint32_t grains;
    volatile uint32_t errors;
    volatile int32_t done;
} beep_args_t;

#endif
