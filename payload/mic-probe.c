/* ps5-discord - Development payload: can a process outside the app open the
 * microphone? Opens it like a background daemon would, reads three seconds
 * and logs the level to /data/ps5discord/mic-probe.log.
 * Copyright (C) 2026 Desstann
 * SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(int *user);
int sceAudioInOpen(int user, uint32_t type, uint32_t index, uint32_t grain, uint32_t rate, uint32_t param);
int sceAudioInInput(int handle, void *samples);
int sceAudioInClose(int handle);

int main(void) {
    FILE *log = fopen("/data/ps5discord/mic-probe.log", "w");
    if (!log) return 1;
    int user = -1;
    sceUserServiceInitialize(NULL);
    sceUserServiceGetForegroundUser(&user);
    fprintf(log, "foreground user=0x%x\n", user);

    const int users[2] = {user, 255};
    const uint32_t types[2] = {0, 1};
    int handle = -1;
    for (int u = 0; u < 2 && handle < 0; ++u) {
        for (int t = 0; t < 2 && handle < 0; ++t) {
            const int h = sceAudioInOpen(users[u], types[t], 0, 256, 16000, 0);
            fprintf(log, "open user=0x%x type=%u -> 0x%08x\n", users[u], types[t], (unsigned)h);
            if (h >= 0) handle = h;
        }
    }
    if (handle >= 0) {
        int16_t grain[256];
        int peak = 0, reads = 0, errors = 0;
        for (int i = 0; i < 3 * 16000 / 256; ++i) {
            const int r = sceAudioInInput(handle, grain);
            if (r < 0) {
                if (++errors == 1) fprintf(log, "input -> 0x%08x\n", (unsigned)r);
                continue;
            }
            ++reads;
            for (int s = 0; s < 256; ++s) {
                const int v = abs(grain[s]);
                if (v > peak) peak = v;
            }
        }
        fprintf(log, "reads=%d errors=%d peak=%d\n", reads, errors, peak);
        sceAudioInClose(handle);
    }
    fclose(log);
    return 0;
}
