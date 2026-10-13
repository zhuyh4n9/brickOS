/*
 * Deferred work (bottom half / workqueue): the ISR -> bh path. The queue and hot-path submit live
 * in core and the drain runs at IRQ exit (after eoi, before ERET), so no scheduler is needed; the
 * delay is bounded by the next interrupt exit.
 *
 * bh context = "already eoi, not yet ERET", on the interrupted thread's own stack, with PSTATE.I
 * set, br_irq_in_isr() == 0 and br_work_in_bh() == 1. Inside bh, blocking/yield are forbidden
 * (-EPERM), ending the thread panics, long-held locks are forbidden and items must be short.
 */
#ifndef BR_CORE_BR_WORK_H
#define BR_CORE_BR_WORK_H

#include <br/core/br_types.h>

/* Queue depth (compile-time static upper bound; 16 x (fn,arg) = 256 B at 4 KiB page scale).
 * Full => br_work_submit returns -EAGAIN, never a silent drop (caller must handle it). */
#define BR_WORKQ_DEPTH 16u

/* Execution cap per IRQ exit, in items. The drain runs with interrupts off, so without a cap one
 * interrupt could stretch latency arbitrarily; the rest waits for the next interrupt exit
 * (prototype tick = 1/HZ, default 5 ms), bounding a full drain at ceil(pending / 4) x tick. */
#define BR_WORK_BH_BUDGET 4u

/* Work-item shape (v1 has no work object/cancel: bare fn + arg submit). */
typedef void (*br_work_fn_t)(void *arg);

/*
 * Submit one work item; `fn(arg)` runs exactly once in **bh context**, at the next interrupt exit.
 * ISR-safe (one L2 critical section + one slot write). Returns 0 enqueued, -EINVAL if fn is NULL,
 * -EAGAIN if the queue is full -- the only failure the caller must handle: if this submit is the
 * sole carrier of an interrupt event (e.g. SLOW-domain demux), -EAGAIN must roll back the actions
 * already taken or the event is lost forever. Host/self-tests may call br_work_drain() to run it
 * at once.
 */
int br_work_submit(br_work_fn_t fn, void *arg);

/* Take and run at most `max_items` items; returns how many ran. Called by core's IRQ exit (with
 * BR_WORK_BH_BUDGET) and by host/self-tests. Not reentrant: called from inside bh it returns 0
 * so a work item that submits again cannot eat the stack. */
br_u32 br_work_drain(br_u32 max_items);

/* Whether we are in bh context (BR_TRUE inside a work item). */
br_bool br_work_in_bh(void);

/* Observation snapshot; diagnostics only, no golden layout promise. */
typedef struct br_work_stat {
    br_u32 submitted;      /* cumulative successful enqueues */
    br_u32 ran;            /* cumulative executions */
    br_u32 dropped_full;   /* rejections because the queue was full (-EAGAIN) */
    br_u32 dropped_invalid;/* rejections because fn == NULL (-EINVAL) */
    br_u32 pending;        /* items currently pending */
    br_u32 max_pending;    /* historical max pending (evidence the queue depth is enough) */
    br_u32 drain_passes;   /* number of drain calls */
} br_work_stat_t;

void br_work_stats_get(br_work_stat_t *out);

/* The selftest entry is declared in core/c_core/selftest/, not here (test surface, not plugin API). */

#endif /* BR_CORE_BR_WORK_H */
