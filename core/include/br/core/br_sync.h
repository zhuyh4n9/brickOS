/*
 * brickOS prototype v0.2.0 — synchronization primitives (core): mutex / semaphore / cond / spinlock
 *
 * Design basis: `3-01` §7 (synchronization primitives, lines 117–139), §14 CA-4 (three timeout states), INV-1/INV-2 (timeout semantics),
 *           `3-02` §6.4 (`br_irq_lock/unlock` nest counting lives in core), `6-01` §3.2 (TC-SYNC-*).
 *
 * ## Ownership and deviation (ADR-0006 §2)
 *
 * Design `3-01` §115 puts the implementation of synchronization primitives in the scheduler plugin. This prototype moves it to core:
 *   - lock objects must be visible to all plugins => a plugin header would force "plugins depending on each other";
 *   - blocking/wakeup mechanics are the same for coop and preempt (only "who runs first" differs);
 *   - the design's ops table is itself an ellipsis-bearing draft.
 * So `br_sched_ops` keeps only "who runs first", and synchronization semantics stay in core. The external semantics are unchanged
 * (signatures, error codes, and the ISR-safe `br_sem_give` all follow the design).
 *
 * ## Things not in the design (registered honestly)
 *
 * `spinlock` has 0 hits in the design documents (`grep -rni spin Design/` returns nothing; the closest is
 * `br_irq_lock/br_irq_unlock`). User requirements explicitly ask for it => a new surface here, recorded via CA-5:
 * `br_spinlock_t` = spin bit + IRQ masking (on a single core, IRQ masking is the real mutex; the spin bit reserves
 * room for multicore). Its relation to `br_irq_lock`: spinlock = irq_lock + cross-core spin + observable contention count.
 *
 * ## Time constants
 *
 * `BR_TIMEOUT_INF` / `BR_TIMEOUT_ZERO` / `br_deadline_from_now()` belong to `br_time.h`
 * (design `3-01` §14 CA-1 / §7); this header only uses them.
 */
#ifndef BR_CORE_BR_SYNC_H
#define BR_CORE_BR_SYNC_H

#include <br/core/br_irq.h>
#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

/* Object magic word: observation and selftest use it to tell whether an object was initialized (not in the design; see ADR-0006 §2). */
#define BR_SYNC_MAGIC_MUTEX 0x4D555458u /* "MUTX" */
#define BR_SYNC_MAGIC_SEM   0x53454D41u /* "SEMA" */
#define BR_SYNC_MAGIC_COND  0x434F4E44u /* "COND" */
#define BR_SYNC_MAGIC_SPIN  0x5350494Eu /* "SPIN" */

/* ==================================================================== mutex */

/*
 * Mutex. `head` = wait-chain head (nodes are embedded in thread TCBs — the sync path allocates nothing).
 * `locked` and `owner` are stored separately: `locked` for the lock-free fast path, `owner` for diagnostics and future PI (v2).
 */
typedef struct br_mutex {
    br_u32        magic;
    volatile br_u32 locked;
    br_thread_t  *owner;
    br_u32        waiters;
    void         *head;
} br_mutex_t;

#define BR_MUTEX_INIT_VALUE { BR_SYNC_MAGIC_MUTEX, 0u, BR_NULL, 0u, BR_NULL }
/* Static initialization (BR_MUTEX_DEFINE from design `3-01` §7); static objects need not call init again. */
#define BR_MUTEX_DEFINE(name) br_mutex_t name = BR_MUTEX_INIT_VALUE

int br_mutex_init(br_mutex_t *m);
/* Blocking acquire (wait forever). -EINVAL = uninitialized object. */
int br_mutex_lock(br_mutex_t *m);
/* With timeout: BR_TIMEOUT_ZERO = trylock (busy => `-ETIMEDOUT` — design `6-01` TC-SYNC-002
 * and INV-1: a zero timeout is just "wait 0 microseconds", not a different error); BR_TIMEOUT_INF = infinite. */
int br_mutex_lock_to(br_mutex_t *m, br_time_t timeout);
/* Unlock by a non-owner => -EPERM (semantics from design `3-01` §7). */
int br_mutex_unlock(br_mutex_t *m);

