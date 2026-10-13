/*
 * Host-side semantics gate for **sched/rr** (time-slice round-robin preemption). The host has no
 * interrupt: host_tick_from_current() calls br_sched_on_tick() + br_sched_irq_epilogue() from the
 * running thread, equivalent to "this tick interrupted this thread" (the saved context really is
 * the caller's own). Cases: HOST-RR-SLICE (AABB alternation), HOST-RR-NOTICK (no tick, no
 * preempt), HOST-RR-RESCHED, HOST-RR-CONF.
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

#include <br/sched/rr.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_sched.h"

int br_sched_selftest(void);

/* ---- case framework ---- */

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
#define HOST_STACK_SLOTS  8u
/* Case's own virtual tick, decoupled from product [kernel].hz (criteria do not depend on it). */
#define TICK_US           100000u

static br_u8 s_stk[HOST_STACK_SLOTS][HOST_STACK_BYTES] BR_ALIGN(16);

static br_task_attr_t host_attr(br_u32 slot, const char *name)
{
    br_task_attr_t a;

    a.name       = name;
    a.stack      = (void *)(s_stk[slot % HOST_STACK_SLOTS] + HOST_STACK_BYTES);
    a.stack_size = HOST_STACK_BYTES;
    a.prio       = 0u;
    a.flags      = 0u;
    return a;
}

/* "one tick that interrupts the current thread" (see file header). */
static void host_tick_from_current(void)
{
    host_sched_advance_us(TICK_US);
    host_sched_tick();
    br_sched_irq_epilogue();
}

/* ---- 1) time-slice round-robin: busy threads preempted by tick => AABB AABB ---- */

#define RR_ITERS 4

static volatile char s_trace[32];
static volatile int  s_trace_n;
static volatile br_u32 s_runs_a;
static volatile br_u32 s_runs_b;

static void trace_put(char c)
{
    if (s_trace_n < (int)sizeof(s_trace)) {
        s_trace[s_trace_n] = c;
        s_trace_n++;
    }
}

/* Busy thread: never yields; takes ticks in its own loop (the target shape). */
static void host_busy_ticks(void *arg)
{
    const char c = (char)(br_intptr_t)arg;

    for (int i = 0; i < RR_ITERS; i++) {
        trace_put(c);
        if (c == 'A') {
            s_runs_a++;
        } else {
            s_runs_b++;
        }
        host_tick_from_current();
    }
}

static void host_case_slice(void)
{
    br_task_attr_t aa = host_attr(0u, "rkA");
    br_task_attr_t ab = host_attr(1u, "rkB");
    br_thread_t *ta = BR_NULL;
    br_thread_t *tb = BR_NULL;

    s_trace_n = 0;
    s_runs_a = 0u;
    s_runs_b = 0u;
    for (br_u32 i = 0u; i < (br_u32)sizeof(s_trace); i++) {
        s_trace[i] = '.';
    }

    const int r1 = br_task_create(&ta, &aa, host_busy_ticks, (void *)(br_intptr_t)'A');
    const int r2 = br_task_create(&tb, &ab, host_busy_ticks, (void *)(br_intptr_t)'B');

    /* Yield so A/B can run (they self-generate ticks); when we wake, both are done. */
    while ((ta != BR_NULL && br_task_state(ta) != BR_TASK_ZOMBIE) ||
           (tb != BR_NULL && br_task_state(tb) != BR_TASK_ZOMBIE)) {
        br_task_yield();
    }

    const int j1 = (ta != BR_NULL) ? br_task_join(ta, BR_NULL) : BR_ERR(EINVAL);
    const int j2 = (tb != BR_NULL) ? br_task_join(tb, BR_NULL) : BR_ERR(EINVAL);

    const char *p = (const char *)s_trace;
    printf("[HOSTTEST] TRACE rr slice seq=%.8s (n=%d, slice=%u ticks)\n",
           p, (int)s_trace_n, (unsigned)RR_SLICE_TICKS);

    /* Criterion: strictly AABB AABB -- each run no longer than the slice. Anything else means the
     * slice did not take effect or preemption happened in the wrong place. */
    const br_bool seq_ok = (s_trace_n == 2 * RR_ITERS) &&
                           (p[0] == 'A') && (p[1] == 'A') &&
                           (p[2] == 'B') && (p[3] == 'B') &&
                           (p[4] == 'A') && (p[5] == 'A') &&
                           (p[6] == 'B') && (p[7] == 'B');

    host_check((r1 == 0) && (r2 == 0) && (j1 == 0) && (j2 == 0) && seq_ok,
               "HOST-RR-SLICE",
               "busy threads that do not yield are preempted by the slice: alternation == AABBAABB (each run exactly 2 ticks)");
    host_check((s_runs_a == RR_ITERS) && (s_runs_b == RR_ITERS),
               "HOST-RR-SLICE",
               "both busy threads complete all their iterations (preemption swallowed no execution)");
}

