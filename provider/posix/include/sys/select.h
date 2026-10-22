/*
 * select is built on poll. fd_set is a fixed bitmap: FD_SETSIZE = 64 is twice the native fd
 * table (BR_FT_MAX = 32), so out-of-range bits cannot silently truncate -- select judges them
 * invalid and returns -EINVAL.
 * struct timeval lives here because this generation has no gettimeofday.
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
