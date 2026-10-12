/*
 * brickOS prototype v0.2.0 — scheduler framework + coop self-test suite (core/selftest/)
 *
 * Design basis and rulings: `docs/decisions/0010-plugin-selftest.md`.
 * Case spec: `6-01-test.md` §3.1/§3.3 (`TC-TASK-*` / `TC-TIME-*`); implementation: `3-03-sched.md`.
 *
 * ## Why these lines moved here from `sched_core.c`
 *   They are tests, yet lived in the same .c as the production implementation — so
 *   "disable self-test" could not drop them, and readers of the production code had to
 *   dig through a thousand lines of mechanism for the checks. After the move:
 *     - `core/src/sched/sched_core.c` keeps mechanism only (state machine/timeouts/switch/thread API);
 *     - self-test off ⇒ `core/selftest/` is not compiled at all ⇒ case code does not
 *       reach the image (mechanical fact, not "compiled but not run"; see product.toml
 *       selftest_sources and build.rs);
 *     - case readers and implementation readers no longer interfere.
 *   This is a move, not a rewrite: checks/logs/order are unchanged (see the log contract below).
 *
 * ## The only cross-boundary observation
 *   The suite really does create/yield/join/sleep and must print which scheduler was
 *   registered (`scheduler=%s kind=%u spawn=%u`). Those fields exist only in the production
 *   registry — read via `br_sched_ops_get()` from `sched_internal.h`, not copied into a
 *   second plugin table here (that would give "what is registered" two sources of truth).
 *
 * ## Log contract (gates match literally ⇒ one changed character changes the check)
 *   `[TASKCONF] PASS/FAIL <id> <desc>` … `[TASKCONF] SUMMARY pass=%d fail=%d total=%d`
 *   require/forbid/require_tags in `tests/gates.toml` and the tag table in
 *   `sched/coop/tests/smoke.toml` are anchored to these two lines.
 *
 * ## Return value
 *   `int` = number of failed cases (ADR-0010 §2.4: the suite only aggregates, `br_core_selftest()`
 *   sums, gates decide red/green). No failure stops the system — it is just logged.
 *
 * ## ★ Branch by scheduler kind (ADR-0011 §5)
 *   This suite serves both `sched/coop` and `sched/rr` (preempt) plugins. Shared cases always
 *   run; the domain-specific ones branch on the registry `ops.kind` (not a compile-time switch,
 *   so "the kind the plugin reports" and "what the case expects" cannot disagree):
 *     - coop        ⇒ `TC-TASK-101`: a busy thread that never yields is NOT preempted (the coop definition)
 *     - preempt(rr) ⇒ `TC-TASK-102`: the same shape MUST be preempted by the time slice;
 *                      `TC-TASK-103`: no preemption may occur inside an L2 critical section (honesty clause).
 *   They are the two directions of one boundary; only together is it visible that they are opposites.
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>          /* TC-TASK-103: L2 critical section (IRQs off ⇒ no IRQ exit) */
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

/* TCB common header (BR_TASK_MAX / struct br_thread) and the read-only registry view are
 * core-private; cross-directory relative includes have precedent here
 * (core/src/sync/sync.c -> "../sched/sched_internal.h"). */
#include "../src/sched/sched_internal.h"

/* The entry's visible declaration lives in `core/selftest/core_selftest.c` (the only
 * aggregator); repeating it here satisfies this file's rule that every function has a
 * prototype before use (-Wmissing-prototypes). */
int br_sched_selftest(void);

/* =====================================================================
 * Conformance cases (TC-TASK-* / TC-TIME-*, 6-01 §3.1/§3.3)
 *
 * Shape: the suite itself runs in a real thread (APP thread / host supervisor thread) and
 * really does create/yield/join/sleep — so it validates switching/blocking/wakeup/timeout,
 * not just data structures. Each sub-case's thread stack is a static array (the prototype
 * has no dynamic allocation).
 * ===================================================================== */

#define BR_CONF_STACK_BYTES 4096u
#define BR_CONF_BUSY_ITERS  200000u

