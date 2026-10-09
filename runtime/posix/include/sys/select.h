/*
 * brickOS prototype v0.2.0 — select 面(sys/select.h; runtime/posix 插件提供)
 *
 * ★ `select` 建在 `poll` 之上(语义等价, 少一份实现)。`fd_set` 是**定长位图**:
 *   `FD_SETSIZE = 64` 比原生 fd 表(`BR_FT_MAX = 32`)宽一倍, 于是"用 select 管到
 *   表外"这件事不会静默截断 —— 超界位在 `select` 里被判为无效并返回 -EINVAL。
 *
 * ★ struct timeval 放在这里(而不是 sys/time.h): 本代**没有 gettimeofday**(无墙钟, P-1),
 *   于是"时间值的载体"只有 select 这一个消费者。将来补墙钟时, 这里改成一个 include
 *   即可(把 timeval 挪去 sys/time.h)。
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
