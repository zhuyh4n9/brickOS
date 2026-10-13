/*
 * Synchronization primitives (mutex / semaphore / cond / spinlock), implemented in core so all
 * plugins can see the lock objects: br_sched_ops keeps only "who runs first". spinlock is a new
 * surface (not in the design): spin bit + IRQ masking -- on a single core IRQ masking is the real
 * mutex, the spin bit reserves room for multicore.
 */
#ifndef BR_CORE_BR_SYNC_H
#define BR_CORE_BR_SYNC_H

#include <br/core/br_irq.h>
#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

/* Object magic word: observation and selftest tell whether an object was initialized. */
#define BR_SYNC_MAGIC_MUTEX 0x4D555458u /* "MUTX" */
#define BR_SYNC_MAGIC_SEM   0x53454D41u /* "SEMA" */
#define BR_SYNC_MAGIC_COND  0x434F4E44u /* "COND" */
#define BR_SYNC_MAGIC_SPIN  0x5350494Eu /* "SPIN" */

/* ---- mutex ---- */

/* Wait-chain head `head`; nodes are embedded in thread TCBs (no allocation on the sync path).
 * `locked` is for the lock-free fast path, `owner` for diagnostics and future PI. */
typedef struct br_mutex {
    br_u32        magic;
    volatile br_u32 locked;
    br_thread_t  *owner;
    br_u32        waiters;
    void         *head;
} br_mutex_t;

#define BR_MUTEX_INIT_VALUE { BR_SYNC_MAGIC_MUTEX, 0u, BR_NULL, 0u, BR_NULL }
/* Static initializer; static objects need no br_mutex_init() call. */
#define BR_MUTEX_DEFINE(name) br_mutex_t name = BR_MUTEX_INIT_VALUE

int br_mutex_init(br_mutex_t *m);
/* Blocking acquire (wait forever). -EINVAL = uninitialized object. */
int br_mutex_lock(br_mutex_t *m);
/* Timeout form: BR_TIMEOUT_ZERO = trylock (busy => -ETIMEDOUT; a zero timeout is just "wait
 * 0 us", not a different error, TC-SYNC-002); BR_TIMEOUT_INF = infinite. */
int br_mutex_lock_to(br_mutex_t *m, br_time_t timeout);
/* Unlock by a non-owner => -EPERM. */
int br_mutex_unlock(br_mutex_t *m);

br_bool br_mutex_is_locked(const br_mutex_t *m);
br_thread_t *br_mutex_owner(const br_mutex_t *m);

/* ---- semaphore ---- */

typedef struct br_sem {
    br_u32        magic;
    volatile br_u32 count;
    br_u32        waiters;
    void         *head;
} br_sem_t;

#define BR_SEM_INIT_VALUE(n) { BR_SYNC_MAGIC_SEM, (n), 0u, BR_NULL }
#define BR_SEM_DEFINE(name, n) br_sem_t name = BR_SEM_INIT_VALUE(n)

int br_sem_init(br_sem_t *s, unsigned initial);
/* Take (p operation): count > 0 => decrement; otherwise block (three timeout states as above). */
int br_sem_take(br_sem_t *s, br_time_t timeout);
/* Give (v operation): ISR-safe; count += 1, wakes one waiter. Under v1 COOP it does not switch
 * to the woken thread immediately. */
int br_sem_give(br_sem_t *s);
br_u32 br_sem_count(const br_sem_t *s);

/* ---- cond ---- */

typedef struct br_cond {
    br_u32 magic;
    br_u32 waiters;
    void  *head;
} br_cond_t;

#define BR_COND_INIT_VALUE { BR_SYNC_MAGIC_COND, 0u, BR_NULL }
#define BR_COND_DEFINE(name) br_cond_t name = BR_COND_INIT_VALUE

int br_cond_init(br_cond_t *c);
/* Atomically "release m + wait + reacquire m"; on -ETIMEDOUT the lock state must already be
 * restored (TC-SYNC-008). Signal with no waiters is a no-op (unlike sem). */
int br_cond_wait(br_cond_t *c, br_mutex_t *m, br_time_t timeout);
int br_cond_signal(br_cond_t *c);    /* wake one (ISR context allowed) */
int br_cond_broadcast(br_cond_t *c); /* wake all */
br_u32 br_cond_waiters(const br_cond_t *c);

/* ---- spinlock ---- */

/* Spinlock (see file header). lock masks IRQs (nest counting in core) + spins until acquired,
 * returning the prior interrupt state that must be passed unchanged to unlock; single core
 * acquires on the first try. */
typedef struct br_spinlock {
    br_u32          magic;
    volatile br_u32 lock;
} br_spinlock_t;

#define BR_SPINLOCK_INIT_VALUE { BR_SYNC_MAGIC_SPIN, 0u }
#define BR_SPINLOCK_DEFINE(name) br_spinlock_t name = BR_SPINLOCK_INIT_VALUE

int br_spinlock_init(br_spinlock_t *s);
br_irq_state_t br_spinlock_lock(br_spinlock_t *s);
/* busy => -EBUSY (does not block or mask interrupts; `saved` unchanged) */
int br_spinlock_trylock(br_spinlock_t *s, br_irq_state_t *saved);
void br_spinlock_unlock(br_spinlock_t *s, br_irq_state_t saved);
br_bool br_spinlock_is_locked(const br_spinlock_t *s);
/* Count of spins of at least one full round (always 0 on a single core). */
br_u32 br_spinlock_contention(void);

/* The sync selftest suite is declared in core/c_core/selftest/, not here (test surface). */

#endif /* BR_CORE_BR_SYNC_H */
