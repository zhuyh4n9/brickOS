/*
 * brickOS prototype v0.2.0 — synchronization primitive implementation (core): mutex / semaphore / cond / spinlock
 *
 * Design basis: `3-01` §7 (synchronization primitives, lines 117–139), §14 CA-4 (three-state
 *           timeout), INV-1/INV-2 (timeout semantics),
 *           `3-02` §6.4 (`br_irq_lock/unlock` nesting count lives in core), §11.3 (give
 *           semantics in ISR: only change state / link, **never switch inside an ISR**),
 *           `6-01` §3.2/§3.3 (TC-SYNC-* / TC-TIME-*).
 *
 * ## Three load-bearing designs
 *
 * 1. **The only basis for blocking** is `br_sched_block_current(abs_deadline)` (`br_sched.h`):
 *    returns 0 = woken, `-ETIMEDOUT` = deadline reached. This file touches no stacks and
 *    performs no context switches.
 *
 * 2. **Waiter nodes are embedded in the TCB** (`sched_internal.h`'s
 *    `br_waitq_push/pop/remove/len`), so the synchronization path is **allocation-free** —
 *    a lock never fails because "allocation failed". The wait-chain pointer is the object's
 *    `head` field, and nodes reuse the TCB's `wait_next`.
 *
 * 3. **"Check condition + link/unlink wait chain + ownership handoff" all happen inside
 *    the IRQ-disabled critical section**; only the step that actually yields the CPU runs
 *    with interrupts enabled (otherwise ticks (`1/HZ`) cannot arrive and timeouts would
 *    never fire).
 *
 * ## Handshake bits with the scheduler framework (ICB protocol; see ADR-0007 §2.6)
 *
 * Between "finish linking the wait chain in the IRQ-disabled critical section" and
 * "`br_sched_block_current()` sets the thread BLOCKED", there is a window where an ISR can
 * `give`/`signal` and wake this thread. It is closed with the TCB's **existing**
 * `wait_status` (no new field needed):
 *   - the caller sets `self->wait_status = -ETIMEDOUT` inside the critical section and links
 *     the wait chain;
 *   - `br_sched_wake(t)` writes `t->wait_status = 0` first, and only when
 *     `t->state == BLOCKED` sets READY and enqueues ⇒ a wake landing in the window **only
 *     sets the flag, does not enqueue**, so it is not lost;
 *   - `br_sched_block_current()` sees `wait_status == 0` and **does not switch away**,
 *     returning 0 immediately;
 *   - the timeout scan sets `wait_status = -ETIMEDOUT` only for threads with
 *     `state == BLOCKED`.
 * Whether "timeout then handoff" or "handoff then timeout", the order converges to the same
 * winner.
 *
 * ## Error code conventions (frozen)
 *
 * `-EINVAL` (object uninitialized / magic trampled) / `-EPERM` (unlock by non-owner) /
 * `-EBUSY` (spinlock busy with `BR_TIMEOUT_ZERO`) / `-ETIMEDOUT` (all blocking timeouts,
 * INV-1) / `-ENOTSUP` (blocking needed but no thread context). **sem's `take(ZERO)` uses
 * `-ETIMEDOUT`** (it is a "zero deadline", deliberately different from mutex trylock;
 * basis 6-01 TC-SYNC-004, see ADR-0007 §2.3).
 */
#include <br/core/br_error.h>
#include <br/core/br_fault.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_sync.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

/* TCB layout and the four wait-chain primitives are a core-internal contract
 * (sched_internal.h says so itself). Cross-directory relative includes have precedent in
 * this repo (core/src/irq/irq_fault.c -> "../panic_internal.h"). */
#include "../sched/sched_internal.h"

/* =====================================================================
 * shared helpers
 * ===================================================================== */

#define SYNC_MUTEX_OK(m) ((m) != BR_NULL && (m)->magic == BR_SYNC_MAGIC_MUTEX)
#define SYNC_SEM_OK(s)   ((s) != BR_NULL && (s)->magic == BR_SYNC_MAGIC_SEM)
#define SYNC_COND_OK(c)  ((c) != BR_NULL && (c)->magic == BR_SYNC_MAGIC_COND)
#define SYNC_SPIN_OK(s)  ((s) != BR_NULL && (s)->magic == BR_SYNC_MAGIC_SPIN)

/* Relative timeout → absolute deadline. INF passes through ("no deadline" is not
 * "a very distant deadline"). */
static br_time_t sync_deadline(br_time_t timeout)
{
    return (timeout == BR_TIMEOUT_INF) ? BR_TIMEOUT_INF : br_deadline_from_now(timeout);
}

