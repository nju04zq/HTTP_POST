#ifndef __UTIL_H__
#define __UTIL_H__

#define _GNU_SOURCE

#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/time.h>
#include <sys/syscall.h>

#ifdef _DEBUG_MODE_
#define LOGGER_TIME_TS_MAX_LEN 63
#define gettid() syscall(__NR_gettid)
#define logger(level, fmt, args...) \
do {\
    struct timeval __cur_tv; \
    struct tm __cur_tm; \
    char __ts[LOGGER_TIME_TS_MAX_LEN+1]; \
    gettimeofday(&__cur_tv, NULL); \
    localtime_r(&__cur_tv.tv_sec, &__cur_tm); \
    strftime(__ts, LOGGER_TIME_TS_MAX_LEN, "%T", &__cur_tm); \
    printf("%s.%03d <%04x> "#level" "fmt"\n", __ts, (int)__cur_tv.tv_usec/1000,\
           (unsigned int)gettid(), ##args); \
    fflush(stdin); \
} while (0)
#else
#define logger(level, fmt, args...)
#endif

#ifndef True
#define True true
#endif

#ifndef False
#define False false
#endif

#define MAX(a, b) ((a) > (b) ? (a) : (b))

void memzero (void *p, uint32_t size);
#endif //__UTIL_H__
