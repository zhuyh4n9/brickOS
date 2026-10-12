/*
 * brickOS prototype v0.2.0 — event waiting (poll.h; provided by the runtime/posix plugin)
 *
 * ★ v1 semantics = the existing design SD-7 line ("**polling + coarse-grained sleep**"):
 *   check the ready bits per fd (`br_file_poll`); if none is ready, `br_task_sleep` a short
 *   slice and check again until the timeout. Consistent with the TR-B decision in `11-02`
 *   §2.1; `poll_attach` (a real wait-queue) is a v2 surface, and the design explicitly
 *   requires it to be **concurrent with preempt** (R-S1).
 * ★ Hence the **actual wakeup latency ≈ one tick (`1/HZ`, 5 ms by default)**, not poll's
 *   timeout value. `timeout = 0` means "check once" (return immediately), `timeout < 0`
 *   means "wait forever".
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
