/*
 * No wall clock: only br_clock_now() (monotonic microseconds since boot), no RTC/epoch source.
 * CLOCK_MONOTONIC is available and correct; CLOCK_REALTIME is declared but returns -ENOTSUP.
 * gettimeofday/time are absent.
 * Resolution is one tick (1/HZ; default 200 Hz -> 5 ms): nanosleep never wakes early but may
 * overshoot by up to one tick; clock_getres reports this granularity.
 */
#ifndef BR_POSIX_TIME_H
#define BR_POSIX_TIME_H

#include <br/core/br_time.h>
#include <sys/types.h>

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

typedef int clockid_t;

#define CLOCK_REALTIME   0
#define CLOCK_MONOTONIC  1

/* Real granularity: one tick = 1/HZ (no tickless). */
#define BR_POSIX_CLOCK_RES_NS  (1000000000L / (long)BR_CFG_TICK_HZ)

int clock_gettime(clockid_t id, struct timespec *ts);
int clock_getres (clockid_t id, struct timespec *ts);
int nanosleep(const struct timespec *req, struct timespec *rem);

#endif /* BR_POSIX_TIME_H */
