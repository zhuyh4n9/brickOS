/*
 * sched/coop — cooperative scheduler (coop) implementation
 *
 * Design basis: `3-03-sched.md` §1 ("semantics in core, policy in the plugin")/§2;
 *           `3-01-core-api-list.md` §5.1 (br_sched_ops registration point)/§6 (tasks and threads);
 *           `3-02-int.md` §11.3 (coop IRQ-scheduling semantics: an IRQ return must resume the interrupted thread);
 *           rulings and the ops table: `docs/decisions/0006-scheduler-framework.md`.
 *
 * ## All the policy this plugin owns
 *   - **ready queue = FIFO** (singly linked list, node in the plugin-private tail — zero
 *     allocation, same shape as CA-2);
 *   - `pick_next` dequeues; empty ⇒ BR_NULL ⇒ core goes idle/WFI (3-02 §11.1's L2 critical section);
 *   - **no tick policy** (`on_tick = BR_NULL`): coop does not time-slice — there is no
 *     preemption, and round-robin semantics come only from voluntary yields in
 *     `br_task_yield()`. That is the boundary between coop and preempt.
 *   - `sleep_until` hook returns 0: timeout wakeups are **owned solely by core's `wake_at` table**;
 *     the slot is kept here for preempt/tt to attach their own timer wheel/schedule table.
 *
 * ## State-machine split with core (ADR-0006 §1's I2)
 *   The plugin **changes queue membership only**; `state` is always changed by core. `pick_next`
 *   only dequeues; setting RUNNING is core's job — that keeps a single source of truth.
 *
 * ## ISR safety
 *   Every change to the ready queue happens inside an `br_irq_lock()/br_irq_unlock()` L2 critical
 *   section: the timer ISR wakes threads (⇒ enqueue) in `br_sched_on_tick()`, while the thread
 *   side may be mid-`pick_next` — without this critical section the ISR could tear the queue
 *   "half way through the tail update".
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_sched.h>
#include <br/core/br_types.h>

#include <br/sched/coop.h>

/* plugin-private tail (hangs after the common TCB header; size reported to core's tcb_size). */
struct coop_priv {
    struct br_thread *ready_next;
};

static struct br_thread *s_head;   /* queue head (next to run) */
static struct br_thread *s_tail;   /* queue tail (where yield re-enqueues) */
static br_u32            s_len;

static br_sched_ops_t    s_ops;    /* not const: tcb_size is computed at runtime (br_sched_tcb_size()) */

static struct coop_priv *priv_of(br_thread_t *t)
{
    return (struct coop_priv *)br_thread_priv(t);
}

/* ------------------------------------------------------------------ ready queue */

static int coop_thread_ready(br_thread_t *t)
{
    if (t == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();

    struct coop_priv *p = priv_of(t);
    p->ready_next = BR_NULL;

    if (s_tail == BR_NULL) {
        s_head = t;
    } else {
        priv_of(s_tail)->ready_next = t;
    }
    s_tail = t;
    s_len++;

    br_irq_unlock(st);
    return 0;
}

/*
 * RUNNING → BLOCKED hook. Under coop the RUNNING thread is **not in the queue anyway**, so
 * this is normally a no-op; a defensive linear unlink is still done: if the plugin is misused
 * (double ready), no ghost node "not runnable yet still queued" is left behind.
 */
static void coop_thread_block(br_thread_t *t)
{
    if (t == BR_NULL) {
        return;
    }

    const br_irq_state_t st = br_irq_lock();

    struct br_thread **pp = &s_head;
    struct br_thread  *prev = BR_NULL;
    while (*pp != BR_NULL) {
        if (*pp == t) {
            *pp = priv_of(t)->ready_next;
            if (s_tail == t) {
                s_tail = prev;
            }
            if (s_head == BR_NULL) {
                s_tail = BR_NULL;
            }
            s_len--;
            break;
        }
        prev = *pp;
        pp = &priv_of(*pp)->ready_next;
    }
    priv_of(t)->ready_next = BR_NULL;

    br_irq_unlock(st);
}

/* Dequeue. Does **not** change state (core sets RUNNING); empty ⇒ BR_NULL ⇒ core idle. */
static br_thread_t *coop_pick_next(void)
{
    br_thread_t *t;

    const br_irq_state_t st = br_irq_lock();

    t = s_head;
    if (t != BR_NULL) {
        s_head = priv_of(t)->ready_next;
        if (s_head == BR_NULL) {
            s_tail = BR_NULL;
        }
        priv_of(t)->ready_next = BR_NULL;
        s_len--;
    }

    br_irq_unlock(st);
    return t;
}

/*
 * Timeout-registration hook. Core's `wake_at` table is the single source of truth (expiries
 * are scanned on the periodic tick); returning 0 here means "registration accepted", and the
 * slot is kept for preempt/tt to attach their own time structures.
 */
static int coop_sleep_until(br_thread_t *t, br_time_t abs_us)
{
    (void)t;
    (void)abs_us;
    return 0;
}

/* ------------------------------------------------------------------ lifecycle */

int coop_early_init(void)
{
    s_head = BR_NULL;
    s_tail = BR_NULL;
    s_len  = 0u;

    s_ops.name         = "sched/coop";
    s_ops.kind         = BR_SCHED_KIND_COOP;
    s_ops.tcb_size     = (br_u32)(br_sched_tcb_size() + sizeof(struct coop_priv));
    s_ops.thread_ready = coop_thread_ready;
    s_ops.thread_block = coop_thread_block;
    s_ops.pick_next    = coop_pick_next;
    s_ops.on_tick      = BR_NULL;      /* coop has no time slice: no tick policy */
    s_ops.sleep_until  = coop_sleep_until;
    s_ops.idle         = BR_NULL;      /* default = core's L2 critical section + WFI (3-02 §11.1) */

    br_sched_register(&s_ops);         /* exactly once; a second registration = core panic */
    return 0;
}

int coop_init(void)
{
    return 0;
}

int coop_start(void)
{
    return 0;
}
