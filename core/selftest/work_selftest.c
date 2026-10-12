/*
 * brickOS prototype v0.2.0 — **self-test suite** for deferred work (bottom half / workqueue) (core/selftest/)
 *
 * Design basis and per-item rulings: `docs/decisions/0011-round-robin-bh-workqueue.md`;
 * design sources: `3-01-core-api-list.md` §5 (deferred work = bottom half),
 *           `3-02-int.md` §11.1 (path one: ISR → deferred work)/§11.4.1 (semantics of disable under BH form)/
 *           §12.4 (execution timing and priority)/IR-10 (a failed submit must roll back).
 *
 * ## Why these cases live in core's self-test directory
 *   Same origin as `sched_selftest.c` (ADR-0010): cases are not a plugin's exported capability, so they
 *   should not enter `br_work.h`'s golden surface; and "disable self-test ⇒ the whole directory is not
 *   compiled" applies to the core side too.
 *
 * ## Case list (one-to-one with the host leg of `tests/host/work_test.c`)
 *   TC-WQ-001  bounded queue: filling DEPTH entries succeeds, entry DEPTH+1 ⇒ -EAGAIN (no silent drop)
 *   TC-WQ-002  FIFO: drain order == submit order; drain(max) strictly respects the budget
 *   TC-WQ-003  non-reentrant: calling drain again inside a work item ⇒ 0 (stack does not grow with queue length)
 *   TC-WQ-004  bh context: inside the body `br_work_in_bh()` is true and `br_irq_in_atomic()` is true,
 *              while `br_irq_in_isr()` is false (that is the definition of "bottom half")
 *   TC-WQ-005  bh prohibitions: blocking ⇒ -EPERM; yielding ⇒ rejected + BH_CTX_VIOLATION logged
 *   TC-WQ-006  statistics self-consistent (submitted/ran/pending/max_pending match the actions)
 *   TC-WQ-007  actually consumed at the **IRQ exit**: on target driven by `br_irq_enter`'s exit drain,
 *              here expressed as "submit + explicit drain", the decidable part of the same chain
 *
 * ## Return value
 *   `int` = **number of failures** (ADR-0010 §2.4). Any failure **does not halt**.
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_trace.h>
#include <br/core/br_types.h>
#include <br/core/br_work.h>

/* The TCB common header (ready/blocked/state fields) and "read-only registry observation" are core-private
 * surfaces (same origin as sched_selftest.c: cross-directory relative includes have precedent in this repo). */
#include "../src/sched/sched_internal.h"

/* Visible declarations of the entry are centralized in `core/selftest/core_selftest.c` (the sole aggregator);
 * declaring it again here satisfies the "every function has a prototype before use" discipline (-Wmissing-prototypes). */
int br_work_selftest(void);

static int s_wq_pass;
static int s_wq_fail;

static void wq_result(br_bool ok, const char *id, const char *what)
{
    if (ok != BR_FALSE) {
        s_wq_pass++;
        br_log_info("[WQCONF] PASS %s %s", id, what);
    } else {
        s_wq_fail++;
        br_log_info("[WQCONF] FAIL %s %s", id, what);
    }
}

/* ------------------------------------------------------------------ work item bodies */

static volatile br_u32 s_wq_order[BR_WORKQ_DEPTH + 4u];
static volatile br_u32 s_wq_order_n;

static void wq_item_note(void *arg)
{
    const br_u32 v = (br_u32)(br_uintptr_t)arg;

    if (s_wq_order_n < (br_u32)(sizeof(s_wq_order) / sizeof(s_wq_order[0]))) {
        s_wq_order[s_wq_order_n] = v;
        s_wq_order_n++;
    }
}

static volatile int s_wq_nested = -1;
static volatile int s_wq_bh_flag = -1;
static volatile int s_wq_atomic_flag = -1;
static volatile int s_wq_isr_flag = -1;
static volatile int s_wq_block_rc = 999;
static volatile br_thread_t *s_wq_self_before;
static volatile br_thread_t *s_wq_self_after;
static volatile int s_wq_bh_after_yield = -1;
static volatile br_u32 s_wq_violation_before;
static volatile br_u32 s_wq_violation_after;

