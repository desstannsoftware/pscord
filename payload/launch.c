/* ps5-discord - Development payload: starts PSCord on the console, so a build
 * can be checked without picking it up from the home screen.
 * Copyright (C) 2026 Desstann
 * SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t size;
    uint32_t user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    uint32_t check_flag;
} LaunchParam;

int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(int *user);
int sceSystemServiceLaunchApp(const char *title_id, const char **argv, LaunchParam *param);

int main(void) {
    int user = -1;
    sceUserServiceInitialize(NULL);
    sceUserServiceGetForegroundUser(&user);

    LaunchParam param;
    memset(&param, 0, sizeof(param));
    param.size = sizeof(param);
    param.user_id = (uint32_t)user;
    const int result = sceSystemServiceLaunchApp("PPSA99190", NULL, &param);

    FILE *log = fopen("/data/ps5discord/launch.log", "w");
    if (log) {
        fprintf(log, "user=0x%x launch=0x%08x\n", user, (unsigned)result);
        fclose(log);
    }
    return 0;
}