/*
 * Time basis for the preemption cases (TC-TASK-102/103): the criterion is "switched away
 * within a few time slices", and wall-clock length varies with the product's HZ
 * (`product.toml [kernel].hz`, default 200 ⇒ tick = 5 ms, rr slice = 2 tick = 10 ms;
 * ADR-0017). 600 ms gives ≈ 60 slices at the default and still ~30 slices at HZ=100 — what
 * is needed here is a window far larger than one slice, not an exact duration.
 */
#define BR_CONF_PREEMPT_WAIT_US 600000u
#define BR_CONF_CRIT_US         400000u

static int s_conf_pass;
static int s_conf_fail;

static br_u8 s_conf_stack[BR_TASK_MAX][BR_CONF_STACK_BYTES] BR_ALIGN(16);
static br_u8 s_busy_stack[BR_CONF_STACK_BYTES] BR_ALIGN(16);
static br_u8 s_victim_stack[BR_CONF_STACK_BYTES] BR_ALIGN(16);

static volatile br_uintptr_t s_probe_addr;
static volatile br_time_t    s_sleep_delta;
static volatile int          s_sleep_rc;
static volatile int          s_flag_a;
static volatile int          s_seq_n;
static volatile char         s_seq[32];
static volatile br_u32       s_victim_ticks;
static volatile int          s_preempt_seen;
static volatile int          s_victim_after_yield;
static volatile int          s_crit_violation;
static volatile br_u32       s_crit_ticks;

static void conf_result(br_bool ok, const char *id, const char *what)
{
    if (ok != BR_FALSE) {
        s_conf_pass++;
        br_log_info("[TASKCONF] PASS %s %s", id, what);
    } else {
        s_conf_fail++;
        br_log_info("[TASKCONF] FAIL %s %s", id, what);
    }
}

static br_task_attr_t conf_attr(br_u32 slot, const char *name)
{
    br_task_attr_t a;

    a.name       = name;
    a.stack      = (void *)(s_conf_stack[slot % BR_TASK_MAX] + BR_CONF_STACK_BYTES);
    a.stack_size = BR_CONF_STACK_BYTES;
    a.prio       = 0u;
    a.flags      = 0u;
    return a;
}

/* --- Thread bodies of each case ---------------------------------------  * WORKAROUND(br-wa-test-001): this suite's `TC-TASK-*`/`TC-TIME-*` ids are
 * not yet aligned one-by-one with the `6-01` §3.1/§3.3 table (meaning drift; the prose here
 * is accurate); see WORKAROUNDS.md for the alignment work.
 */

static void conf_entry_ok(void *arg)
{
    (void)arg;                          /* normal return ⇒ trampoline calls br_task_exit(0) */
}

static void conf_entry_code(void *arg)
{
    br_task_exit((int)(br_intptr_t)arg);
}

static void conf_entry_flag(void *arg)
{
    (void)arg;
    s_flag_a = 1;
}

static void conf_entry_ab(void *arg)
{
    const char c = (char)(br_intptr_t)arg;

    for (int i = 0; i < 4; i++) {
        if (s_seq_n < (int)BR_ARRAY_SIZE(s_seq)) {
            s_seq[s_seq_n] = c;
            s_seq_n++;
        }
        br_log_info("[TASKCONF] RUN worker %c i=%d", c, i);
        br_task_yield();                /* the only voluntary yield point in coop */
    }
}

static void conf_entry_probe(void *arg)
{
    (void)arg;
    volatile br_u8 local[128];

    local[0] = 0x5Au;
    if (local[0] != 0x5Au) {
        s_probe_addr = 0u;
        return;
    }
    s_probe_addr = (br_uintptr_t)(const void *)&local[0];
}

static void conf_entry_sleep(void *arg)
{
    (void)arg;
    const br_time_t t0 = br_clock_now();

    s_sleep_rc = br_task_sleep(2000u);          /* 2 ms */
    s_sleep_delta = br_clock_now() - t0;
}

static void conf_entry_victim(void *arg)
{
    (void)arg;
    s_victim_ticks++;
    return;
}

static void conf_entry_busy(void *arg)
{
    (void)arg;

    /* busy loop: never yields. Under coop the victim must NOT be switched in meanwhile. */
    for (br_u32 i = 0u; i < BR_CONF_BUSY_ITERS; i++) {
        if (s_victim_ticks != 0u) {
            s_preempt_seen = 1;
        }
    }

    br_task_yield();                    /* explicit yield: only now should the victim run */

    if (s_victim_ticks != 0u) {
        s_victim_after_yield = 1;
    }
}