static br_u32 wq_trace_violations(void)
{
    br_trace_evt_t ev[32];
    br_u32 n;
    br_u32 hits = 0u;

    while ((n = br_trace_drain(ev, 32u)) > 0u) {
        for (br_u32 i = 0u; i < n; i++) {
            if (ev[i].id == BR_TRACE_BH_CTX_VIOLATION) {
                hits++;
            }
        }
    }
    return hits;
}

static void wq_item_context(void *arg)
{
    (void)arg;

    s_wq_bh_flag     = (br_work_in_bh() != BR_FALSE) ? 1 : 0;
    s_wq_atomic_flag = (br_irq_in_atomic() != BR_FALSE) ? 1 : 0;
    s_wq_isr_flag    = (br_irq_in_isr() != BR_FALSE) ? 1 : 0;

    s_wq_nested = (int)br_work_drain(4u);          /* TC-WQ-003 */

    br_thread_t *me = br_task_self();
    if (me != BR_NULL) {
        me->wait_status = BR_ERR(ETIMEDOUT);    /* ICB contract: set "not woken" before opening the window */
    }
    s_wq_block_rc = br_sched_block_current(BR_TIMEOUT_INF);   /* TC-WQ-005 */

    s_wq_self_before = br_task_self();
    br_task_yield();                               /* TC-WQ-005: should be rejected */
    s_wq_self_after = br_task_self();
    s_wq_bh_after_yield = (br_work_in_bh() != BR_FALSE) ? 1 : 0;

    wq_item_note((void *)(br_uintptr_t)0xE0u);
}

/* ------------------------------------------------------------------ cases */

static void case_wq_001_bound(void)
{
    br_work_stat_t st;
    int first_rc = 0;
    int full_rc;

    br_work_stats_get(&st);
    const br_u32 base_drop = st.dropped_full;

    /* The queue may not be empty (leftovers from earlier cases) ⇒ first fill it up completely. */
    while (br_work_submit(wq_item_note, (void *)(br_uintptr_t)1u) == 0) {
        /* fill until full */
    }
    full_rc = br_work_submit(wq_item_note, (void *)(br_uintptr_t)2u);
    br_work_stats_get(&st);

    const br_bool ok = (full_rc == BR_ERR(EAGAIN)) &&
                       (st.pending == (br_u32)BR_WORKQ_DEPTH) &&
                       (st.max_pending == (br_u32)BR_WORKQ_DEPTH) &&
                       (st.dropped_full > base_drop);
    (void)first_rc;

    wq_result(ok, "TC-WQ-001",
              "bounded queue: -EAGAIN after filling DEPTH, and dropped_full/max_pending grow honestly");

    /* Empty it (cleanup: later cases need a clean queue) */
    (void)br_work_drain((br_u32)BR_WORKQ_DEPTH);
}

static void case_wq_002_fifo_budget(void)
{
    br_work_stat_t st;

    s_wq_order_n = 0u;

    for (br_u32 i = 0u; i < 6u; i++) {
        const int r = br_work_submit(wq_item_note, (void *)(br_uintptr_t)(100u + i));
        if (r != 0) {
            wq_result(BR_FALSE, "TC-WQ-002", "submit failed (queue still occupied by earlier cases?)");
            return;
        }
    }

    const br_u32 a = br_work_drain(2u);          /* budget 2 */
    br_work_stats_get(&st);
    const br_u32 pending_mid = st.pending;
    const br_u32 b = br_work_drain(8u);
    br_work_stats_get(&st);

    br_bool fifo = (s_wq_order_n == 6u);
    for (br_u32 i = 0u; fifo && i < 6u; i++) {
        if (s_wq_order[i] != (100u + i)) {
            fifo = BR_FALSE;
        }
    }

    wq_result(fifo && (a == 2u) && (pending_mid == 4u) && (b == 4u) && (st.pending == 0u),
              "TC-WQ-002",
              "FIFO order + drain(max) strictly respects the budget (2 drained leaves 4, taken by the next round)");
}

