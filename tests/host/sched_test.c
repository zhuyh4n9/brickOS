/*
 * Host-side semantic gate for the scheduling framework / **coop**. Shared fixtures live in
 * tests/host/host_sched.c; ucontext gives a real stack switch, so create/yield/block/join/sleep
 * take the real path. After the image switched to sched/rr this is coop's only leg -- TC-TASK-101
 * ("a busy thread that does not yield is not switched away") no longer runs on target.
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

#include <br/sched/coop.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_sched.h"

/* TCB common header (opening the ICB window for `wait_status` is the caller's obligation). */
#include "sched_internal.h"

/* Selftest suite entry (not declared in br_sched.h). */
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

/* ---- 1) ready-queue FIFO order: three threads rotate strictly ABCABCABC ---- */

static volatile char s_fifo_seq[32];
static volatile int  s_fifo_n;

static void host_worker_seq(void *arg)
{
    const char c = (char)(br_intptr_t)arg;

    for (int i = 0; i < 3; i++) {
        if (s_fifo_n < 32) {
            s_fifo_seq[s_fifo_n] = c;
            s_fifo_n++;
        }
        br_task_yield();
    }
}

static void host_case_fifo(void)
{
    br_task_attr_t a1 = host_attr(0u, "hA");
    br_task_attr_t a2 = host_attr(1u, "hB");
    br_task_attr_t a3 = host_attr(2u, "hC");
    br_thread_t *ta = BR_NULL;
    br_thread_t *tb = BR_NULL;
    br_thread_t *tc = BR_NULL;
    int j1, j2, j3;

    s_fifo_n = 0;
    for (br_u32 i = 0u; i < 32u; i++) {
        s_fifo_seq[i] = '.';
    }

    const int r1 = br_task_create(&ta, &a1, host_worker_seq, (void *)(br_intptr_t)'A');
    const int r2 = br_task_create(&tb, &a2, host_worker_seq, (void *)(br_intptr_t)'B');
    const int r3 = br_task_create(&tc, &a3, host_worker_seq, (void *)(br_intptr_t)'C');

    j1 = (ta != BR_NULL) ? br_task_join(ta, BR_NULL) : BR_ERR(EINVAL);
    j2 = (tb != BR_NULL) ? br_task_join(tb, BR_NULL) : BR_ERR(EINVAL);
    j3 = (tc != BR_NULL) ? br_task_join(tc, BR_NULL) : BR_ERR(EINVAL);

    const char *p = (const char *)s_fifo_seq;
    printf("[HOSTTEST] TRACE fifo seq=%.9s (n=%d)\n", p, (int)s_fifo_n);

    const br_bool ok = (r1 == 0) && (r2 == 0) && (r3 == 0) &&
                       (j1 == 0) && (j2 == 0) && (j3 == 0) && (s_fifo_n == 9) &&
                       (p[0] == 'A') && (p[1] == 'B') && (p[2] == 'C') &&
                       (p[3] == 'A') && (p[4] == 'B') && (p[5] == 'C') &&
                       (p[6] == 'A') && (p[7] == 'B') && (p[8] == 'C');

    host_check(ok, "HOST-SCHED-FIFO", "coop ready queue FIFO: three threads rotate strictly ABCABCABC");
}

/* ---- 2) block/wake state machine (drives br_sched_block_current / br_sched_wake directly) ---- */

static volatile int s_bw_rc = -12345;

static void host_waiter_inf(void *arg)
{
    (void)arg;
    br_thread_t *me = br_task_self();

    me->wait_status = BR_ERR(ETIMEDOUT);   /* ICB handshake: pre-set "not woken" before opening the window */
    s_bw_rc = br_sched_block_current(BR_TIMEOUT_INF);
}

static void host_case_blockwake(void)
{
    br_task_attr_t a = host_attr(3u, "hwait");
    br_thread_t *t = BR_NULL;
    br_bool blocked, ready, zombie;

    s_bw_rc = -12345;
    const int r = br_task_create(&t, &a, host_waiter_inf, BR_NULL);

    br_task_yield();                     /* let the waiter run and block */
    blocked = (t != BR_NULL) && (br_task_state(t) == BR_TASK_BLOCKED);

    br_sched_wake(t);                    /* BLOCKED → READY (only changes state + links, does not touch the stack) */
    ready = (t != BR_NULL) && (br_task_state(t) == BR_TASK_READY);

    br_task_yield();                     /* let it resume: block_current returns wait_status (0) */
    zombie = (t != BR_NULL) && (br_task_state(t) == BR_TASK_ZOMBIE);

    const int j = (t != BR_NULL) ? br_task_join(t, BR_NULL) : BR_ERR(EINVAL);

    host_check((r == 0) && blocked && ready && zombie && (j == 0) && (s_bw_rc == 0),
               "HOST-SCHED-BLOCKWAKE",
               "block_current/wake: RUNNING→BLOCKED→(wake)READY→RUNNING→ZOMBIE");
}