br_bool br_mutex_is_locked(const br_mutex_t *m);
br_thread_t *br_mutex_owner(const br_mutex_t *m);

/* ==================================================================== semaphore */

typedef struct br_sem {
    br_u32        magic;
    volatile br_u32 count;
    br_u32        waiters;
    void         *head;
} br_sem_t;

#define BR_SEM_INIT_VALUE(n) { BR_SYNC_MAGIC_SEM, (n), 0u, BR_NULL }
#define BR_SEM_DEFINE(name, n) br_sem_t name = BR_SEM_INIT_VALUE(n)

int br_sem_init(br_sem_t *s, unsigned initial);
/* Take (p operation): count > 0 => decrement and return; otherwise block (three timeout states as above). */
int br_sem_take(br_sem_t *s, br_time_t timeout);
/* Give (v operation): ISR-safe (`3-01` §7 / `3-02` §11.3); count += 1, wakes one waiter.
 * Under v1's COOP it does not switch to the woken thread immediately (`3-02` line 1097). */
int br_sem_give(br_sem_t *s);
br_u32 br_sem_count(const br_sem_t *s);

/* ==================================================================== cond */

typedef struct br_cond {
    br_u32 magic;
    br_u32 waiters;
    void  *head;
} br_cond_t;

#define BR_COND_INIT_VALUE { BR_SYNC_MAGIC_COND, 0u, BR_NULL }
#define BR_COND_DEFINE(name) br_cond_t name = BR_COND_INIT_VALUE

int br_cond_init(br_cond_t *c);
/*
 * Atomically "release m + wait + reacquire m". On a timeout return of -ETIMEDOUT the lock state must already be restored
 * (the TC-SYNC-008 criterion). With no waiters, signal is a no-op (it does not count — this is the difference from sem,
 * see ruling G12 in ADR-0006 §2).
 */
int br_cond_wait(br_cond_t *c, br_mutex_t *m, br_time_t timeout);
int br_cond_signal(br_cond_t *c);    /* wake one (ISR context allowed) */
int br_cond_broadcast(br_cond_t *c); /* wake all */
br_u32 br_cond_waiters(const br_cond_t *c);

/* ==================================================================== spinlock */

/*
 * Spinlock (a new surface not in the design, see the file header note). Semantics:
 *   `br_spinlock_lock` = mask IRQs (nest counting in core) + spin until the lock is acquired; it returns the
 *   interrupt state from before entry, which must be passed unchanged to `br_spinlock_unlock` (the same pairing discipline as br_irq_lock/unlock).
 *   On the single-core prototype the spin bit always succeeds on the first try (nobody else is running), but it makes the
 *   critical section explicit in code and reserves room for multicore.
 */
typedef struct br_spinlock {
    br_u32          magic;
    volatile br_u32 lock;
} br_spinlock_t;

#define BR_SPINLOCK_INIT_VALUE { BR_SYNC_MAGIC_SPIN, 0u }
#define BR_SPINLOCK_DEFINE(name) br_spinlock_t name = BR_SPINLOCK_INIT_VALUE

int br_spinlock_init(br_spinlock_t *s);
br_irq_state_t br_spinlock_lock(br_spinlock_t *s);
/* busy => -EBUSY (does not block, does not mask interrupts; `saved` unchanged) */
int br_spinlock_trylock(br_spinlock_t *s, br_irq_state_t *saved);
void br_spinlock_unlock(br_spinlock_t *s, br_irq_state_t saved);
br_bool br_spinlock_is_locked(const br_spinlock_t *s);
/* Observation: count of spins of at least one full round (always 0 on a single core => it proves contention is really measured here). */
br_u32 br_spinlock_contention(void);

/*
 * The sync suite entry `br_sync_selftest()` is deliberately not declared here. Reason: a selftest entry is not a plugin's
 * external capability, and leaving it in this header would put it in the golden interface surface — turning "change one case" into an interface
 * change (same rationale as ADR-0005 ruling 9 on hooks). Declarations live in `core/selftest/core_selftest.c` and the
 * implementation with the suite in `core/selftest/sync_selftest.c` (ADR-0010).
 */

#endif /* BR_CORE_BR_SYNC_H */
