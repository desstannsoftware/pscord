// PS5 Discord - Logs imports that resolved to nothing at load time.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: GPL-3.0-or-later

/* No headers: the names are declared as plain symbols to read their addresses. */
typedef __SIZE_TYPE__ size_t;
#define NULL ((void *)0)
extern int printf(const char *format, ...);

/* Imports no earlier console-tested title used: a null one is logged at start
 * so the next crash names its cause. */
extern char imp_accept[] __asm__("accept");
extern char imp_bind[] __asm__("bind");
extern char imp_feof[] __asm__("feof");
extern char imp_getpeername[] __asm__("getpeername");
extern char imp_getsockname[] __asm__("getsockname");
extern char imp_ioctl[] __asm__("ioctl");
extern char imp_lseek[] __asm__("lseek");
extern char imp_mprotect[] __asm__("mprotect");
extern char imp_pthread_attr_setdetachstate[] __asm__("pthread_attr_setdetachstate");
extern char imp_pthread_rwlock_destroy[] __asm__("pthread_rwlock_destroy");
extern char imp_pthread_rwlock_init[] __asm__("pthread_rwlock_init");
extern char imp_recvfrom[] __asm__("recvfrom");
extern char imp_shutdown[] __asm__("shutdown");
extern char imp_signal[] __asm__("signal");
extern char imp_strncasecmp[] __asm__("strncasecmp");
extern char imp_strspn[] __asm__("strspn");
extern char imp_tolower[] __asm__("tolower");

void dc_report_missing_imports(void)
{
    const struct
    {
        const char *name;
        const void *address;
    } imports[] = {
        {"accept", imp_accept},
        {"bind", imp_bind},
        {"feof", imp_feof},
        {"getpeername", imp_getpeername},
        {"getsockname", imp_getsockname},
        {"ioctl", imp_ioctl},
        {"lseek", imp_lseek},
        {"mprotect", imp_mprotect},
        {"pthread_attr_setdetachstate", imp_pthread_attr_setdetachstate},
        {"pthread_rwlock_destroy", imp_pthread_rwlock_destroy},
        {"pthread_rwlock_init", imp_pthread_rwlock_init},
        {"recvfrom", imp_recvfrom},
        {"shutdown", imp_shutdown},
        {"signal", imp_signal},
        {"strncasecmp", imp_strncasecmp},
        {"strspn", imp_strspn},
        {"tolower", imp_tolower},
    };
    int missing = 0;
    for (size_t i = 0; i < sizeof(imports) / sizeof(imports[0]); ++i)
    {
        if (imports[i].address == NULL)
        {
            printf("[dc] missing import: %s\n", imports[i].name);
            ++missing;
        }
    }
    printf("[dc] import check: %d missing\n", missing);
}
