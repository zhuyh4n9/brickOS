/*
 * brickOS prototype v0.2.0 — deferred work (bottom half / workqueue, core): the only proper ISR → bh path
 *
 * Basis: `3-01-core-api-list.md` §5 ("deferred work (br-sched group; bottom half)"),
 *        §1 API overview (`br_work_submit` is on the ISR-safe whitelist), §11 CA-3;
 *        `3-02-int.md` §11.1 (path one: ISR → deferred work), §11.4.1 (`br_irq_disable` semantics in
 *        the BH shape: suppress, neither cancel nor replay), §12.1/§12.4 (dispatch shape and
 *        execution time), §9.4 (SLOW cascade domain demux goes through bh), IR-10 (a failed submit
 *        must roll back). Item-by-item ruling:
 *        `docs/decisions/0011-round-robin-bh-workqueue.md`.
 *
 * ## Ownership (consistent with the other mechanism/policy splits in this repo)
 *
 * Design `3-01` §5 puts the work queue **implementation** in the scheduling plugin (because "when
 * bh runs" is policy). By user ruling this prototype puts the **whole workqueue in core**:
 *   - the queue itself (bounded static ring) and `br_work_submit` are **mechanism** (hot path,
 *     called from within ISRs);
 *   - execution time = the **IRQ exit** (after eoi, before ERET), so it does not depend on whether
 *     a scheduler is registered — the same independence as `br_sched_irq_epilogue()` (§11.2's seam
 *     must exist already in Stage 1).
 * Cost (recorded honestly): design §11.3's "ISR → bh deferral has no upper bound" does **not** hold
 * in this prototype — we provide a **bounded** delay (the next interrupt exit), see the
 * `br_work_submit` comment and ADR-0011 §3.
 *
 * ## What bh context is (one sentence)
 *   The short stretch "already eoi, not yet ERET": on the **interrupted thread's own stack**,
 *   `PSTATE.I == 1`, `br_irq_in_isr() == 0` (the ISR has returned), `br_work_in_bh() == 1`.
 *   Consequences (**contract**):
 *     - inside bh, blocking is **forbidden** (`br_sched_block_current` returns `-EPERM` directly),
 *       yielding is forbidden (`br_task_yield` refuses and leaves a trace), and ending the thread
 *       panics — there is no scheduling context to fall back to;
 *     - inside bh, taking long-held locks is **forbidden** (it runs "interrupts off"), so work items
 *       must be short (§9.4's bh contract);
 *     - blocking/config native APIs are all `-EPERM`/`-EINVAL` inside bh (runtime guards).
 *   This is a **tightening** (stricter than the design's "thread-context bh contract"): in exchange
 *   we get "no worker thread introduced, no scheduler dependency, bounded delay", and the SLOW-domain
 *   demux still meets the genuine hard requirement that status registers may only be read outside
 *   ISR context.
 */
#ifndef BR_CORE_BR_WORK_H
#define BR_CORE_BR_WORK_H

#include <br/core/br_types.h>

/*
 * Queue depth (compile-time static upper bound; design `3-01` §5: "queue depth = static manifest
 * declaration (resource budget)"). This prototype takes 16 at a 4 KiB page scale — 16 × (fn,arg) =
 * 256 B, the same order as the other static pools.
 * Full ⇒ `br_work_submit` returns `-EAGAIN` (**never silently dropped**: the caller must handle it,
 * see IR-10).
 */
#define BR_WORKQ_DEPTH 16u

/*
 * Execution cap per IRQ exit (number of work items).
 * Why a cap is mandatory: the drain happens at the **interrupts-off** IRQ exit ⇒ no cap would mean
 * "one interrupt can stretch interrupt latency arbitrarily long". Using 4 makes the worst-case IRQ
 * exit increment 4 work items; the rest is left to the **next interrupt exit** (prototype tick =
 * `1/HZ`, default 5 ms; `product.toml [kernel].hz`, ADR-0017 ⇒ the upper bound for "draining the
 * queue" is `ceil(pending / 4) × tick`).
 */
#define BR_WORK_BH_BUDGET 4u

/* Work-item function shape (design §5: v1 has no work object/cancel — bare fn + arg submit). */
typedef void (*br_work_fn_t)(void *arg);

/*
 * Submit one work item: `fn(arg)` will run exactly once in **bh context**.
 *
 * **ISR-safe** (CA-3's first whitelist entry: one L2 critical section + one slot write).
 * Returns: 0 = enqueued; `-EINVAL` = fn is NULL; `-EAGAIN` = queue full (**the only failure the
 * caller must handle**).
 *
 * ⚠ Caller duty (IR-10): if this submit is "the sole carrier of some interrupt event" (e.g. the
 *   SLOW-domain demux), `-EAGAIN` must **roll back** the actions already taken (clear busy / release
 *   the parent line), otherwise that event is silently lost forever — the only deadlock entry of
 *   this mechanism. Both call sites (`irq_core.c` / `irq_domain.c`) do so.
 *
 * ⚠ Execution time: the next interrupt exit (not "immediately"). Submitted from thread context ⇒
 *   upper bound = one tick. Self-tests/the host may call `br_work_drain()` explicitly to run it at
 *   once (see `core/selftest/work_selftest.c`).
 */
int br_work_submit(br_work_fn_t fn, void *arg);

/*
 * Take and run at most `max_items` items, returning how many actually ran.
 *
 * Called by core's IRQ exit (`irq_core.c`, passing `BR_WORK_BH_BUDGET`); also the explicit drive
 * point for the host/self-tests. **Not reentrant**: if already in bh (some work item calls it
 * again) ⇒ return 0 (the outer loop continues), so that "submit again from inside a work item"
 * cannot eat the stack.
 */
br_u32 br_work_drain(br_u32 max_items);

/* Whether we are currently in bh context (`BR_TRUE` inside a work item). For guards and cases. */
br_bool br_work_in_bh(void);

/* Observation snapshot (not a hot path; same shape as br_irq_stat_t: diagnostics only, no golden layout promise). */
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

/*
 * The work queue's "suite entry" is **no longer** declared here: same as `br_sched.h`/`br_sync.h`
 * (ADR-0010) — a self-test entry is not a plugin's public capability, so the declaration is
 * centralised in `core/selftest/core_selftest.c`.
 */

#endif /* BR_CORE_BR_WORK_H */