/*
 * Blocking epilogue: when `br_sched_block_current()` returns due to timeout, the waker did
 * not unlink us (it does not even know which chain we are on) ⇒ **we must unlink
 * ourselves**, otherwise a "ghost" node remains and the next give/signal wakes a thread
 * that already returned.
 * On a normal wake the waker already popped us, `br_waitq_remove` returns 0, and the count
 * is left alone.
 */
static int sync_wait_finish(void **head, br_u32 *waiters, int rc)
{
    if (rc == 0) {
        return 0;
    }

    br_thread_t *self = br_task_self();
    br_irq_state_t st = br_irq_lock();

    if (self != BR_NULL && br_waitq_remove(head, self) != 0) {
        if (*waiters > 0u) {
            (*waiters)--;
        }
    }
    br_irq_unlock(st);
    return rc;
}

/*
 * Unlock done **inside the critical section** (the caller already verified it is the owner
 * and interrupts are disabled).
 * With waiters ⇒ **hand ownership over directly** (locked stays 1, owner becomes the woken
 * thread): this avoids the window where the count is released first and a third thread
 * grabs it.
 */
static void sync_mutex_release_locked(br_mutex_t *m)
{
    br_thread_t *w = br_waitq_pop(&m->head);

    if (w != BR_NULL) {
        if (m->waiters > 0u) {
            m->waiters--;
        }
        m->owner = w;              /* handoff */
        br_sched_wake(w);
    } else {
        m->locked = 0u;
        m->owner  = BR_NULL;
    }
}

/* =====================================================================
 * mutex
 * ===================================================================== */

