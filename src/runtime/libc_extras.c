// PS5 Discord - libc entry points OpenSSL and the client need but the
// title's libc does not export.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: GPL-3.0-or-later

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Turkey has kept UTC+3 all year since 2016. */
#define LOCAL_OFFSET_SECONDS (3 * 3600)

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static struct tm *civil_from_seconds(int64_t t, struct tm *out)
{
    int64_t days = t / 86400;
    int64_t rem = t % 86400;
    if (rem < 0)
    {
        rem += 86400;
        --days;
    }
    out->tm_hour = (int)(rem / 3600);
    out->tm_min = (int)(rem % 3600 / 60);
    out->tm_sec = (int)(rem % 60);
    out->tm_wday = (int)((days % 7 + 11) % 7);

    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    const int64_t y = (int64_t)yoe + era * 400 + (m <= 2);

    out->tm_year = (int)(y - 1900);
    out->tm_mon = (int)m - 1;
    out->tm_mday = (int)d;
    out->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    out->tm_isdst = 0;
    return out;
}

struct tm *gmtime_r(const time_t *t, struct tm *out)
{
    return civil_from_seconds((int64_t)*t, out);
}

struct tm *localtime_r(const time_t *t, struct tm *out)
{
    return civil_from_seconds((int64_t)*t + LOCAL_OFFSET_SECONDS, out);
}

time_t timegm(struct tm *tm)
{
    int64_t year = tm->tm_year + 1900;
    int64_t month = tm->tm_mon;
    year += month / 12;
    month %= 12;
    if (month < 0)
    {
        month += 12;
        --year;
    }
    const int64_t days = days_from_civil(year, (unsigned)month + 1, 1) + tm->tm_mday - 1;
    return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

typedef struct
{
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
} Dl_info;

int dladdr(const void *address, Dl_info *info)
{
    (void)address;
    (void)info;
    return 0;
}

/* arc4random lives in libScePosixForWebKit and sceRandomGetRandomNumber in
 * libSceRandom; a game process loads neither, so both imports stay null. The
 * kernel's own generator (sysctl kern.arandom) is always there. */
extern int sysctl(const int *name, unsigned int name_length, void *old_value, size_t *old_length,
                  const void *new_value, size_t new_length);
extern uint64_t sceKernelGetProcessTime(void);

void arc4random_buf(void *buffer, size_t size)
{
    static const int kArandom[2] = {1 /* CTL_KERN */, 37 /* KERN_ARND */};
    unsigned char *out = (unsigned char *)buffer;
    while (size > 0)
    {
        size_t chunk = size < 256 ? size : 256;
        if (sysctl(kArandom, 2, out, &chunk, NULL, 0) != 0 || chunk == 0)
        {
            static uint64_t state;
            state ^= sceKernelGetProcessTime() | 1;
            chunk = size < 256 ? size : 256;
            for (size_t i = 0; i < chunk; ++i)
            {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                out[i] = (unsigned char)state;
            }
        }
        out += chunk;
        size -= chunk;
    }
}

uint32_t arc4random(void)
{
    uint32_t value;
    arc4random_buf(&value, sizeof(value));
    return value;
}

uint32_t arc4random_uniform(uint32_t bound)
{
    if (bound < 2)
        return 0;
    const uint32_t floor = -bound % bound;
    for (;;)
    {
        const uint32_t value = arc4random();
        if (value >= floor)
            return value % bound;
    }
}

/* Name lookup is ours (src/dc/net/dns.cpp); OpenSSL's BIO_lookup paths that
 * import these from libScePosixForWebKit must fail instead of jumping to 0. */
struct addrinfo;
struct sockaddr;
#define DC_EAI_FAIL 4

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **result)
{
    (void)node;
    (void)service;
    (void)hints;
    if (result != NULL)
        *result = NULL;
    return DC_EAI_FAIL;
}

void freeaddrinfo(struct addrinfo *list)
{
    (void)list;
}

const char *gai_strerror(int code)
{
    (void)code;
    return "name lookup unavailable";
}

int getnameinfo(const struct sockaddr *address, unsigned int length, char *host, unsigned int host_length,
                char *service, unsigned int service_length, int flags)
{
    (void)address;
    (void)length;
    (void)host;
    (void)host_length;
    (void)service;
    (void)service_length;
    (void)flags;
    return DC_EAI_FAIL;
}

void *gethostbyname(const char *name)
{
    (void)name;
    return NULL;
}

struct timeval;
extern int clock_gettime(int clock, struct timespec *now);

int gettimeofday(struct timeval *tv, void *zone)
{
    (void)zone;
    struct timespec now;
    if (clock_gettime(0 /* CLOCK_REALTIME */, &now) != 0)
        return -1;
    if (tv != NULL)
    {
        long *fields = (long *)tv;
        fields[0] = (long)now.tv_sec;
        fields[1] = now.tv_nsec / 1000;
    }
    return 0;
}

int issetugid(void)
{
    return 0;
}

struct mmsghdr;

int sendmmsg(int fd, struct mmsghdr *messages, unsigned int count, int flags)
{
    (void)fd;
    (void)messages;
    (void)count;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

int recvmmsg(int fd, struct mmsghdr *messages, unsigned int count, int flags, struct timespec *timeout)
{
    (void)fd;
    (void)messages;
    (void)count;
    (void)flags;
    (void)timeout;
    errno = ENOSYS;
    return -1;
}

/* OpenSSL's async fibres save and restore callee-saved registers through
 * these; the buffer layout is private to this pair. */
__asm__(".text\n"
        ".globl _setjmp\n"
        ".type _setjmp,@function\n"
        "_setjmp:\n"
        "    movq (%rsp), %rdx\n"
        "    movq %rdx, 0(%rdi)\n"
        "    movq %rbx, 8(%rdi)\n"
        "    leaq 8(%rsp), %rdx\n"
        "    movq %rdx, 16(%rdi)\n"
        "    movq %rbp, 24(%rdi)\n"
        "    movq %r12, 32(%rdi)\n"
        "    movq %r13, 40(%rdi)\n"
        "    movq %r14, 48(%rdi)\n"
        "    movq %r15, 56(%rdi)\n"
        "    xorl %eax, %eax\n"
        "    ret\n"
        ".size _setjmp, .-_setjmp\n"
        ".globl _longjmp\n"
        ".type _longjmp,@function\n"
        "_longjmp:\n"
        "    movl %esi, %eax\n"
        "    testl %eax, %eax\n"
        "    jnz 1f\n"
        "    incl %eax\n"
        "1:  movq 8(%rdi), %rbx\n"
        "    movq 16(%rdi), %rsp\n"
        "    movq 24(%rdi), %rbp\n"
        "    movq 32(%rdi), %r12\n"
        "    movq 40(%rdi), %r13\n"
        "    movq 48(%rdi), %r14\n"
        "    movq 56(%rdi), %r15\n"
        "    jmp *0(%rdi)\n"
        ".size _longjmp, .-_longjmp\n");