/*
 * Preempting busy loop (TC-TASK-102): the length is bounded by the clock, not by iteration
 * count — the criterion is "was it switched away within a few slices", and iteration counts
 * drift with optimization level and platform. Stop as soon as the victim has run (avoid
 * always burning the full 600 ms).
 */
static void conf_entry_busy_preempt(void *arg)
{
    (void)arg;

    const br_time_t t0 = br_clock_now();

    while ((br_clock_now() - t0) < BR_CONF_PREEMPT_WAIT_US) {
        if (s_victim_ticks != 0u) {
            s_preempt_seen = 1;
            break;
        }
    }

    br_task_yield();

    if (s_victim_ticks != 0u) {
        s_victim_after_yield = 1;
    }
}

/*
 * Critical-section variant (TC-TASK-103): stay inside `br_irq_lock()` for far longer than
 * one time slice. Criterion: the victim must not run even once meanwhile (the I4 honesty
 * clause — preemption happens only at an IRQ exit, and with IRQs off there is no exit); once
 * released it must run soon (proving it was not lost, rather than never scheduled).
 */
static void conf_entry_crit(void *arg)
{
    (void)arg;

    const br_irq_state_t st = br_irq_lock();
    const br_time_t t0 = br_clock_now();

    while ((br_clock_now() - t0) < BR_CONF_CRIT_US) {
        if (s_victim_ticks != 0u) {
            s_crit_violation = 1;       /* preempted with IRQs off = I4 broken */
        }
        s_crit_ticks++;
    }
    br_irq_unlock(st);

    br_task_yield();                    /* after release, let the victim run */
    if (s_victim_ticks != 0u) {
        s_victim_after_yield = 1;
    }
}

/* --- Sub-cases -------------------------------------------------------- */

static void case_task_001(void)
{
    br_task_attr_t a = conf_attr(0u, "t001");
    br_thread_t *t = BR_NULL;

    const int r = br_task_create(&t, &a, conf_entry_ok, BR_NULL);
    const br_bool ok = (r == 0) && (t != BR_NULL) && (br_task_state(t) == BR_TASK_READY) &&
                       (br_task_self() != BR_NULL);

    conf_result(ok, "TC-TASK-001", "state=READY after create (and br_task_self() non-NULL)");
    if (t != BR_NULL) {
        (void)br_task_join(t, BR_NULL);
    }
}

static void case_task_002(void)
{
    br_task_attr_t a = conf_attr(1u, "t002");
    br_thread_t *t = BR_NULL;
    int code = -1;

    const int r = br_task_create(&t, &a, conf_entry_code, (void *)(br_intptr_t)42);
    const int j = (t != BR_NULL) ? br_task_join(t, &code) : BR_ERR(EINVAL);

    conf_result((r == 0) && (j == 0) && (code == 42), "TC-TASK-002",
                "join obtains exit code 42");
}

