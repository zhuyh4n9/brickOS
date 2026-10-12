/*
 * brickOS prototype v0.2.0 — semaphores (semaphore.h; provided by the runtime/posix plugin)
 *
 * Base = core's `br_sem_t` (allocation-free wait chain, `3-01` §7). `sem_post` may be called
 * directly from an ISR (core's `br_sem_give` is ISR-safe, `3-02` §11.3) — one of this
 * service's few legal interrupt-to-thread channels.
 *
 * ★ `sem_t` **is** `br_sem_t`: the layout is public + there is a static initializer
 *   (`BR_SEM_INIT_VALUE`), so both `sem_init` and static initialization work; there is no
 *   lazy-initialization ambiguity.
 * ★ `sem_getvalue` is an **observation** (core's `br_sem_count`) and does not change
 *   semantics.
 * ★ `sem_open`/`sem_close`/`sem_unlink` (named semaphores) are **not in this generation**:
 *   they need a kernel object namespace, which is a v2 topic — not declaring them = exposed
 *   at compile time.
 */
#ifndef BR_POSIX_SEMAPHORE_H
#define BR_POSIX_SEMAPHORE_H

#include <sys/types.h>
#include <br/core/br_sync.h>
#include <br/core/br_time.h>

typedef br_sem_t sem_t;

#define BR_POSIX_SEM_VALUE_MAX  0x7fffffffu

int sem_init(sem_t *s, int pshared, unsigned value);
int sem_destroy(sem_t *s);          /* no-op ⇒ 0 (same reason as pthread_*_destroy) */
int sem_wait(sem_t *s);
int sem_trywait(sem_t *s);
int sem_timedwait(sem_t *s, const struct timespec *abstime);
int sem_post(sem_t *s);
int sem_getvalue(sem_t *s, int *out);

#endif /* BR_POSIX_SEMAPHORE_H */