/* ---- 2) no tick => never preempt (paired with 1: the only source of preemption is a tick) ---- */

static volatile int s_notick_victim;

static void host_victim(void *arg)
{
    (void)arg;
    s_notick_victim++;
}

static void host_busy_notick(void *arg)
{
    (void)arg;

    /* A whole busy loop that emits no tick => the opponent must not be switched in. */
    for (volatile br_u32 i = 0u; i < 200000u; i++) {
        if (s_notick_victim != 0) {
            s_notick_victim = 99;      /* marker: truly preempted */
        }
    }
    br_task_yield();                   /* explicit yield: only now may the opponent run */
}

static void host_case_no_tick(void)
{
    br_task_attr_t ab = host_attr(2u, "ntBusy");
    br_task_attr_t av = host_attr(3u, "ntVictim");
    br_thread_t *tb = BR_NULL;
    br_thread_t *tv = BR_NULL;

    s_notick_victim = 0;

    const int r1 = br_task_create(&tb, &ab, host_busy_notick, BR_NULL);
    const int r2 = br_task_create(&tv, &av, host_victim, BR_NULL);
    const int j1 = (tb != BR_NULL) ? br_task_join(tb, BR_NULL) : BR_ERR(EINVAL);
    const int j2 = (tv != BR_NULL) ? br_task_join(tv, BR_NULL) : BR_ERR(EINVAL);

    host_check((r1 == 0) && (r2 == 0) && (j1 == 0) && (j2 == 0) &&
               (s_notick_victim == 1),
               "HOST-RR-NOTICK",
               "no tick ⇒ the busy thread is not preempted (the only source of preemption is a tick; the opponent runs only after an explicit yield)");
}

/* ---- 3) setting and consuming need_resched ---- */

static void host_case_resched(void)
{
    const br_bool before = br_sched_resched_pending();

    br_sched_request_resched();
    const br_bool set = br_sched_resched_pending();

    /* Right now only main (not on the queue) + supervisor exist => pick_next only picks self: the
     * exit consumes the request but does not switch stacks (switching to self is pointless). */
    br_sched_irq_epilogue();
    const br_bool consumed = br_sched_resched_pending();
    const br_bool still_self = (br_task_name(br_task_self()) != NULL) &&
                               (strcmp(br_task_name(br_task_self()), "supervisor") == 0);

    host_check((before == BR_FALSE) && (set == BR_TRUE) && (consumed == BR_FALSE) && still_self,
               "HOST-RR-RESCHED",
               "need_resched: set is visible → exit consumes it → no stack switch when nobody else is runnable");
}

/* ---- supervisor thread ---- */

static void host_supervisor(void *arg)
{
    (void)arg;

    br_log_info("[HOSTTEST] supervisor start: self=%s", br_task_name(br_task_self()));

    host_case_slice();
    host_case_no_tick();
    host_case_resched();

    /* core's suite with kind=preempt runs TC-TASK-102/103 and skips TC-TASK-101's coop criterion.
     * Turn on the automatic tick source: those busy loops never yield and can only be interrupted
     * by their own clock reads (see host_sched.h). */
    host_sched_auto_tick(BR_TRUE);
    br_sched_selftest();
    host_sched_auto_tick(BR_FALSE);

    host_check(host_sched_idle_calls() > 0u, "HOST-RR-IDLE",
               "pick_next returns NULL ⇒ core goes idle (host hook does WFI for it) and is woken by a tick");
    host_check(br_sched_task_count() == 2u, "HOST-RR-POOL",
               "all test threads reclaimed by join (only main + supervisor remain)");
    host_check(host_sched_watch_hits() == 0u, "HOST-RR-CONF",
               "br_sched_selftest had no [TASKCONF] FAIL");

    printf("[HOSTTEST] SUMMARY pass=%d fail=%d total=%d (idle_calls=%u)\n",
           s_hpass, s_hfail, s_hpass + s_hfail, (unsigned)host_sched_idle_calls());

    exit((s_hfail == 0) ? 0 : 1);
}

int main(void)
{
    printf("[HOSTTEST] ==== sched/rr: time-slice round-robin preemption (kind=preempt) ====\n");

    host_sched_watch("[TASKCONF] FAIL");

    const int r = rr_early_init();
    if (r != 0) {
        printf("[HOSTTEST] FAIL rr_early_init rc=%d\n", r);
        return 1;
    }
    host_sched_main(host_supervisor);
}