static void case_task_003_interleave(void)
{
    br_task_attr_t af = conf_attr(3u, "t003flag");
    br_task_attr_t a1 = conf_attr(2u, "workerA");
    br_task_attr_t a2 = conf_attr(3u, "workerB");
    br_thread_t *tf = BR_NULL;
    br_thread_t *ta = BR_NULL;
    br_thread_t *tb = BR_NULL;
    int code_a = -1;
    int code_b = -1;

    /*
     * (a) Direct check: after this thread yields, the other thread really ran (flag set).
     *     The ready queue holds only the flag thread now, so FIFO must run it first.
     */
    s_flag_a = 0;
    const int rf = br_task_create(&tf, &af, conf_entry_flag, BR_NULL);
    br_task_yield();
    const br_bool ran = (s_flag_a == 1);
    const int jf = (tf != BR_NULL) ? br_task_join(tf, BR_NULL) : BR_ERR(EINVAL);
    conf_result((rf == 0) && (jf == 0) && ran, "TC-TASK-003",
                "another thread really ran after yield");

    /*
     * (b) Interleave evidence: two workers each do "append letter + yield" x 4.
     *     join(A) blocks this thread — so the scheduler alternates A/B.
     */
    s_seq_n = 0;
    for (br_u32 i = 0u; i < BR_ARRAY_SIZE(s_seq); i++) {
        s_seq[i] = '.';
    }

    const int r1 = br_task_create(&ta, &a1, conf_entry_ab, (void *)(br_intptr_t)'A');
    const int r2 = br_task_create(&tb, &a2, conf_entry_ab, (void *)(br_intptr_t)'B');

    const int j1 = (ta != BR_NULL) ? br_task_join(ta, &code_a) : BR_ERR(EINVAL);
    const int j2 = (tb != BR_NULL) ? br_task_join(tb, &code_b) : BR_ERR(EINVAL);

    const char *p = (const char *)s_seq;
    const br_bool seq_ok = (s_seq_n == 8) &&
                           (p[0] == 'A') && (p[1] == 'B') && (p[2] == 'A') && (p[3] == 'B') &&
                           (p[4] == 'A') && (p[5] == 'B') && (p[6] == 'A') && (p[7] == 'B');

    br_log_info("[TASKCONF] TRACE coop interleave seq=%c%c%c%c%c%c%c%c",
                p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);

    conf_result((r1 == 0) && (r2 == 0) && (j1 == 0) && (j2 == 0) && (code_a == 0) &&
                (code_b == 0) && seq_ok,
                "TC-TASK-003", "two threads strictly alternate (ABABABAB): the forced switch point really switches");
}

static void case_task_004(void)
{
    br_task_attr_t a = conf_attr(4u, "t004");
    br_thread_t *t = BR_NULL;
    int code = -1;

    const int r = br_task_create(&t, &a, conf_entry_ok, BR_NULL);
    br_task_yield();                    /* let it finish (entry returns ⇒ trampoline ⇒ exit) */

    const br_bool zombie = (t != BR_NULL) && (br_task_state(t) == BR_TASK_ZOMBIE);
    const int j = (t != BR_NULL) ? br_task_join(t, &code) : BR_ERR(EINVAL);

    conf_result((r == 0) && zombie && (j == 0) && (code == 0), "TC-TASK-004",
                "state=ZOMBIE after exit and join reclaims the slot");
}

static void case_task_005(void)
{
    br_task_attr_t a = conf_attr(5u, "t005");
    br_task_attr_t bad = conf_attr(5u, "bad");
    br_thread_t *x = BR_NULL;
    int r[7];

    r[0] = br_task_create(BR_NULL, &a, conf_entry_ok, BR_NULL);
    r[1] = br_task_create(&x, BR_NULL, conf_entry_ok, BR_NULL);

    bad.stack = BR_NULL;
    r[2] = br_task_create(&x, &bad, conf_entry_ok, BR_NULL);

    bad = conf_attr(5u, "bad");
    bad.stack_size = 128u;              /* < BR_STACK_MIN */
    r[3] = br_task_create(&x, &bad, conf_entry_ok, BR_NULL);

    r[4] = br_task_create(&x, &a, BR_NULL, BR_NULL);
    r[5] = br_task_join(BR_NULL, BR_NULL);
    r[6] = br_task_join(br_task_self(), BR_NULL);

    br_bool all = BR_TRUE;
    for (br_u32 i = 0u; i < BR_ARRAY_SIZE(r); i++) {
        if (r[i] != BR_ERR(EINVAL)) {
            all = BR_FALSE;
        }
    }
    conf_result(all && (x == BR_NULL), "TC-TASK-005",
                "invalid args -EINVAL (create NULL out/attr/stack/entry + join NULL/self)");
}

static void case_task_006(void)
{
    br_thread_t *ts[BR_TASK_MAX + 2u];
    br_u32 n = 0u;
    int last = 0;

    for (br_u32 i = 0u; i < (BR_TASK_MAX + 2u); i++) {
        br_task_attr_t ai = conf_attr(i, "pool");
        const int r = br_task_create(&ts[n], &ai, conf_entry_ok, BR_NULL);
        if (r == 0) {
            n++;
        } else {
            last = r;
            break;
        }
    }

    conf_result(last == BR_ERR(ENOMEM), "TC-TASK-006",
                "TCB pool full ⇒ -ENOMEM (the pool limit is a hard bound)");

    for (br_u32 i = 0u; i < n; i++) {
        (void)br_task_join(ts[i], BR_NULL);
    }
}

