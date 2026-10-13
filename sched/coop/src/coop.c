/*
 * sched/coop -- cooperative scheduler. Ready queue = FIFO singly linked list, nodes in the
 * plugin-private tail (zero allocation); pick_next only dequeues -- core sets RUNNING, so the
 * plugin changes queue membership only, never thread state. on_tick = BR_NULL: no time slicing,
 * so round-robin comes only from voluntary yields in br_task_yield().
 * Every queue change runs in an br_irq_lock()/br_irq_unlock() critical section: the timer ISR can
 * enqueue a woken thread in br_sched_on_tick() while a thread is mid-pick_next.
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_sched.h>
#include <br/core/br_types.h>

#include <br/sched/coop.h>

/* Plugin-private tail (after the common TCB header; size reported to core's tcb_size). */
struct coop_priv {
    struct br_thread *ready_next;
};

static struct br_thread *s_head;   /* queue head (next to run) */
static struct br_thread *s_tail;   /* queue tail (where yield re-enqueues) */
static br_u32            s_len;

static br_sched_ops_t    s_ops;    /* not const: tcb_size is set at runtime */

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

/* RUNNING -> BLOCKED hook: normally a no-op (the RUNNING thread is not queued); the defensive
 * linear unlink keeps plugin misuse (double ready) from leaving a ghost node. */
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

/* Dequeue. Does not change state (core sets RUNNING); empty => BR_NULL => core idle. */
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
 * Timeout-registration hook. Core's wake_at table is the single source of truth (expiries are
 * scanned on the periodic tick); returning 0 means "registration accepted". The slot is kept for
 * preempt/tt to attach their own time structures.
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
    s_ops.on_tick      = BR_NULL;      /* no time slice: no tick policy */
    s_ops.sleep_until  = coop_sleep_until;
    s_ops.idle         = BR_NULL;      /* default = core's WFI critical section */

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
