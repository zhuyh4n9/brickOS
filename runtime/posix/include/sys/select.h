/*
 * brickOS prototype v0.2.0 - select surface (sys/select.h; provided by the runtime/posix plugin)
 *
 * `select` is built on top of `poll` (equivalent semantics, one less implementation). `fd_set`
 * is a **fixed-size bitmap**: `FD_SETSIZE = 64` is twice as wide as the native fd table
 * (`BR_FT_MAX = 32`), so "managing beyond the table with select" cannot silently truncate -
 * out-of-range bits are judged invalid by `select` and return -EINVAL.
 *
 * `struct timeval` lives here (rather than sys/time.h): this generation has **no gettimeofday**
 * (no wall clock, P-1), so select is the only consumer of "a time value". When a wall clock is
 * added, this becomes an include (move timeval to sys/time.h).
 */
#ifndef BR_POSIX_SYS_SELECT_H
#define BR_POSIX_SYS_SELECT_H

#include <sys/types.h>

#define FD_SETSIZE  64

struct timeval {
    time_t      tv_sec;
    suseconds_t tv_usec;
};

typedef struct { unsigned long __bits[(FD_SETSIZE + 63u) / 64u]; } fd_set;

#define FD_ZERO(s)      do { unsigned long *__b = (s)->__bits; \
                             for (unsigned __i = 0u; __i < (FD_SETSIZE + 63u) / 64u; __i++) { __b[__i] = 0ul; } \
                        } while (0)
#define FD_SET(fd, s)   ((void)(((fd) >= 0 && (fd) < FD_SETSIZE) ? \
                            ((s)->__bits[(unsigned)(fd) / 64u] |= (1ul << ((unsigned)(fd) % 64u))) : 0u))
#define FD_CLR(fd, s)   ((void)(((fd) >= 0 && (fd) < FD_SETSIZE) ? \
                            ((s)->__bits[(unsigned)(fd) / 64u] &= ~(1ul << ((unsigned)(fd) % 64u))) : 0u))
#define FD_ISSET(fd, s) (((fd) >= 0 && (fd) < FD_SETSIZE) ? \
                            (((s)->__bits[(unsigned)(fd) / 64u] >> ((unsigned)(fd) % 64u)) & 1ul) : 0ul)

int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *timeout);

#endif /* BR_POSIX_SYS_SELECT_H */
