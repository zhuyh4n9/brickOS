/*
 * sched/rr — round-robin **preemptive** scheduler implementation
 *
 * Basis: `3-03-sched.md` §1 ("semantics in core, policy in plugins")/§2;
 *        `3-01-core-api-list.md` §5.1 (br_sched_ops registration point)/§2.1 (coop vs preempt
 *        boundary); `3-02-int.md` §11.2 (preemption seam at the IRQ exit)/§11.3 (v1 coop never
 *        preempts ⇒ this file is exactly the "the next IRQ exit may switch" shape)/§12.4 (bh vs
 *        scheduling order); item-by-item ruling and evidence:
 *        `docs/decisions/0011-round-robin-bh-workqueue.md`.
 *
 * ## All policy owned by this plugin
 *   - **ready queue = FIFO** (singly linked list, nodes in the plugin's private tail — zero
 *     allocation, same shape as coop);
 *   - `pick_next` dequeues **and reloads that thread's time slice**; empty ⇒ BR_NULL ⇒ core runs
 *     idle/WFI (the L2 critical section of 3-02 §11.1, in core);
 *   - `on_tick`: decrement the current thread's slice, **on exhaustion call
 *     `br_sched_request_resched()`** — the **only** difference from coop (the stack switch itself
 *     is still done by core at the IRQ exit, see below);
 *   - `sleep_until` hook returns 0: timeout wakeups are owned centrally by core's `wake_at` table;
 *   - `idle = BR_NULL`: default is core's WFI critical section.
 *
 * ## Split of duties with core (ADR-0006 §1 I2 + ADR-0011)
 *   The plugin **only touches queue membership + its own private tail**; `state` is always changed
 *   by core (I2). "When to switch" belongs to the plugin (time slice), "how to switch" to core:
 *     `br_sched_request_resched()` only sets a bit; core, at the **IRQ exit (after eoi, before
 *     ERET)**, puts the current thread back on the tail, calls `pick_next` and, if needed,
 *     `br_sched_switch_to`.
 *   ⇒ There is **not one line of switch code here, nor one line changing state** — that is exactly
 *     what cutting "mechanism/policy" apart looks like: switching to `sched/coop` is just picking
 *     another plugin, with no core change.
 *
 * ## ISR safety
 *   Every ready-queue change happens inside the L2 critical section of `br_irq_lock()/
 *   br_irq_unlock()`: the timer ISR can wake a thread in `br_sched_on_tick()` (⇒ enqueue) while a
 *   thread may be halfway through `pick_next` — without that critical section the queue would be
 *   torn by the ISR in the middle of a "tail update". `on_tick` itself runs in the ISR; it only
 *   reads `br_task_self()`/`br_task_state()` (core read-only surface) and its own private tail.
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_sched.h>
#include <br/core/br_types.h>

#include <br/sched/rr.h>

/* Plugin private tail (placed after the TCB common header; size reported to core as `tcb_size`). */
struct rr_priv {
    struct br_thread *ready_next;  /* ready-queue node (zero allocation) */
    br_u32            slice_left;  /* remaining slice (ticks; 0 = due for rotation) */
    br_u32            turns;       /* observation: times scheduled in (diagnostics/self-test) */
};

static struct br_thread *s_head;   /* head (next thread to run) */
static struct br_thread *s_tail;   /* tail (where preempted/yielding threads rejoin) */
static br_u32            s_len;

static br_sched_ops_t    s_ops;    /* non-const: tcb_size is computed at runtime (br_sched_tcb_size()) */

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

/*
 * RUNNING → BLOCKED hook. Under RR the RUNNING thread is **not in the queue anyway**, so this is
 * normally a no-op; it still does a defensive linear unlink so that plugin misuse (double ready)
 * cannot leave a ghost node that is "not runnable yet still in the queue".
 */
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
 * Dequeue **+ load the time slice**. Why load here (and not in `thread_ready`):
 *   `thread_ready` is the common "enter ready" entry (create / wake / yield / preempted back to
 *   the queue); resetting the slice there would give away a free full slice on every yield.
 *   Loading at the moment of **being picked** makes "one thread takes one full slice per turn"
 *   depend on a single place.
 * Does not change `state` (core sets RUNNING); empty queue ⇒ BR_NULL ⇒ core idle.
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
        p->slice_left = RR_SLICE_TICKS;   /* ★ your turn: hand out a full time slice */
        p->turns++;
    }

    br_irq_unlock(st);
    return t;
}

/*
 * tick policy = this plugin's entire "preemption policy".
 *
 * Called by core's `br_sched_on_tick()` (= platform timer ISR), so it **must be ISR-safe**:
 * it only writes its own private tail and core's need_resched bit; no locks, no stack switch,
 * no callbacks.
 *
 * Semantics: decrement every tick; on reaching zero ⇒ request one preemption (core consumes it at
 * this IRQ exit). If nobody else can run right now, core's `pick_next` picks the same thread again
 * and reloads its slice — so "no pointless stack switch when uncontended" also comes free from core.
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
        return;                            /* already requested; await consumption at the IRQ exit */
    }

    p->slice_left--;
    if (p->slice_left == 0u) {
        br_sched_request_resched();         /* ← the only policy exit */
    }
}

/*
 * Timeout registration hook. Core's `wake_at` table is the single source of truth (it scans for
 * expiry on each periodic tick); returning 0 here means "registration accepted", and the slot is
 * left for a future timing wheel to hang its own structures on.
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
    s_ops.on_tick      = rr_on_tick;       /* ★ the only difference from coop */
    s_ops.sleep_until  = rr_sleep_until;
    s_ops.idle         = BR_NULL;          /* default = core's L2 critical section + WFI (3-02 §11.1) */

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
