/*
 * Semaphores over core's br_sem_t (allocation-free wait chain). sem_post is ISR-safe (core's
 * br_sem_give) -- one of the few legal interrupt-to-thread channels. sem_t is br_sem_t:
 * public layout plus BR_SEM_INIT_VALUE, so static initialization works. sem_getvalue is a
 * snapshot (br_sem_count). Named semaphores (sem_open/close/unlink) are not in this generation.
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