static void case_task_007(void)
{
    br_task_attr_t a = conf_attr(7u, "t007");
    br_thread_t *t = BR_NULL;

    s_probe_addr = 0u;
    const int r = br_task_create(&t, &a, conf_entry_probe, BR_NULL);
    const int j = (t != BR_NULL) ? br_task_join(t, BR_NULL) : BR_ERR(EINVAL);

    const br_uintptr_t top = (br_uintptr_t)a.stack;
    const br_uintptr_t lo  = top - (br_uintptr_t)a.stack_size;
    const br_bool in_range = (s_probe_addr >= lo) && (s_probe_addr < top);

    conf_result((r == 0) && (j == 0) && in_range, "TC-TASK-007",
                "a local variable's address falls inside this thread's own stack range (the stack is really used)");
}

static void case_time_001(void)
{
    br_task_attr_t a = conf_attr(0u, "tsleep");
    br_thread_t *t = BR_NULL;

    s_sleep_delta = 0u;
    s_sleep_rc = -1;
    const int r = br_task_create(&t, &a, conf_entry_sleep, BR_NULL);
    const int j = (t != BR_NULL) ? br_task_join(t, BR_NULL) : BR_ERR(EINVAL);

    conf_result((r == 0) && (j == 0) && (s_sleep_rc == 0) && (s_sleep_delta >= 2000u),
                "TC-TIME-001", "sleep(2000us) does not wake early (measured >= requested)");
}

static void case_time_002(void)
{
    const br_time_t t0 = br_clock_now();
    const int r = br_task_sleep(0u);
    const br_time_t d = br_clock_now() - t0;

    /*
     * The check uses "measured < 1 ms" rather than `d == 0`: on target a timer ISR may land
     * between the two reads (it is preemptive), so d may be a few us — which does not
     * contradict "returns immediately". The real counterexample is "registered in the timeout
     * table and switched away", which takes at least one tick (`1/HZ`, 5 ms by default).
     */
    br_log_info("[TASKCONF] TRACE sleep0 rc=%d elapsed_us=%lu", r, (br_u64)d);
    conf_result((r == 0) && (d < BR_US_PER_MS), "TC-TIME-002",
                "sleep(0) returns 0 immediately (measured < 1ms, never entered the timeout table)");
}

static void case_time_003(void)
{
    const int r = br_task_sleep(BR_TIMEOUT_INF);

    conf_result(r == BR_ERR(EINVAL), "TC-TIME-003", "sleep(INF) ⇒ -EINVAL (ruling G13)");
}

/*
 * TC-TASK-104 (ADR-0011 §5): the observable face of I2 — "a RUNNING thread is not in the
 * ready structure".
 *
 * Why this deserves a case: I2 states "the state machine has one source of truth", but it
 * stayed a paper rule (ADR-0006 §1 lists it without a check). Preemption raises the stakes:
 * the IRQ exit switch stack cycles through "push to tail → pop → set RUNNING", and one wrong
 * write leaves a ghost node that is "not runnable but still queued" or "RUNNING but still queued".
 *
 * The check uses relative counts (not absolute): in the image selftest runs in the main
 * context while the APP thread already sits in the ready queue, so the absolute value is not
 * 0. Instead it asserts "current (RUNNING) is never counted": create one thread ⇒ +1; join it
 * ⇒ back to baseline.
 */
static void case_task_104_ready_set(void)
{
    br_task_attr_t a = conf_attr(6u, "t104");
    br_thread_t *t = BR_NULL;

    const br_u32 base = br_sched_ready_count();

    const int r = br_task_create(&t, &a, conf_entry_ok, BR_NULL);
    const br_u32 after_create = br_sched_ready_count();

    /* just wait for it to finish (trampoline → exit → ZOMBIE): it is then neither queued nor RUNNING. */
    br_task_yield();
    const br_u32 after_exit = br_sched_ready_count();

    const int j = (t != BR_NULL) ? br_task_join(t, BR_NULL) : BR_ERR(EINVAL);
    const br_u32 after_join = br_sched_ready_count();

    br_log_info("[TASKCONF] TRACE ready-set base=%u +create=%u +exit=%u +join=%u",
                base, after_create, after_exit, after_join);

    /*
     * Four values matching point by point (note ZOMBIE is NOT in the ready structure, so the
     * count drops back to baseline once it finishes):
     *   create ⇒ base+1 (READY); exit ⇒ base (ZOMBIE); join reclaims ⇒ base (slot released).
     */
    conf_result((r == 0) && (j == 0) && (after_create == base + 1u) &&
                (after_exit == base) && (after_join == base),
                "TC-TASK-104",
                "I2: READY ⇄ ready structure; RUNNING/ZOMBIE are never in it (relative counts match point by point)");
}

