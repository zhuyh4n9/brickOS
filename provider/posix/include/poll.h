/*
 * v1 = polling + coarse sleep: check br_file_poll per fd; if none is ready, br_task_sleep a
 * short slice and retry until timeout. Actual wakeup latency is ~one tick (1/HZ, 5 ms default),
 * not the timeout value. timeout = 0 checks once; timeout < 0 waits forever.
 */
#ifndef BR_POSIX_POLL_H
#define BR_POSIX_POLL_H

#include <sys/types.h>

struct pollfd {
    int   fd;
    short events;    /* events to wait for (in) */
    short revents;   /* events that actually occurred (out) */
};

typedef unsigned long nfds_t;

#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);

#endif /* BR_POSIX_POLL_H */
