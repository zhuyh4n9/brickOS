/*
 * Deferred work (bottom half / workqueue, core). Bounded static ring (BR_WORKQ_DEPTH), no
 * allocation, full => -EAGAIN; ISR-safe (shared state changes only inside an L2 critical section);
 * non-reentrant (br_work_drain is a no-op while already in bh). Drain runs at the IRQ exit with
 * IRQs disabled, so items must be short and non-blocking.
 */
#include <br/core/br_error.h>
#include <br/hal/br_irq.h>
#include <br/core/br_types.h>
#include <br/hal/br_work.h>

/* ---- static ring ---- */

typedef struct br_work_item {
    br_work_fn_t fn;
    void        *arg;
} br_work_item_t;

static br_work_item_t s_q[BR_WORKQ_DEPTH];
static br_u32         s_head;    /* dequeue cursor (consumer) */
static br_u32         s_tail;    /* enqueue cursor (producer) */
static br_u32         s_count;   /* items currently pending */

static br_bool        s_in_bh;   /* true while drain runs work items */

/* Diagnostic counters (not used in decisions; exposed by br_work_stats_get) */
static br_u32 s_submitted;
static br_u32 s_ran;
static br_u32 s_dropped_full;
static br_u32 s_dropped_invalid;
static br_u32 s_max_pending;
static br_u32 s_drain_passes;

/* ---- submit ---- */

int br_work_submit(br_work_fn_t fn, void *arg)
{
    /* Bad argument (call-site bug) and full queue (capacity budget) are different failures, counted
     * separately; IR-10 requires the caller to roll back the latter. */
    if (fn == BR_NULL) {
        s_dropped_invalid++;
        return BR_ERR(EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();   /* mutually exclusive with drain's dequeue */

    if (s_count >= (br_u32)BR_WORKQ_DEPTH) {
        br_irq_unlock(st);
        s_dropped_full++;
        return BR_ERR(EAGAIN);              /* never silently dropped */
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

/* ---- execution ---- */

br_u32 br_work_drain(br_u32 max_items)
{
    /* Non-reentrant: already in bh means this call came from inside a work item; the outer loop
     * continues. */
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

        /* Dequeue inside L2 so it does not race with interrupts. */
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

        /* Run after leaving the critical section: the item may take L2 itself. While it runs
         * s_in_bh is true, so blocking/yielding APIs refuse. */
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
