/*
 * brickOS prototype v0.2.0 — time (time.h; provided by the runtime/posix plugin)
 *
 * ★★ **No wall clock**: the prototype has only a monotonic counter (`br_clock_now()`,
 *    microseconds since boot) and the platform arch timer, **no RTC/epoch source**
 *    (design gap P-1). Hence:
 *     - `CLOCK_MONOTONIC` is **available and semantically correct** (monotonic
 *       non-decreasing, see `3-01` INV-2);
 *     - `CLOCK_REALTIME` is **declared but returns -ENOTSUP** — rather than return a
 *       number pretending to be "since 1970", let callers see at runtime that this
 *       target has no wall clock (honest-subset duty).
 *   This matches the TR-C ruling in `11-02` §2.4; `gettimeofday`/`time` are **not in
 *   this header**.
 *
 * ★ Resolution = **one tick** (`1/HZ`): the platform timer is a periodic tick (the rate
 *   `HZ` comes from `product.toml [kernel].hz`, default 200 ⇒ 5 ms; ADR-0017); tickless
 *   is not done yet (P-2). So `nanosleep`'s **semantics** are "never wake early", while
 *   the **actual overshoot** can reach one tick — `clock_getres` reports this granularity
 *   honestly. This is the most easily misused spot of this generation, hence noted here.
 */
#ifndef BR_POSIX_TIME_H
#define BR_POSIX_TIME_H

#include <br/core/br_time.h>   /* BR_CFG_TICK_HZ: the resolution has a single truth source (ADR-0017) */
#include <sys/types.h>

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

typedef int clockid_t;

#define CLOCK_REALTIME   0
#define CLOCK_MONOTONIC  1

/* tickless is not in place ⇒ this is the **real** granularity, not a "design value"
 * (= one tick = 1/HZ; see the note above) */
#define BR_POSIX_CLOCK_RES_NS  (1000000000L / (long)BR_CFG_TICK_HZ)

int clock_gettime(clockid_t id, struct timespec *ts);
int clock_getres (clockid_t id, struct timespec *ts);
int nanosleep(const struct timespec *req, struct timespec *rem);

#endif /* BR_POSIX_TIME_H */
