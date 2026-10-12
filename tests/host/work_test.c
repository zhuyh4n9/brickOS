/*
 * tests/host/work_test.c — host-side semantic gate for **deferred work (bottom half / workqueue)**
 *
 * Why run on the host: queue boundedness / FIFO / non-reentrancy / context flags / statistics are
 * **pure semantics** that the host can push to the limit in milliseconds (constructing a "queue
 * full" on the target would need an interrupt storm). The target side has another leg (QEMU's
 * `TC-IRQ-015/016/017`: per-line BH dispatch, suppression, no replay); together they are complete.
 *
 * ★ The three unshakeable properties verified (`br_work.h` W1/W2/W3):
 *   W1 bounded: the queue is a compile-time static ring; full ⇒ `-EAGAIN` (never silently dropped — IR-10)
 *   W2 ISR-safe: the submit path does not call back and takes no lock
 *   W3 non-reentrant: calling `br_work_drain` again inside a work item returns 0, stack depth does not grow with queue length
 *
 * Output protocol: [HOSTTEST] PASS|FAIL … SUMMARY; any failure ⇒ non-zero exit code.
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_trace.h>
#include <br/core/br_types.h>
#include <br/core/br_work.h>

#include <br/sched/rr.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_sched.h"

/* TCB common header: when calling `br_sched_block_current` inside bh you must open the window per the ICB contract (see br_sched.h). */
#include "sched_internal.h"

/* =====================================================================
 * case framework
 * ===================================================================== */

static int s_hpass;
static int s_hfail;

static void host_check(br_bool ok, const char *tag, const char *what)
{
    if (ok != BR_FALSE) {
        s_hpass++;
        printf("[HOSTTEST] PASS %s %s\n", tag, what);
    } else {
        s_hfail++;
        printf("[HOSTTEST] FAIL %s %s\n", tag, what);
    }
}

#define HOST_STACK_BYTES  8192u
static br_u8 s_stk[4][HOST_STACK_BYTES] BR_ALIGN(16);

/* =====================================================================
 * work item bodies
 * ===================================================================== */

static volatile br_u32 s_order[BR_WORKQ_DEPTH + 4u];
static volatile br_u32 s_order_n;
static volatile br_u32 s_ran;

static void note(br_u32 v)
{
    if (s_order_n < (br_u32)(sizeof(s_order) / sizeof(s_order[0]))) {
        s_order[s_order_n] = v;
        s_order_n++;
    }
    s_ran++;
}

static void item_note(void *arg)
{
    note((br_u32)(br_uintptr_t)arg);
}

/* ---- W3: non-reentrancy + bh context flag + block/yield bans ---- */

static volatile int s_nested_rc = -1;
static volatile int s_bh_flag_inside;
static volatile int s_bh_flag_after_yield;
static volatile int s_block_rc = 999;
static volatile br_thread_t *s_self_before;
static volatile br_thread_t *s_self_after;
static volatile br_u32 s_trace_violation;

static void item_context(void *arg)
{
    (void)arg;

    s_bh_flag_inside = (br_work_in_bh() != BR_FALSE) ? 1 : 0;
    s_nested_rc = (int)br_work_drain(4u);        /* W3: non-reentrant ⇒ 0 */

    /* blocking inside bh ⇒ -EPERM (the arguments are fine, the context is wrong). */
    br_thread_t *me = br_task_self();
    me->wait_status = BR_ERR(ETIMEDOUT);      /* open the window per the ICB contract */
    s_block_rc = br_sched_block_current(BR_TIMEOUT_INF);

    /* yield inside bh ⇒ refused + trace record (not silently treated as "yield succeeded"). */
    s_self_before = br_task_self();
    br_task_yield();
    s_self_after = br_task_self();
    s_bh_flag_after_yield = (br_work_in_bh() != BR_FALSE) ? 1 : 0;

    note(0xE0u);
}

/* Drain the trace ring and count BH_CTX_VIOLATION (runtime trace of the bh ban). */
static br_u32 drain_violation_count(void)
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

/* ---- cases ---- */

static void case_bound_and_order(void)
{
    br_work_stat_t st;
    br_u32 ran;
    int rc;
    br_u32 first_full = 0u;

    br_work_stats_get(&st);
    const br_u32 base_drop = st.dropped_full;
    const br_u32 base_ran  = st.ran;

    s_order_n = 0u;
    s_ran = 0u;

    /* W1: fill the queue — the DEPTH+1-th submit must be -EAGAIN */
    rc = 0;
    for (br_u32 i = 0u; i < (br_u32)BR_WORKQ_DEPTH; i++) {
        const int r = br_work_submit(item_note, (void *)(br_uintptr_t)(100u + i));
        if (r != 0) {
            rc = r;
            break;
        }
    }
    first_full = (br_u32)br_work_submit(item_note, (void *)(br_uintptr_t)999u);
    br_work_stats_get(&st);

    host_check((rc == 0) && (first_full == (br_u32)BR_ERR(EAGAIN)) &&
               (st.pending == (br_u32)BR_WORKQ_DEPTH) &&
               (st.max_pending == (br_u32)BR_WORKQ_DEPTH),
               "HOST-WQ-BOUND",
               "queue bounded: filling DEPTH entries succeeds, the DEPTH+1-th ⇒ -EAGAIN (no silent drop)");

    /* FIFO: drain order must match submit order item by item (budget of 1 each time ⇒ also checks the budget is honored) */
    ran = 0u;
    for (br_u32 i = 0u; i < (br_u32)BR_WORKQ_DEPTH; i++) {
        const br_u32 k = br_work_drain(1u);
        if (k == 0u) {
            break;
        }
        ran += k;
    }
    br_bool fifo = (ran == (br_u32)BR_WORKQ_DEPTH) &&
                   (s_order_n == (br_u32)BR_WORKQ_DEPTH);
    for (br_u32 i = 0u; fifo && i < (br_u32)BR_WORKQ_DEPTH; i++) {
        if (s_order[i] != (100u + i)) {
            fifo = BR_FALSE;
        }
    }

    br_work_stats_get(&st);
    host_check(fifo && (st.pending == 0u) && (st.dropped_full > base_drop) &&
               (st.ran == base_ran + (br_u32)BR_WORKQ_DEPTH),
               "HOST-WQ-FIFO",
               "drain executes FIFO item by item (with budget=1 one per round), pending==0 once drained");

    /* bad arguments and full capacity are two kinds of failure (counted separately, so a bug is not read as "budget too small") */
    const int bad = br_work_submit(BR_NULL, BR_NULL);
    br_work_stats_get(&st);
    host_check((bad == BR_ERR(EINVAL)) && (st.dropped_invalid == 1u),
               "HOST-WQ-EINVAL", "fn == NULL ⇒ -EINVAL (counted separately, not confused with queue full)");
}

