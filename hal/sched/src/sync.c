/*
 * Synchronization primitives: mutex / semaphore / cond / spinlock. Blocking rests solely on
 * br_sched_block_current() (0 = woken, -ETIMEDOUT = deadline); waiter nodes are embedded in the
 * TCB, so the sync path never allocates. Condition check, chain link/unlink and ownership handoff
 * run with IRQs disabled; only the yield itself runs with IRQs on, or ticks could not arrive.
 */
#include <br/core/br_error.h>
#include <br/hal/br_fault.h>
#include <br/hal/br_irq.h>
#include <br/core/br_log.h>
#include <br/hal/br_sched.h>
#include <br/hal/br_sync.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

/* TCB layout and the four wait-chain primitives are a core-internal contract (sched_internal.h). */
#include "sched_internal.h"

/* ---- shared helpers ---- */

#define SYNC_MUTEX_OK(m) ((m) != BR_NULL && (m)->magic == BR_SYNC_MAGIC_MUTEX)
#define SYNC_SEM_OK(s)   ((s) != BR_NULL && (s)->magic == BR_SYNC_MAGIC_SEM)
#define SYNC_COND_OK(c)  ((c) != BR_NULL && (c)->magic == BR_SYNC_MAGIC_COND)
#define SYNC_SPIN_OK(s)  ((s) != BR_NULL && (s)->magic == BR_SYNC_MAGIC_SPIN)

/* Relative timeout -> absolute deadline; INF means "no deadline", not a distant one. */
static br_time_t sync_deadline(br_time_t timeout)
{
    return (timeout == BR_TIMEOUT_INF) ? BR_TIMEOUT_INF : br_deadline_from_now(timeout);
}

/*
 * Blocking epilogue: on timeout the waker did not unlink us (it does not know our chain), so we
 * unlink ourselves; otherwise a ghost node makes the next give/signal wake a returned thread.
 * On a normal wake the waker already popped us and br_waitq_remove returns 0.
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
 * Unlock inside the critical section (caller verified ownership, IRQs off). With waiters, hand
 * ownership over directly (locked stays 1, owner = woken thread), so no window opens for a third
 * thread to grab the lock.
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

/* ---- mutex ---- */

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
        /* -ETIMEDOUT, not -EBUSY: lock_to(ZERO) means "wait 0 us" and INV-1 unifies "cannot
         * wait" to -ETIMEDOUT (TC-SYNC-002); spinlock trylock still uses -EBUSY. */
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
    /* rc == 0 ⇒ `br_mutex_unlock` already handed ownership to us; do not touch locked/owner
     * here. */
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

/* ---- semaphore ---- */

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
    /* rc == 0 ⇒ give already handed the token directly to us (count was not added back), so no
     * decrement. */
    return sync_wait_finish(&s->head, &s->waiters, rc);
}

/* ISR-safe: only count + link/handoff + wake flag, no switch; the woken thread runs at the next
 * scheduling point (v1 COOP semantics). */
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

/* ---- cond ---- */

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
     * Zero deadline: do not link the wait chain. Still "release + reacquire + timeout", but the
     * criterion lives in the primitive rather than in br_sched_block_current's reading of a past
     * deadline, so the result is deterministic; no thread context is needed either.
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

    /* TC-SYNC-008: on -ETIMEDOUT, m must already be re-held; reacquire without a deadline
     * (timeout covers waiting, not re-locking). */
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
    /* no waiters ⇒ no-op (G12: do not turn cond into sem). */

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

/* ---- spinlock (outside the design surface; see br_sync.h) ---- */

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

/* Disable IRQs (nesting count in core) and spin until acquired; returns the pre-entry IRQ state.
 * Single core: the spin bit always succeeds first try, but disabling IRQs is the real mutex. */
br_irq_state_t br_spinlock_lock(br_spinlock_t *s)
{
    const br_irq_state_t st = br_irq_lock();

    if (s == BR_NULL || s->magic != BR_SYNC_MAGIC_SPIN) {
        /* No error channel; uninitialized/trampled magic is a kernel bug, not "unlocked". */
        br_panic_bare("spinlock: magic corrupt (s=%p)", (const void *)s);
    }

    if (s->lock != 0u) {
        s_spin_contention++;                 /* on a single core only "locking itself" reaches here */
        while (s->lock != 0u) {
            /* Multicore: waits for another core; single core: a self-deadlock, not a normal
             * path. */
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
    /* Return the IRQ state even with corrupt magic, so one dirty object cannot leave IRQs off. */
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