static void case_wq_003_006_context(void)
{
    br_work_stat_t st0, st1;

    s_wq_nested = -1;
    s_wq_bh_flag = -1;
    s_wq_atomic_flag = -1;
    s_wq_isr_flag = -1;
    s_wq_block_rc = 999;
    s_wq_self_before = BR_NULL;
    s_wq_self_after = BR_NULL;
    s_wq_bh_after_yield = -1;

    br_work_stats_get(&st0);
    s_wq_violation_before = wq_trace_violations();

    const int r = br_work_submit(wq_item_context, BR_NULL);
    const br_u32 ran = br_work_drain(4u);

    s_wq_violation_after = wq_trace_violations();
    br_work_stats_get(&st1);

    wq_result((r == 0) && (ran == 1u) && (s_wq_bh_flag == 1) && (s_wq_atomic_flag == 1) &&
              (s_wq_isr_flag == 0),
              "TC-WQ-004",
              "bh context: in_bh=true / in_atomic=true / in_isr=false (that is the definition of bottom half)");

    wq_result(s_wq_nested == 0, "TC-WQ-003",
              "non-reentrant: calling drain again inside a work item ⇒ 0 (stack does not grow with queue length)");

    wq_result(s_wq_block_rc == BR_ERR(EPERM), "TC-WQ-005",
              "br_sched_block_current inside bh ⇒ -EPERM (no schedulable context to fall back to)");

    wq_result((s_wq_self_before == s_wq_self_after) && (s_wq_bh_after_yield == 1) &&
              (s_wq_violation_after > s_wq_violation_before),
              "TC-WQ-005",
              "br_task_yield inside bh is rejected (thread unchanged, still in bh) + BH_CTX_VIOLATION logged");

    wq_result((st1.submitted == st0.submitted + 1u) && (st1.ran == st0.ran + 1u) &&
              (st1.pending == 0u) && (st1.max_pending >= st0.max_pending),
              "TC-WQ-006",
              "statistics self-consistent: submitted/ran each +1, pending back to zero, max_pending monotonic");
}

static void case_wq_007_irq_exit(void)
{
    /*
     * On target the IRQ-exit consumption chain is performed by `br_irq_enter`'s exit calling
     * `br_work_drain(BR_WORK_BH_BUDGET)` (that path is covered by QEMU's TC-IRQ-015/017). Here we verify its
     * **decidable preconditions** on the core side: the budget parameter itself is positive, and one drain
     * consumes at most BUDGET entries — otherwise "the IRQ exit is bounded" is an empty statement.
     */
    s_wq_order_n = 0u;

    for (br_u32 i = 0u; i < (br_u32)BR_WORK_BH_BUDGET + 2u; i++) {
        (void)br_work_submit(wq_item_note, (void *)(br_uintptr_t)(200u + i));
    }
    const br_u32 ran = br_work_drain((br_u32)BR_WORK_BH_BUDGET);
    br_work_stat_t st;
    br_work_stats_get(&st);

    const br_bool ok = (BR_WORK_BH_BUDGET > 0u) && (ran == (br_u32)BR_WORK_BH_BUDGET) &&
                       (st.pending == 2u);

    /* cleanup */
    (void)br_work_drain(8u);

    wq_result(ok, "TC-WQ-007",
              "a single IRQ-exit run is bounded (BR_WORK_BH_BUDGET); leftovers wait for the next interrupt exit");
}

int br_work_selftest(void)
{
    s_wq_pass = 0;
    s_wq_fail = 0;

    br_log_info("[WQCONF] ==== bottom half / workqueue conformance (3-01 §5 / 3-02 §11.1/§11.4.1)");
    br_log_info("[WQCONF] ---- queue_depth=%u bh_budget=%u",
                (unsigned)BR_WORKQ_DEPTH, (unsigned)BR_WORK_BH_BUDGET);

    case_wq_001_bound();
    case_wq_002_fifo_budget();
    case_wq_003_006_context();
    case_wq_007_irq_exit();

    br_log_info("[WQCONF] SUMMARY pass=%d fail=%d total=%d",
                s_wq_pass, s_wq_fail, s_wq_pass + s_wq_fail);

    if (s_wq_fail != 0) {
        br_log_error("[WQCONF] failures present — bottom half / workqueue not conformance-clean");
    }

    return s_wq_fail;
}