/* ---- Domain-specific: the coop definition (no yield ⇒ no preemption) ---- */

static void case_task_101_coop(void)
{
    br_task_attr_t ab = { .name = "busy",
                          .stack = (void *)(s_busy_stack + BR_CONF_STACK_BYTES),
                          .stack_size = BR_CONF_STACK_BYTES,
                          .prio = 0u, .flags = 0u };
    br_task_attr_t av = { .name = "victim",
                          .stack = (void *)(s_victim_stack + BR_CONF_STACK_BYTES),
                          .stack_size = BR_CONF_STACK_BYTES,
                          .prio = 0u, .flags = 0u };
    br_thread_t *tb = BR_NULL;
    br_thread_t *tv = BR_NULL;

    s_victim_ticks = 0u;
    s_preempt_seen = 0;
    s_victim_after_yield = 0;

    /* create busy before victim ⇒ ready order [busy, victim]; after join(busy) busy runs first. */
    const int r1 = br_task_create(&tb, &ab, conf_entry_busy, BR_NULL);
    const int r2 = br_task_create(&tv, &av, conf_entry_victim, BR_NULL);
    const int j1 = (tb != BR_NULL) ? br_task_join(tb, BR_NULL) : BR_ERR(EINVAL);
    const int j2 = (tv != BR_NULL) ? br_task_join(tv, BR_NULL) : BR_ERR(EINVAL);

    conf_result((r1 == 0) && (r2 == 0) && (j1 == 0) && (j2 == 0) &&
                (s_preempt_seen == 0) && (s_victim_after_yield == 1) &&
                (s_victim_ticks == 1u),
                "TC-TASK-101", "coop: a busy thread that never yields is not preempted; victim runs only after yield");
}

/* ---- Domain-specific: the preempt definition (the same shape must be sliced away) ---- */

static void case_task_102_preempt(void)
{
    br_task_attr_t ab = { .name = "busy",
                          .stack = (void *)(s_busy_stack + BR_CONF_STACK_BYTES),
                          .stack_size = BR_CONF_STACK_BYTES,
                          .prio = 0u, .flags = 0u };
    br_task_attr_t av = { .name = "victim",
                          .stack = (void *)(s_victim_stack + BR_CONF_STACK_BYTES),
                          .stack_size = BR_CONF_STACK_BYTES,
                          .prio = 0u, .flags = 0u };
    br_thread_t *tb = BR_NULL;
    br_thread_t *tv = BR_NULL;

    s_victim_ticks = 0u;
    s_preempt_seen = 0;
    s_victim_after_yield = 0;

    const br_time_t t0 = br_clock_now();

    const int r1 = br_task_create(&tb, &ab, conf_entry_busy_preempt, BR_NULL);
    const int r2 = br_task_create(&tv, &av, conf_entry_victim, BR_NULL);
    const int j1 = (tb != BR_NULL) ? br_task_join(tb, BR_NULL) : BR_ERR(EINVAL);
    const int j2 = (tv != BR_NULL) ? br_task_join(tv, BR_NULL) : BR_ERR(EINVAL);
    const br_time_t elapsed = br_clock_now() - t0;

    br_log_info("[TASKCONF] TRACE preempt busy_us=%lu victim_ticks=%u seen=%d",
                (br_u64)elapsed, (unsigned)s_victim_ticks, s_preempt_seen);

    /*
     * Criterion = the victim ran during the busy loop (before busy yielded) ⇒ the time slice
     * really switched busy out. This is the OPPOSITE of TC-TASK-101: the same code must be
     * false under coop and true under preempt.
     */
    conf_result((r1 == 0) && (r2 == 0) && (j1 == 0) && (j2 == 0) &&
                (s_preempt_seen == 1),
                "TC-TASK-102",
                "preempt: a busy thread that never yields is preempted by the slice (victim ran before busy yielded)");
}