static void case_bh_context(void)
{
    br_work_stat_t st0, st1;

    s_nested_rc = -1;
    s_bh_flag_inside = -1;
    s_block_rc = 999;
    s_self_before = BR_NULL;
    s_self_after = BR_NULL;
    s_bh_flag_after_yield = -1;
    s_order_n = 0u;

    br_work_stats_get(&st0);
    const br_u32 tr0 = drain_violation_count();

    const int r = br_work_submit(item_context, BR_NULL);
    const br_u32 ran = br_work_drain(4u);

    br_work_stats_get(&st1);
    const br_u32 tr1 = drain_violation_count();
    s_trace_violation = tr1 - tr0;

    host_check((r == 0) && (ran == 1u) && (s_bh_flag_inside == 1),
               "HOST-WQ-BHCTX", "br_work_in_bh() is true inside the work item body (bh context flag)");
    host_check(s_nested_rc == 0, "HOST-WQ-NORECURSE",
               "br_work_drain called again inside a work item ⇒ returns 0 directly (non-reentrant, stack does not grow with queue length)");
    host_check(s_block_rc == BR_ERR(EPERM), "HOST-WQ-NOBLOCK",
               "br_sched_block_current inside bh ⇒ -EPERM (no schedulable context to fall back to)");
    host_check((s_self_before == s_self_after) && (s_bh_flag_after_yield == 1) &&
               (s_trace_violation >= 1u),
               "HOST-WQ-NOYIELD",
               "br_task_yield inside bh is refused (same thread, still in bh) + BH_CTX_VIOLATION trace");
    host_check(st1.drain_passes > st0.drain_passes, "HOST-WQ-STATS",
               "drain_passes counter increments (stats usable for diagnostics)");
}

static void case_budget(void)
{
    s_order_n = 0u;

    for (br_u32 i = 0u; i < 6u; i++) {
        (void)br_work_submit(item_note, (void *)(br_uintptr_t)(700u + i));
    }

    const br_u32 a = br_work_drain(2u);
    br_work_stat_t st;
    br_work_stats_get(&st);
    const br_u32 pending_after = st.pending;

    const br_u32 b = br_work_drain(8u);
    br_work_stats_get(&st);

    host_check((a == 2u) && (pending_after == 4u) && (b == 4u) && (st.pending == 0u),
               "HOST-WQ-BUDGET",
               "drain(max) strictly honors the budget: the 2+4 items run, the rest stay queued for the next round (prescription for the IRQ exit)");
}

/* =====================================================================
 * supervisor thread
 * ===================================================================== */

static volatile int s_other_hits;

static void other_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 3; i++) {
        s_other_hits++;
        br_task_yield();
    }
}

static void host_supervisor(void *arg)
{
    (void)arg;

    br_log_info("[HOSTTEST] supervisor start: self=%s", br_task_name(br_task_self()));

    case_bound_and_order();
    case_bh_context();
    case_budget();

    /* Yield once: if "yield inside bh" really switched away, the opponent would run here —
     * complementary to the HOST-WQ-NOYIELD check above (that one only looks at thread identity,
     * this one at whether someone else really ran). */
    s_other_hits = 0;
    br_task_attr_t a = { .name = "other",
                         .stack = (void *)(s_stk[0] + HOST_STACK_BYTES),
                         .stack_size = HOST_STACK_BYTES,
                         .prio = 0u, .flags = 0u };
    br_thread_t *t = BR_NULL;
    const int r = br_task_create(&t, &a, other_thread, BR_NULL);
    const int j = (t != BR_NULL) ? br_task_join(t, BR_NULL) : BR_ERR(EINVAL);

    host_check((r == 0) && (j == 0) && (s_other_hits > 0),
               "HOST-WQ-SCHED",
               "workqueue coexists with the scheduler: threads yield and are switched in as usual (bh does not break scheduling)");

    printf("[HOSTTEST] SUMMARY pass=%d fail=%d total=%d\n",
           s_hpass, s_hfail, s_hpass + s_hfail);

    exit((s_hfail == 0) ? 0 : 1);
}

int main(void)
{
    printf("[HOSTTEST] ==== bottom half (workqueue / bh) semantics ====\n");

    host_sched_watch("[TASKCONF] FAIL");

    const int r = rr_early_init();
    if (r != 0) {
        printf("[HOSTTEST] FAIL rr_early_init rc=%d\n", r);
        return 1;
    }
    host_sched_main(host_supervisor);
}