/* ---- 3) timeout-table expiry wake (finite absolute deadline) ---- */

static volatile int s_to_rc = -12345;

static void host_waiter_to(void *arg)
{
    (void)arg;
    br_thread_t *me = br_task_self();

    me->wait_status = BR_ERR(ETIMEDOUT);
    s_to_rc = br_sched_block_current(host_sched_now() + 5000u);
}

static void host_case_timeout(void)
{
    br_task_attr_t a = host_attr(4u, "hto");
    br_thread_t *t = BR_NULL;

    s_to_rc = -12345;
    const int r = br_task_create(&t, &a, host_waiter_to, BR_NULL);
    const int j = (t != BR_NULL) ? br_task_join(t, BR_NULL) : BR_ERR(EINVAL);

    host_check((r == 0) && (j == 0) && (s_to_rc == BR_ERR(ETIMEDOUT)),
               "HOST-SCHED-TIMEOUT",
               "finite deadline expires ⇒ -ETIMEDOUT (wake_at table wakes it during the tick scan)");
}

/* ---- 4) sleep_until registration ---- */

static volatile br_time_t s_su_delta;
static volatile int      s_su_rc = -12345;

static void host_waiter_su(void *arg)
{
    (void)arg;
    const br_time_t t0 = br_clock_now();

    s_su_rc = br_task_sleep_until(t0 + 3000u);
    s_su_delta = br_clock_now() - t0;
}

static void host_case_sleep_until(void)
{
    br_task_attr_t a = host_attr(5u, "hsu");
    br_thread_t *t = BR_NULL;

    s_su_rc = -12345;
    s_su_delta = 0u;
    const int r = br_task_create(&t, &a, host_waiter_su, BR_NULL);
    const int j = (t != BR_NULL) ? br_task_join(t, BR_NULL) : BR_ERR(EINVAL);

    host_check((r == 0) && (j == 0) && (s_su_rc == 0) && (s_su_delta >= 3000u),
               "HOST-SCHED-SLEEPUNTIL",
               "br_task_sleep_until registers an absolute deadline, does not wake early");
}

/* ---- supervisor thread: runs all cases then exits with the failure count ---- */

static void host_supervisor(void *arg)
{
    (void)arg;

    br_log_info("[HOSTTEST] supervisor thread start: self=%s state=%u",
                br_task_name(br_task_self()), (unsigned)br_task_state(br_task_self()));

    host_case_fifo();
    host_case_blockwake();
    host_case_timeout();
    host_case_sleep_until();

    /* core's selftest suite: runs in real threads (create/join/yield/sleep) and returns the number
     * of failing items; red/green is aggregated by the br_log hooks above. */
    br_sched_selftest();

    host_check(host_sched_idle_calls() > 0u, "HOST-SCHED-IDLE",
               "pick_next returns NULL ⇒ core enters idle (the host emulates WFI via a hook) and the tick wakes it");
    host_check(br_sched_task_count() == 2u, "HOST-SCHED-POOL",
               "all test threads are reclaimed by join (only main + supervisor remain)");
    host_check(host_sched_watch_hits() == 0u, "HOST-SCHED-CONF",
               "br_sched_selftest has no [TASKCONF] FAIL");
    host_check((br_task_name(br_task_self()) != NULL) &&
               (strcmp(br_task_name(br_task_self()), "supervisor") == 0),
               "HOST-SCHED-NAME", "br_task_name/br_task_self are correct in a real thread");

    printf("[HOSTTEST] SUMMARY pass=%d fail=%d total=%d (idle_calls=%u)\n",
           s_hpass, s_hfail, s_hpass + s_hfail, (unsigned)host_sched_idle_calls());

    exit((s_hfail == 0) ? 0 : 1);
}

int main(void)
{
    printf("[HOSTTEST] ==== coop: cooperative scheduler (no preemption) ====\n");

    /* [TASKCONF] FAIL inside the suite must also affect the exit code. */
    host_sched_watch("[TASKCONF] FAIL");

    const int r = coop_early_init();
    if (r != 0) {
        printf("[HOSTTEST] FAIL coop_early_init rc=%d\n", r);
        return 1;
    }
    host_sched_main(host_supervisor);
}
