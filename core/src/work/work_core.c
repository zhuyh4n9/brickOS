/*
 * brickOS prototype v0.2.0 — deferred work (bottom half / workqueue, core) implementation
 *
 * Design and rulings: file header of `core/include/br/core/br_work.h` + `docs/decisions/0011-...md`.
 * Design basis: `3-01` §5 (deferred work = bottom half; static queue depth, full ⇒ -EAGAIN),
 *           `3-02` §11.1 (ISR → deferred work is the first of four interaction paths), §11.4.1
 *           (semantics of `br_irq_disable` in BH form: suppress, do not cancel/replay — the
 *           suppression decision is in irq_core.c; this file only supplies the fact that a work
 *           item runs in bh + the `br_work_in_bh()` predicate),
 *           §9.4 (SLOW domain demux is its first real consumer), IR-10 (a failed submit must roll back).
 *
 * Three immovable properties (implementation and cases rest on them):
 *   W1 **bounded**: the queue is a compile-time static ring (`BR_WORKQ_DEPTH`), no allocation,
 *      no growth; full ⇒ -EAGAIN.
 *   W2 **ISR-safe**: the only shared mutable state (ring + counts) is changed only inside an
 *      `br_irq_lock()` L2 critical section; the submit path takes no lock, reads no clock, logs
 *      nothing, calls nothing back (callbacks always happen in drain).
 *   W3 **non-reentrant**: `br_work_drain` raises the "in bh" flag while running work items; a
 *      work item that calls `br_work_drain` again (or re-submits for the outer loop to
 *      continue) is safe — stack depth does not grow with queue length.
 *
 * Why drain runs with **interrupts disabled**: it happens at IRQ exit (after eoi, before ERET),
 * where `PSTATE.I == 1` was fixed by hardware on exception entry. We **deliberately do not open**
 * it (opening it would force reasoning about exception nesting IR-6 and ISR stack-frame
 * ownership), so the bh contract is "short + non-blocking" (see the corollary in the header
 * comment). This is also why §11.4.1's "the suppression decision must be atomic" holds for
 * free: bh and ISR are mutually exclusive by nature.
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_types.h>
#include <br/core/br_work.h>

/* ------------------------------------------------------------------ static ring */

typedef struct br_work_item {
    br_work_fn_t fn;
    void        *arg;
} br_work_item_t;

static br_work_item_t s_q[BR_WORKQ_DEPTH];
static br_u32         s_head;    /* dequeue cursor (consumer) */
static br_u32         s_tail;    /* enqueue cursor (producer) */
static br_u32         s_count;   /* items currently pending */

static br_bool        s_in_bh;   /* W3: true while drain runs work items */

/* Diagnostic counters (not used in decisions; exposed by br_work_stats_get) */
static br_u32 s_submitted;
static br_u32 s_ran;
static br_u32 s_dropped_full;
static br_u32 s_dropped_invalid;
static br_u32 s_max_pending;
static br_u32 s_drain_passes;

/* ------------------------------------------------------------------ submit */

int br_work_submit(br_work_fn_t fn, void *arg)
{
    /* A bad argument and a full queue are two different failures: the former is a **call-site
     * bug**, the latter a **capacity budget** (IR-10 requires the caller to roll back the
     * latter). Counted separately so a bug is not misread as "budget exhausted". */
    if (fn == BR_NULL) {
        s_dropped_invalid++;
        return BR_ERR(EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();   /* W2: mutually exclusive with drain's dequeue */

    if (s_count >= (br_u32)BR_WORKQ_DEPTH) {
        br_irq_unlock(st);
        s_dropped_full++;
        return BR_ERR(EAGAIN);              /* W1: never silently dropped */
    }

    s_q[s_tail].fn  = fn;
    s_q[s_tail].arg = arg;
    s_tail = (s_tail + 1u) % (br_u32)BR_WORKQ_DEPTH;
    s_count++;
    s_submitted++;
    if (s_count > s_max_pending) {
        s_max_pending = s_count;               /* evidence for "is the queue deep enough" */
    }

    br_irq_unlock(st);
    return 0;
}

/* ------------------------------------------------------------------ execution */

br_u32 br_work_drain(br_u32 max_items)
{
    /* W3: non-reentrant — already in bh means this call came from inside a work item; the outer loop continues. */
    if (s_in_bh != BR_FALSE) {
        return 0u;
    }
    if (max_items == 0u) {
        return 0u;
    }

    s_in_bh = BR_TRUE;
    s_drain_passes++;

    br_u32 ran = 0u;

    while (ran < max_items) {
        br_work_fn_t fn;
        void        *arg;

        /* Dequeue happens inside the L2 critical section (L2 means "this short span does not race with interrupts"). */
        const br_irq_state_t st = br_irq_lock();

        if (s_count == 0u) {
            br_irq_unlock(st);
            break;
        }

        fn  = s_q[s_head].fn;
        arg = s_q[s_head].arg;
        s_q[s_head].fn  = BR_NULL;             /* clear the slot: no stale pointer from the previous occupant on reuse */
        s_q[s_head].arg = BR_NULL;
        s_head = (s_head + 1u) % (br_u32)BR_WORKQ_DEPTH;
        s_count--;

        br_irq_unlock(st);

        /* ★ Run only after leaving the critical section: the work item must itself be able to
         *   take L2 (else self-lock), and must stay observable (counters, trace). While it
         *   runs `s_in_bh` is true ⇒ blocking/yielding APIs refuse. */
        if (fn != BR_NULL) {
            fn(arg);
            s_ran++;
        }
        ran++;
    }

    s_in_bh = BR_FALSE;
    return ran;
}

br_bool br_work_in_bh(void)
{
    return s_in_bh;
}

void br_work_stats_get(br_work_stat_t *out)
{
    if (out == BR_NULL) {
        return;
    }

    out->submitted       = s_submitted;
    out->ran             = s_ran;
    out->dropped_full    = s_dropped_full;
    out->dropped_invalid = s_dropped_invalid;
    out->pending         = s_count;
    out->max_pending     = s_max_pending;
    out->drain_passes    = s_drain_passes;
}