static void case_task_103_critical(void)
{
    br_task_attr_t ac = { .name = "crit",
                          .stack = (void *)(s_busy_stack + BR_CONF_STACK_BYTES),
                          .stack_size = BR_CONF_STACK_BYTES,
                          .prio = 0u, .flags = 0u };
    br_task_attr_t av = { .name = "victim",
                          .stack = (void *)(s_victim_stack + BR_CONF_STACK_BYTES),
                          .stack_size = BR_CONF_STACK_BYTES,
                          .prio = 0u, .flags = 0u };
    br_thread_t *tc = BR_NULL;
    br_thread_t *tv = BR_NULL;

    s_victim_ticks = 0u;
    s_crit_violation = 0;
    s_crit_ticks = 0u;
    s_victim_after_yield = 0;

    const int r1 = br_task_create(&tc, &ac, conf_entry_crit, BR_NULL);
    const int r2 = br_task_create(&tv, &av, conf_entry_victim, BR_NULL);
    const int j1 = (tc != BR_NULL) ? br_task_join(tc, BR_NULL) : BR_ERR(EINVAL);
    const int j2 = (tv != BR_NULL) ? br_task_join(tv, BR_NULL) : BR_ERR(EINVAL);

    br_log_info("[TASKCONF] TRACE crit spins=%u violation=%d after_yield=%d",
                (unsigned)s_crit_ticks, s_crit_violation, s_victim_after_yield);

    conf_result((r1 == 0) && (r2 == 0) && (j1 == 0) && (j2 == 0) &&
                (s_crit_ticks > 0u) && (s_crit_violation == 0) &&
                (s_victim_after_yield == 1),
                "TC-TASK-103",
                "preempt: no preemption inside an L2 critical section (IRQs off ⇒ no IRQ exit); victim can run at once after release");
}

int br_sched_selftest(void)
{
    s_conf_pass = 0;
    s_conf_fail = 0;

    /* read-only registry snapshot (byte-identical to the old code, which read s_ops in sched_core.c). */
    const br_sched_ops_t *ops = br_sched_ops_get();

    br_log_info("[TASKCONF] ==== sched framework + scheduler conformance (3-03 / 6-01 §3.1/§3.3)");
    br_log_info("[TASKCONF] ---- scheduler=%s kind=%u self=%s spawn=%u",
                (ops != BR_NULL) ? ops->name : "?",
                (ops != BR_NULL) ? ops->kind : 0u,
                br_task_name(br_task_self()),
                (br_u32)(ops != BR_NULL ? ops->tcb_size : 0u));

    case_task_001();
    case_task_002();
    case_task_003_interleave();
    case_task_004();
    case_task_005();
    case_task_006();
    case_task_007();
    case_task_104_ready_set();
    case_time_001();
    case_time_002();
    case_time_003();

    /*
     * ---- Domain-specific: branch on the registry `kind` (not a compile-time switch) ----
     * coop and preempt are the two directions of one boundary ("does a busy thread that never
     * yields get switched away"): running both would be self-contradictory, so only one side
     * runs here — chosen from the truth in `ops->kind`.
     */
    const br_u32 kind = (ops != BR_NULL) ? ops->kind : BR_SCHED_KIND_COOP;

    if (kind == BR_SCHED_KIND_COOP) {
        case_task_101_coop();
    } else {
        /* preempt / tt: the slice (or schedule table) preempts ⇒ the check flips. */
        case_task_102_preempt();
        case_task_103_critical();
    }

    br_log_info("[TASKCONF] SUMMARY pass=%d fail=%d total=%d",
                s_conf_pass, s_conf_fail, s_conf_pass + s_conf_fail);

    if (s_conf_fail != 0) {
        br_log_error("[TASKCONF] failures present — sched framework/coop not conformant");
    }

    /* return the failure count: the aggregator sums it, gates decide red/green (ADR-0010 §2.4). */
    return s_conf_fail;
}
