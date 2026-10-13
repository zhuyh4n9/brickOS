/*
 * sched/rr -- round-robin preemptive scheduler: FIFO ready queue with nodes in the plugin-private
 * tail (zero allocation); pick_next dequeues and reloads the thread's slice.
 * on_tick decrements the running thread's slice and on exhaustion calls
 * br_sched_request_resched() -- the only difference from coop; the switch itself is core's at the
 * IRQ exit. The plugin touches queue membership and its private tail only; state is core's.
 * Queue changes hold br_irq_lock() (the timer ISR can enqueue in br_sched_on_tick()); on_tick runs
 * in the ISR and reads only br_task_self()/br_task_state() and the private tail.
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_sched.h>
#include <br/core/br_types.h>

#include <br/sched/rr.h>

/* Plugin-private tail (after the TCB common header; size reported to core as `tcb_size`). */
struct rr_priv {
    struct br_thread *ready_next;  /* ready-queue node (zero allocation) */
    br_u32            slice_left;  /* remaining slice in ticks; 0 = due for rotation */
    br_u32            turns;       /* observation: times scheduled in (diagnostics/self-test) */
};

static struct br_thread *s_head;   /* head (next thread to run) */
static struct br_thread *s_tail;   /* tail (where preempted/yielding threads rejoin) */
static br_u32            s_len;

static br_sched_ops_t    s_ops;    /* non-const: tcb_size is set at runtime */

static struct rr_priv *priv_of(br_thread_t *t)
{
    return (struct rr_priv *)br_thread_priv(t);
}

/* ------------------------------------------------------------------ ready queue */

static int rr_thread_ready(br_thread_t *t)
{
    if (t == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();

    struct rr_priv *p = priv_of(t);
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
static void rr_thread_block(br_thread_t *t)
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

/*
 * Dequeue + load the time slice. Loading here (not in thread_ready) keeps a yield from handing out
 * a free full slice. Does not change state; empty queue => BR_NULL => core idle.
 */
static br_thread_t *rr_pick_next(void)
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

        struct rr_priv *p = priv_of(t);
        p->slice_left = RR_SLICE_TICKS;   /* hand out a full time slice */
        p->turns++;
    }

    br_irq_unlock(st);
    return t;
}

/*
 * Tick policy = this plugin's entire preemption policy.
 *
 * Called from core's br_sched_on_tick() (= platform timer ISR), so it must be ISR-safe: it writes
 * only its own private tail and core's need_resched bit; no locks, no stack switch, no callbacks.
 *
 * Decrement every tick; at zero request one preemption (core consumes it at this IRQ exit). If
 * nobody else can run, core's pick_next picks the same thread and reloads its slice, so no
 * pointless stack switch happens when uncontended.
 */
static void rr_on_tick(void)
{
    br_thread_t *cur = br_task_self();

    if (cur == BR_NULL) {
        return;
    }
    /* Only time threads that are really running (outside RUNNING there is no slice). */
    if (br_task_state(cur) != BR_TASK_RUNNING) {
        return;
    }

    struct rr_priv *p = priv_of(cur);

    if (p->slice_left == 0u) {
        return;                            /* already requested; consumed at IRQ exit */
    }

    p->slice_left--;
    if (p->slice_left == 0u) {
        br_sched_request_resched();         /* the only policy exit */
    }
}

/*
 * Timeout registration: core's wake_at table owns expiry scanning; returning 0 means the
 * registration was accepted.
 */
static int rr_sleep_until(br_thread_t *t, br_time_t abs_us)
{
    (void)t;
    (void)abs_us;
    return 0;
}

/* ------------------------------------------------------------------ lifecycle */

int rr_early_init(void)
{
    s_head     = BR_NULL;
    s_tail     = BR_NULL;
    s_len      = 0u;

    s_ops.name         = "sched/rr";
    s_ops.kind         = BR_SCHED_KIND_PREEMPT;
    s_ops.tcb_size     = (br_u32)(br_sched_tcb_size() + sizeof(struct rr_priv));
    s_ops.thread_ready = rr_thread_ready;
    s_ops.thread_block = rr_thread_block;
    s_ops.pick_next    = rr_pick_next;
    s_ops.on_tick      = rr_on_tick;       /* the only difference from coop */
    s_ops.sleep_until  = rr_sleep_until;
    s_ops.idle         = BR_NULL;          /* default = core's WFI critical section */

    br_sched_register(&s_ops);             /* exactly once; a second registration = core panic */
    return 0;
}

int rr_init(void)
{
    return 0;
}

int rr_start(void)
{
    return 0;
}