int br_mutex_init(br_mutex_t *m)
{
    if (m == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();
    m->magic   = BR_SYNC_MAGIC_MUTEX;
    m->locked  = 0u;
    m->owner   = BR_NULL;
    m->waiters = 0u;
    m->head    = BR_NULL;
    br_irq_unlock(st);
    return BR_OK;
}

int br_mutex_lock_to(br_mutex_t *m, br_time_t timeout)
{
    if (!SYNC_MUTEX_OK(m)) {
        return BR_ERR(EINVAL);
    }

    br_thread_t   *self = br_task_self();
    br_irq_state_t st   = br_irq_lock();

    if (m->locked == 0u) {                       /* unlocked fast path */
        m->locked = 1u;
        m->owner  = self;
        br_irq_unlock(st);
        return BR_OK;
    }

    if (timeout == BR_TIMEOUT_ZERO) {            /* trylock semantics */
        /* ⚠ This returns `-ETIMEDOUT`, not `-EBUSY`: design `6-01` TC-SYNC-002 states
         * "lock_to(ZERO): unlocked 0 / locked `-ETIMEDOUT`", and INV-1 unifies "cannot
         * wait" to `-ETIMEDOUT` — zero timeout is just "wait 0 microseconds", not another
         * error. The frozen header first said -EBUSY, which was this change's deviation and
         * has been fixed back to the design (ADR-0007 §2.7); spinlock's trylock still uses
         * `-EBUSY` (it is outside the design surface, added by this change, see the
         * br_sync.h header note). */
        br_irq_unlock(st);
        return BR_ERR(ETIMEDOUT);
    }

    if (self == BR_NULL) {                       /* lock is held, must wait, but no thread context */
        br_irq_unlock(st);
        return BR_ERR(ENOTSUP);
    }

    br_waitq_push(&m->head, self);
    m->waiters++;
    self->wait_status = BR_ERR(ETIMEDOUT);    /* ICB: pessimistic default before optimism */
    br_irq_unlock(st);

    const int rc = br_sched_block_current(sync_deadline(timeout));
    /* rc == 0 ⇒ `br_mutex_unlock` already handed ownership to us; do not touch locked/owner here. */
    return sync_wait_finish(&m->head, &m->waiters, rc);
}

int br_mutex_lock(br_mutex_t *m)
{
    return br_mutex_lock_to(m, BR_TIMEOUT_INF);
}

int br_mutex_unlock(br_mutex_t *m)
{
    if (!SYNC_MUTEX_OK(m)) {
        return BR_ERR(EINVAL);
    }

    br_thread_t   *self = br_task_self();
    br_irq_state_t st   = br_irq_lock();

    if ((m->locked == 0u) || (m->owner != self)) {
        br_irq_unlock(st);
        return BR_ERR(EPERM);
    }

    sync_mutex_release_locked(m);
    br_irq_unlock(st);
    return BR_OK;
}

br_bool br_mutex_is_locked(const br_mutex_t *m)
{
    if (!SYNC_MUTEX_OK(m)) {
        return BR_FALSE;
    }
    return (m->locked != 0u) ? BR_TRUE : BR_FALSE;
}

br_thread_t *br_mutex_owner(const br_mutex_t *m)
{
    if (!SYNC_MUTEX_OK(m)) {
        return BR_NULL;
    }
    return m->owner;
}

/* =====================================================================
 * semaphore
 * ===================================================================== */

int br_sem_init(br_sem_t *s, unsigned initial)
{
    if (s == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();
    s->magic   = BR_SYNC_MAGIC_SEM;
    s->count   = (br_u32)initial;
    s->waiters = 0u;
    s->head    = BR_NULL;
    br_irq_unlock(st);
    return BR_OK;
}

int br_sem_take(br_sem_t *s, br_time_t timeout)
{
    if (!SYNC_SEM_OK(s)) {
        return BR_ERR(EINVAL);
    }

    br_thread_t   *self = br_task_self();
    br_irq_state_t st   = br_irq_lock();

    if (s->count > 0u) {
        s->count--;
        br_irq_unlock(st);
        return BR_OK;
    }

    if (timeout == BR_TIMEOUT_ZERO) {            /* zero deadline = expires immediately */
        br_irq_unlock(st);
        return BR_ERR(ETIMEDOUT);
    }

    if (self == BR_NULL) {
        br_irq_unlock(st);
        return BR_ERR(ENOTSUP);
    }

    br_waitq_push(&s->head, self);
    s->waiters++;
    self->wait_status = BR_ERR(ETIMEDOUT);
    br_irq_unlock(st);

    const int rc = br_sched_block_current(sync_deadline(timeout));
    /* rc == 0 ⇒ give already handed the token **directly** to us (count was not added back), so no decrement. */
    return sync_wait_finish(&s->head, &s->waiters, rc);
}

/*
 * ISR-safe (CA-3 whitelist, `3-02` §11.3): only "count + link/handoff + wake flag", no
 * switch. The woken thread runs at the next scheduling point — that is v1 COOP's honest
 * semantics.
 */
int br_sem_give(br_sem_t *s)
{
    if (!SYNC_SEM_OK(s)) {
        return BR_ERR(EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();
    br_thread_t *w = br_waitq_pop(&s->head);

    if (w != BR_NULL) {
        if (s->waiters > 0u) {
            s->waiters--;
        }
        br_sched_wake(w);                        /* direct handoff: count untouched */
    } else {
        s->count++;                              /* nobody waiting ⇒ record it in the count */
    }

    br_irq_unlock(st);
    return BR_OK;
}

br_u32 br_sem_count(const br_sem_t *s)
{
    if (!SYNC_SEM_OK(s)) {
        return 0u;
    }
    return s->count;
}

/* =====================================================================
 * cond
 * ===================================================================== */

int br_cond_init(br_cond_t *c)
{
    if (c == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();
    c->magic   = BR_SYNC_MAGIC_COND;
    c->waiters = 0u;
    c->head    = BR_NULL;
    br_irq_unlock(st);
    return BR_OK;
}

int br_cond_wait(br_cond_t *c, br_mutex_t *m, br_time_t timeout)
{
    if (!SYNC_COND_OK(c) || !SYNC_MUTEX_OK(m)) {
        return BR_ERR(EINVAL);
    }

    br_thread_t *self = br_task_self();
    br_irq_state_t st = br_irq_lock();

    if ((m->locked == 0u) || (m->owner != self)) {   /* cond_wait precondition: hold m */
        br_irq_unlock(st);
        return BR_ERR(EPERM);
    }

    /*
     * Zero-deadline special case: do not link the wait chain. Semantics stay "atomically
     * release + reacquire + return timeout", but without going through
     * `br_sched_block_current` (otherwise whether it times out depends on how the scheduler
     * interprets "deadline already in the past"; here the criterion is brought inside the
     * primitive, so the result is deterministic).
     * This path also needs no thread context (it does not block), so the `self == NULL`
     * check comes after it.
     */
    if (timeout == BR_TIMEOUT_ZERO) {
        sync_mutex_release_locked(m);
        br_irq_unlock(st);
        const int rl = br_mutex_lock(m);
        return (rl != 0) ? rl : BR_ERR(ETIMEDOUT);
    }

    if (self == BR_NULL) {                           /* must block, but no thread context */
        br_irq_unlock(st);
        return BR_ERR(ENOTSUP);
    }

    br_waitq_push(&c->head, self);
    c->waiters++;
    self->wait_status = BR_ERR(ETIMEDOUT);
    sync_mutex_release_locked(m);                    /* atomic: unlock (may hand off to a mutex waiter) */
    br_irq_unlock(st);

    const int rc = br_sched_block_current(sync_deadline(timeout));

    if (rc != 0) {                                   /* timeout: unlink ourselves from the cond wait chain */
        st = br_irq_lock();
        if (br_waitq_remove(&c->head, self) != 0) {
            if (c->waiters > 0u) {
                c->waiters--;
            }
        }
        br_irq_unlock(st);
    }

    /*
     * ★ TC-SYNC-008 criterion: **when returning -ETIMEDOUT due to timeout, m must already
     * be re-held**.
     * Reacquire without a deadline (timeout semantics cover only "waiting", not
     * "re-locking" — standard cond semantics).
     */
    const int rl = br_mutex_lock(m);
    if (rl != 0) {
        return rl;
    }
    return rc;
}

int br_cond_signal(br_cond_t *c)
{
    if (!SYNC_COND_OK(c)) {
        return BR_ERR(EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();
    br_thread_t *w = br_waitq_pop(&c->head);

    if (w != BR_NULL) {
        if (c->waiters > 0u) {
            c->waiters--;
        }
        br_sched_wake(w);
    }
    /* no waiters ⇒ no-op, **no counting** (ruling G12: do not turn cond into sem). */

    br_irq_unlock(st);
    return BR_OK;
}

int br_cond_broadcast(br_cond_t *c)
{
    if (!SYNC_COND_OK(c)) {
        return BR_ERR(EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();

    for (;;) {
        br_thread_t *w = br_waitq_pop(&c->head);
        if (w == BR_NULL) {
            break;
        }
        if (c->waiters > 0u) {
            c->waiters--;
        }
        br_sched_wake(w);
    }

    br_irq_unlock(st);
    return BR_OK;
}

br_u32 br_cond_waiters(const br_cond_t *c)
{
    if (!SYNC_COND_OK(c)) {
        return 0u;
    }
    return c->waiters;
}

/* =====================================================================
 * spinlock (a surface with 0 hits in the design; see the br_sync.h header note and ADR-0007 §2.1)
 * ===================================================================== */

static volatile br_u32 s_spin_contention;   /* number of times the spin loop was entered */

int br_spinlock_init(br_spinlock_t *s)
{
    if (s == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();
    s->magic = BR_SYNC_MAGIC_SPIN;
    s->lock  = 0u;
    br_irq_unlock(st);
    return BR_OK;
}

/*
 * Disable interrupts (nesting count in core) + spin until the lock is acquired, returning
 * the **interrupt state from before entry**.
 * On a single core the spin bit succeeds first try (nobody else is running), but "disable
 * interrupts" is the real mutex; the spin bit makes the critical section explicit in code
 * and reserves the place for multicore (SMP).
 */
br_irq_state_t br_spinlock_lock(br_spinlock_t *s)
{
    const br_irq_state_t st = br_irq_lock();

    if (s == BR_NULL || s->magic != BR_SYNC_MAGIC_SPIN) {
        /* No channel to return an error code; uninitialized/trampled is a kernel bug, not
         * silently "unlocked" (ADR-0007 §2.5). */
        br_panic_bare("spinlock: magic corrupt (s=%p)", (const void *)s);
    }

    if (s->lock != 0u) {
        s_spin_contention++;                 /* on a single core only "locking itself" reaches here */
        while (s->lock != 0u) {
            /* Spin-wait. On multicore this waits for another core; on a single core it is a
             * deadlock, not a normal path. */
        }
    }
    s->lock = 1u;
    return st;
}

int br_spinlock_trylock(br_spinlock_t *s, br_irq_state_t *saved)
{
    if (!SYNC_SPIN_OK(s)) {
        return BR_ERR(EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();

    if (s->lock != 0u) {
        br_irq_unlock(st);                   /* busy: do not hold the lock, do not modify *saved */
        return BR_ERR(EBUSY);
    }

    s->lock = 1u;
    if (saved != BR_NULL) {
        *saved = st;
    }
    return BR_OK;
}

void br_spinlock_unlock(br_spinlock_t *s, br_irq_state_t saved)
{
    if (SYNC_SPIN_OK(s)) {
        s->lock = 0u;
    }
    /* Even with a corrupt magic, hand the interrupt state back — otherwise one dirty
     * object would leave this core with interrupts disabled forever. */
    br_irq_unlock(saved);
}

br_bool br_spinlock_is_locked(const br_spinlock_t *s)
{
    if (!SYNC_SPIN_OK(s)) {
        return BR_FALSE;
    }
    return (s->lock != 0u) ? BR_TRUE : BR_FALSE;
}

br_u32 br_spinlock_contention(void)
{
    return s_spin_contention;
}

