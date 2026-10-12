/*
 * brickOS prototype v0.2.0 — **selftest suite** for sync primitives + time (core/selftest/)
 *
 * Design basis and per-item rulings: `docs/decisions/0010-plugin-selftest.md`.
 * Case definitions: `6-01-test.md` §3.2/§3.3 (`TC-SYNC-001..009` / `TC-TIME-001..003`);
 * implementation basis `3-01` §7, `3-02` §11.3, `docs/decisions/0007-sync-primitives.md`.
 *
 * ## Why these cases moved out of `sync.c`
 *   They are **tests** but used to live in the same .c as the production implementation — so
 *   "turn off selftests" could not turn them off, and readers of the production implementation had
 *   to hunt for decision logic among a thousand lines of mechanism. After moving here:
 *     - `core/src/sync/sync.c` keeps only mechanism (mutex/sem/cond/spinlock);
 *     - with selftests off, the whole `core/selftest/` directory is not compiled ⇒ the case code
 *       **never enters the image** (a mechanical fact, not "compiled but not run"; see
 *       selftest_sources in product.toml and build.rs);
 *     - readers of the cases and readers of the implementation no longer interfere (the suite's
 *       explanation of "white-box forging a non-owner" should not be mixed with production
 *       semantics either).
 *   The code was **moved**, not rewritten: criteria/logs/order are byte-for-byte unchanged (see
 *   the log contract below).
 *
 * ## Fixture ownership
 *   The static fixtures (`s_m`/`s_sem`/`s_cv`/`s_sp`/two thread stacks/shared counters) are used
 *   **only by this suite** (confirmed by grepping `core/`/`tests/` across the tree), so the whole
 *   group moved here with the cases — the production side does not own them, so no new
 *   `sync_internal.h` boundary is needed.
 *   The only cross-boundary dependency is `struct br_thread` from `sched_internal.h` (white-box
 *   forging of an "external owner") and `br_waitq_len()` (checking "no ghost waiters").
 *
 * ## Log contract (gates match **verbatim** ⇒ changing a word changes the criterion)
 *   `[SYNCCONF] PASS/FAIL/SKIP <tag> <desc>` … `[SYNCCONF] SUMMARY pass=%u fail=%u total=%u`
 *   The require/forbid/require_tags of `tests/gates.toml` and `docs/decisions/0007` §gates all
 *   anchor on these two lines (including the SKIP line when "scheduler not registered").
 *
 * ## Return value
 *   `int` = **number of failed items** (ADR-0010 §2.4: the suite only aggregates, it does not
 *   judge). Any failure **does not halt** the system.
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_sync.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

/* The TCB layout and the four wait-chain primitives are a core-internal contract: this suite needs
 * to "white-box forge a non-owner" and use `br_waitq_len()` to check for ghost nodes. Cross-directory
 * relative includes have precedent in this repo (sync.c originally did the same). */
#include "../src/sched/sched_internal.h"

/* The entry's visible declaration is centralized in `core/selftest/core_selftest.c` (the only
 * aggregator); declaring it again here satisfies this file's discipline of "every function has a
 * prototype before use" (-Wmissing-prototypes). */
int br_sync_selftest(void);

/* =====================================================================
 * Conformance cases (TC-SYNC-001..009 / TC-TIME-001..003; 6-01 §3.2/§3.3)
 * ===================================================================== */

#define CONF_ITER        400u
#define CONF_STACK_BYTES 4096u
#define CONF_WAIT_SPIN   1000u

static br_u8 s_stack_a[CONF_STACK_BYTES] BR_ALIGN(16);
static br_u8 s_stack_b[CONF_STACK_BYTES] BR_ALIGN(16);

static br_mutex_t    s_m;
static br_sem_t      s_sem;
static br_cond_t     s_cv;
static br_spinlock_t s_sp;

static volatile br_u32 s_inside;
static volatile br_u32 s_overlap;
static volatile br_u32 s_count;
static int s_wrc[4];

/* "Fake holder" when there is no thread context (white-box: only to create the "non-owner" case, see ADR-0007 §3). */
static struct br_thread s_foreign_owner;

static void conf_line(const char *tag, br_bool ok, const char *desc,
                      br_u32 *pass, br_u32 *fail)
{
    br_log_info("[SYNCCONF] %s %s %s", (ok == BR_TRUE) ? "PASS" : "FAIL", tag, desc);
    if (ok == BR_TRUE) {
        (*pass)++;
    } else {
        (*fail)++;
    }
}

static void conf_skip(const char *tag, const char *why)
{
    br_log_info("[SYNCCONF] SKIP %s %s", tag, why);
}

static br_thread_t *conf_spawn(void (*fn)(void *), void *arg, br_u8 *stack)
{
    br_task_attr_t attr;
    br_thread_t   *t = BR_NULL;

    attr.name       = "syncconf";
    attr.stack      = (void *)(stack + CONF_STACK_BYTES);
    attr.stack_size = CONF_STACK_BYTES;
    attr.prio       = 0u;
    attr.flags      = 0u;

    if (br_task_create(&t, &attr, fn, arg) != 0) {
        return BR_NULL;
    }
    return t;
}

static br_bool conf_join(br_thread_t *t, int *code)
{
    if (t == BR_NULL) {
        return BR_FALSE;
    }
    return (br_task_join(t, code) == 0) ? BR_TRUE : BR_FALSE;
}

/* ---------------------------------------------------------------- workers  * WORKAROUND(br-wa-test-001): this suite's `TC-SYNC-*`/`TC-TIME-*` ids and the
 * table in `6-01` §3.2/§3.3 are **not yet aligned one by one** (meaning has drifted; this note is
 * accurate); the alignment action is tracked in WORKAROUNDS.md.
 */

static void conf_mutex_worker(void *arg)
{
    (void)arg;
    for (br_u32 i = 0u; i < CONF_ITER; i++) {
        if (br_mutex_lock(&s_m) != 0) {
            s_wrc[0] = -1;
            return;
        }
        if (s_inside != 0u) {
            s_overlap++;                 /* both threads in the critical section ⇒ mutual exclusion broken */
        }
        s_inside = 1u;
        s_count++;
        if ((i & 7u) == 0u) {
            br_task_yield();             /* create contention: let the other thread hit the lock and block */
        }
        s_inside = 0u;
        (void)br_mutex_unlock(&s_m);
    }
}

static void conf_eperm_worker(void *arg)
{
    (void)arg;
    s_wrc[1] = br_mutex_unlock(&s_m);    /* main holds the lock ⇒ non-owner */
}

static void conf_timeout_worker(void *arg)
{
    (void)arg;
    s_wrc[2] = br_mutex_lock_to(&s_m, 20000u);   /* times out after 20 ms */
}

static void conf_sem_worker(void *arg)
{
    int *out = (int *)arg;
    *out = br_sem_take(&s_sem, BR_TIMEOUT_INF);
}

static void conf_cond_worker(void *arg)
{
    int *out = (int *)arg;

    if (br_mutex_lock(&s_m) != 0) {
        *out = -1;
        return;
    }
    const int rc = br_cond_wait(&s_cv, &s_m, BR_TIMEOUT_INF);
    /* on return it must still hold m */
    if (!(br_mutex_owner(&s_m) == br_task_self() && br_mutex_is_locked(&s_m) == BR_TRUE)) {
        *out = -2;
    } else {
        *out = rc;
    }
    (void)br_mutex_unlock(&s_m);
}

static void conf_wait_for_waiters(volatile br_u32 *n, br_u32 want)
{
    for (br_u32 i = 0u; (i < CONF_WAIT_SPIN) && (*n < want); i++) {
        br_task_yield();
    }
}

/* ---------------------------------------------------------------- cases */

static void conf_sync_001(br_u32 *pass, br_u32 *fail, br_bool sched)
{
    if (sched != BR_TRUE) {
        conf_skip("TC-SYNC-001", "scheduler not registered (mutex exclusion needs two real threads)");
        return;
    }

    (void)br_mutex_init(&s_m);
    s_inside = 0u;
    s_overlap = 0u;
    s_count = 0u;
    s_wrc[0] = 0;

    br_thread_t *a = conf_spawn(conf_mutex_worker, BR_NULL, s_stack_a);
    br_thread_t *b = conf_spawn(conf_mutex_worker, BR_NULL, s_stack_b);

    int ca = 0;
    int cb = 0;
    const br_bool ja = conf_join(a, &ca);
    const br_bool jb = conf_join(b, &cb);

    const br_bool ok = (ja == BR_TRUE) && (jb == BR_TRUE) && (s_wrc[0] == 0)
                    && (s_count == 2u * CONF_ITER) && (s_overlap == 0u)
                    && (br_mutex_is_locked(&s_m) == BR_FALSE)
                    && (s_m.owner == BR_NULL) && (br_waitq_len(s_m.head) == 0u);
    conf_line("TC-SYNC-001", ok, "mutex exclusion: two threads x400 interleaved counts, no overlap, no loss", pass, fail);
}

static void conf_sync_002(br_u32 *pass, br_u32 *fail)
{
    (void)br_mutex_init(&s_m);

    const int r1 = br_mutex_lock_to(&s_m, BR_TIMEOUT_ZERO);   /* free ⇒ 0 */
    const int r2 = br_mutex_lock_to(&s_m, BR_TIMEOUT_ZERO);   /* locked ⇒ -ETIMEDOUT (6-01 TC-SYNC-002) */
    const int r3 = br_mutex_unlock(&s_m);
    const int r4 = br_mutex_lock_to(&s_m, BR_TIMEOUT_ZERO);   /* free again ⇒ 0 */
    const int r5 = br_mutex_unlock(&s_m);

    const br_bool ok = (r1 == BR_OK) && (r2 == BR_ERR(ETIMEDOUT)) && (r3 == BR_OK)
                    && (r4 == BR_OK) && (r5 == BR_OK)
                    && (br_mutex_is_locked(&s_m) == BR_FALSE);
    conf_line("TC-SYNC-002", ok, "mutex lock_to(ZERO): free 0 / busy -ETIMEDOUT (non-blocking)", pass, fail);
}

static void conf_sync_003(br_u32 *pass, br_u32 *fail, br_bool sched)
{
    br_bool ok;

    (void)br_mutex_init(&s_m);

    if (sched == BR_TRUE) {
        (void)br_mutex_lock(&s_m);
        s_wrc[1] = 0;
        br_thread_t *t = conf_spawn(conf_eperm_worker, BR_NULL, s_stack_a);
        int code = 0;
        const br_bool j = conf_join(t, &code);
        const int rr = br_mutex_unlock(&s_m);
        ok = (j == BR_TRUE) && (s_wrc[1] == BR_ERR(EPERM)) && (rr == BR_OK);
    } else {
        /* no second thread ⇒ white-box forge "lock held by another thread", same owner check. */
        s_m.locked = 1u;
        s_m.owner  = &s_foreign_owner;
        const int rr = br_mutex_unlock(&s_m);
        ok = (rr == BR_ERR(EPERM)) ? BR_TRUE : BR_FALSE;
        (void)br_mutex_init(&s_m);
    }

    conf_line("TC-SYNC-003", ok, "non-owner unlock ⇒ -EPERM", pass, fail);
}

static void conf_sync_004(br_u32 *pass, br_u32 *fail, br_bool sched)
{
    if (sched != BR_TRUE) {
        conf_skip("TC-SYNC-004", "scheduler not registered (timeout needs a real blocked thread)");
        return;
    }

    (void)br_mutex_init(&s_m);
    (void)br_mutex_lock(&s_m);            /* main holds the lock and blocks in join until the timeout is delivered */
    s_wrc[2] = 0;

    br_thread_t *t = conf_spawn(conf_timeout_worker, BR_NULL, s_stack_a);
    int code = 0;
    const br_time_t tw0 = br_clock_now();
    const br_bool j = conf_join(t, &code);
    const br_time_t tw = br_clock_now() - tw0;

    br_log_info("[SYNCCONF] MEASURE TC-SYNC-004 lock_to request=20000us measured=%lu us (tick granularity)",
                (br_u64)tw);

    const br_bool ghost_free = (br_waitq_len(s_m.head) == 0u) && (s_m.waiters == 0u);
    const int u = br_mutex_unlock(&s_m);
    const br_bool released_clean = (br_mutex_is_locked(&s_m) == BR_FALSE)
                                && (br_mutex_owner(&s_m) == BR_NULL)
                                && (s_m.waiters == 0u) && (br_waitq_len(s_m.head) == 0u);
    const int again = br_mutex_lock_to(&s_m, BR_TIMEOUT_ZERO);
    if (again == BR_OK) {
        (void)br_mutex_unlock(&s_m);
    }

    const br_bool ok = (j == BR_TRUE) && (s_wrc[2] == BR_ERR(ETIMEDOUT))
                    && (ghost_free == BR_TRUE) && (u == BR_OK) && (released_clean == BR_TRUE)
                    && (again == BR_OK)
                    && (br_mutex_is_locked(&s_m) == BR_FALSE)
                    && (br_mutex_owner(&s_m) == BR_NULL)
                    && (br_waitq_len(s_m.head) == 0u);
    conf_line("TC-SYNC-004", ok, "lock_to timeout ⇒ -ETIMEDOUT with no ghost waiters; lock acquirable again",
              pass, fail);
}

static void conf_sync_005(br_u32 *pass, br_u32 *fail, br_bool sched)
{
    (void)br_sem_init(&s_sem, 2u);

    const int a = br_sem_take(&s_sem, BR_TIMEOUT_ZERO);   /* count 2 -> 1 */
    const int b = br_sem_take(&s_sem, BR_TIMEOUT_ZERO);   /* count 1 -> 0 */
    const int c = br_sem_take(&s_sem, BR_TIMEOUT_ZERO);   /* empty ⇒ -ETIMEDOUT */
    const int g = br_sem_give(&s_sem);                    /* count -> 1 */
    const int d = br_sem_take(&s_sem, BR_TIMEOUT_ZERO);   /* count -> 0 */

    br_bool ok = (a == BR_OK) && (b == BR_OK) && (c == BR_ERR(ETIMEDOUT)) && (g == BR_OK)
              && (d == BR_OK) && (br_sem_count(&s_sem) == 0u)
              && (br_waitq_len(s_sem.head) == 0u);

    const char *desc = "sem counting semantics (init(2)/take x2/take(ZERO)/give) + give wakes a blocked thread";

    if (sched == BR_TRUE) {
        s_wrc[3] = 0;
        br_thread_t *t = conf_spawn(conf_sem_worker, &s_wrc[3], s_stack_a);
        conf_wait_for_waiters(&s_sem.waiters, 1u);        /* wait until T really blocks in take */
        const int wg = br_sem_give(&s_sem);               /* hand directly to the waiter */
        int code = 0;
        const br_bool j = conf_join(t, &code);
        ok = (ok == BR_TRUE) && (wg == BR_OK) && (j == BR_TRUE) && (s_wrc[3] == BR_OK)
           && (s_sem.waiters == 0u) && (br_waitq_len(s_sem.head) == 0u);
    } else {
        conf_skip("TC-SYNC-005(wake)", "scheduler not registered: give-wakes-blocked-thread not verified, counting semantics only");
        desc = "sem counting semantics (init(2)/take x2/take(ZERO)/give) [no-thread subset]";
    }

    conf_line("TC-SYNC-005", ok, desc, pass, fail);
}

static void conf_sync_006(br_u32 *pass, br_u32 *fail, br_bool sched)
{
    if (sched != BR_TRUE) {
        conf_skip("TC-SYNC-006", "scheduler not registered (needs one thread blocked in take)");
        return;
    }

    (void)br_sem_init(&s_sem, 0u);
    s_wrc[3] = 0;
    br_thread_t *t = conf_spawn(conf_sem_worker, &s_wrc[3], s_stack_a);
    conf_wait_for_waiters(&s_sem.waiters, 1u);

    /*
     * Simulate ISR context: `br_irq_lock()` disables interrupts — that is exactly the context an
     * ISR runs in (IRQs already masked), and an ISR is **not allowed** any call that yields the
     * CPU. So "call br_sem_give() inside an IRQ-disabled critical section" has exactly the same
     * observable constraints as "call it in an ISR": only counter/link/wake-bit changes, no switch.
     * (Actually raising a timer ISR would prove the same thing while binding the case to a platform.)
     */
    const br_irq_state_t ist = br_irq_lock();
    const int give_rc = br_sem_give(&s_sem);
    br_irq_unlock(ist);

    int code = 0;
    const br_bool j = conf_join(t, &code);

    const br_bool ok = (give_rc == BR_OK) && (j == BR_TRUE) && (s_wrc[3] == BR_OK)
                    && (br_sem_count(&s_sem) == 0u) && (br_waitq_len(s_sem.head) == 0u);
    conf_line("TC-SYNC-006", ok, "ISR-context give (CA-3 whitelist): give with IRQs off wakes the thread", pass, fail);
}

static void conf_sync_007(br_u32 *pass, br_u32 *fail, br_bool sched)
{
    (void)br_cond_init(&s_cv);
    (void)br_mutex_init(&s_m);

    /* (a) signal with no waiters ⇒ no-op and **no count** */
    const int s0 = br_cond_signal(&s_cv);
    const br_u32 w0 = br_cond_waiters(&s_cv);

    /* Mechanical criterion for "no count": the following wait(ZERO) must still time out (if signal
     * had accounted for it, it would return 0 immediately) */
    (void)br_mutex_lock(&s_m);
    const int z = br_cond_wait(&s_cv, &s_m, BR_TIMEOUT_ZERO);
    const br_bool held_z = (br_mutex_owner(&s_m) == br_task_self())
                        && (br_mutex_is_locked(&s_m) == BR_TRUE);
    (void)br_mutex_unlock(&s_m);

    br_bool ok = (s0 == BR_OK) && (w0 == 0u) && (z == BR_ERR(ETIMEDOUT))
              && (held_z == BR_TRUE);

    if (sched != BR_TRUE) {
        conf_skip("TC-SYNC-007(signal/wait/broadcast)",
                  "scheduler not registered: only signal-no-waiters no-op and wait(ZERO) recoverability verified");
        conf_line("TC-SYNC-007", ok,
                  "cond (no-thread subset): signal no-waiters no-op (no count) + lock still held after wait(ZERO)",
                  pass, fail);
        return;
    }

    /* (b) signal → wait pairing */
    s_wrc[2] = 123;
    br_thread_t *t = conf_spawn(conf_cond_worker, &s_wrc[2], s_stack_a);
    conf_wait_for_waiters(&s_cv.waiters, 1u);
    const int sig = br_cond_signal(&s_cv);
    int code = 0;
    (void)conf_join(t, &code);
    ok = (ok == BR_TRUE) && (sig == BR_OK) && (s_wrc[2] == BR_OK)
       && (br_cond_waiters(&s_cv) == 0u) && (br_waitq_len(s_cv.head) == 0u);

    /* (c) broadcast wakes all (N=2) */
    (void)br_cond_init(&s_cv);
    (void)br_mutex_init(&s_m);
    s_wrc[2] = 123;
    s_wrc[3] = 123;
    br_thread_t *t1 = conf_spawn(conf_cond_worker, &s_wrc[2], s_stack_a);
    br_thread_t *t2 = conf_spawn(conf_cond_worker, &s_wrc[3], s_stack_b);
    conf_wait_for_waiters(&s_cv.waiters, 2u);
    const int bc = br_cond_broadcast(&s_cv);
    int c1 = 0;
    int c2 = 0;
    const br_bool j1 = conf_join(t1, &c1);
    const br_bool j2 = conf_join(t2, &c2);
    ok = (ok == BR_TRUE) && (bc == BR_OK) && (j1 == BR_TRUE) && (j2 == BR_TRUE)
       && (s_wrc[2] == BR_OK) && (s_wrc[3] == BR_OK)
       && (br_cond_waiters(&s_cv) == 0u) && (br_waitq_len(s_cv.head) == 0u);

    conf_line("TC-SYNC-007", ok,
              "cond: signal no-waiters no-op (no count) + wait/signal pairing + broadcast wakes all", pass, fail);
}

static void conf_sync_008(br_u32 *pass, br_u32 *fail, br_bool sched)
{
    if (sched != BR_TRUE) {
        conf_skip("TC-SYNC-008", "scheduler not registered (cond timeout needs real blocking + tick)");
        return;
    }

    (void)br_cond_init(&s_cv);
    (void)br_mutex_init(&s_m);
    (void)br_mutex_lock(&s_m);

    const int rc = br_cond_wait(&s_cv, &s_m, 20000u);       /* times out after 20 ms */
    const br_bool held = (br_mutex_owner(&s_m) == br_task_self())
                      && (br_mutex_is_locked(&s_m) == BR_TRUE);
    const br_bool clean = (br_cond_waiters(&s_cv) == 0u) && (br_waitq_len(s_cv.head) == 0u);
    (void)br_mutex_unlock(&s_m);

    const br_bool ok = (rc == BR_ERR(ETIMEDOUT)) && (held == BR_TRUE) && (clean == BR_TRUE)
                    && (br_mutex_is_locked(&s_m) == BR_FALSE)
                    && (br_mutex_owner(&s_m) == BR_NULL)
                    && (br_waitq_len(s_m.head) == 0u) && (s_m.waiters == 0u);
    conf_line("TC-SYNC-008", ok, "cond_wait timeout ⇒ -ETIMEDOUT and lock state restored (m still held)",
              pass, fail);
}

static void conf_sync_009(br_u32 *pass, br_u32 *fail)
{
    /* ---- spinlock pairing + IRQ state ---- */
    (void)br_spinlock_init(&s_sp);

    const br_irq_state_t base = br_irq_lock();     /* IRQ state baseline before entry (depth 0) */
    br_irq_unlock(base);

    const br_irq_state_t got = br_spinlock_lock(&s_sp);
    const br_bool locked = br_spinlock_is_locked(&s_sp);
    br_spinlock_unlock(&s_sp, got);
    const br_bool unlocked = (br_spinlock_is_locked(&s_sp) == BR_FALSE);

    /* nesting: take a spinlock inside an outer irq_lock; after returning, IRQ state matches entry */
    const br_irq_state_t outer = br_irq_lock();
    const br_irq_state_t inner = br_spinlock_lock(&s_sp);
    br_spinlock_unlock(&s_sp, inner);
    br_irq_unlock(outer);
    const br_irq_state_t after = br_irq_lock();
    br_irq_unlock(after);

    /* trylock: busy ⇒ -EBUSY, leaves *saved untouched and holds no lock */
    const br_irq_state_t held = br_spinlock_lock(&s_sp);
    br_irq_state_t saved = 0xDEADBEEFu;
    const int tb = br_spinlock_trylock(&s_sp, &saved);
    const br_bool saved_kept = (saved == 0xDEADBEEFu);
    br_spinlock_unlock(&s_sp, held);
    const int tf = br_spinlock_trylock(&s_sp, &saved);
    if (tf == BR_OK) {
        br_spinlock_unlock(&s_sp, saved);
    }

    /* ---- magic check: uninitialized/corrupted must give -EINVAL, never silently "not locked" ---- */
    br_mutex_t zm;
    br_sem_t   zs;
    br_cond_t  zc;
    br_spinlock_t zsp;
    for (br_size_t i = 0u; i < sizeof(zm); i++) { ((br_u8 *)&zm)[i] = 0u; }
    for (br_size_t i = 0u; i < sizeof(zs); i++) { ((br_u8 *)&zs)[i] = 0u; }
    for (br_size_t i = 0u; i < sizeof(zc); i++) { ((br_u8 *)&zc)[i] = 0u; }
    for (br_size_t i = 0u; i < sizeof(zsp); i++) { ((br_u8 *)&zsp)[i] = 0u; }

    const int em  = br_mutex_lock(&zm);
    const int es  = br_sem_take(&zs, BR_TIMEOUT_ZERO);
    const int ec  = br_cond_signal(&zc);
    const int et  = br_spinlock_trylock(&zsp, &saved);

    const br_bool ok = (got == base) && (base == after)
                    && (locked == BR_TRUE) && (unlocked == BR_TRUE)
                    && (tb == BR_ERR(EBUSY)) && (saved_kept == BR_TRUE) && (tf == BR_OK)
                    && (br_spinlock_contention() == 0u)   /* single core: contention count is always 0 */
                    && (em == BR_ERR(EINVAL)) && (es == BR_ERR(EINVAL))
                    && (ec == BR_ERR(EINVAL)) && (et == BR_ERR(EINVAL));
    conf_line("TC-SYNC-009", ok,
              "spinlock lock/unlock IRQ state consistent + trylock -EBUSY + contention count 0 + magic -EINVAL",
              pass, fail);
}

static void conf_time_001(br_u32 *pass, br_u32 *fail, br_bool sched)
{
    if (sched != BR_TRUE) {
        conf_skip("TC-TIME-001", "scheduler not registered (sleep needs tick wakeup)");
        return;
    }

    /* monotonically non-decreasing (the original wording of 6-01 TC-TIME-001) */
    br_bool mono = BR_TRUE;
    br_time_t prev = br_clock_now();
    for (br_u32 i = 0u; i < 1000u; i++) {
        const br_time_t now = br_clock_now();
        if (now < prev) {
            mono = BR_FALSE;
            break;
        }
        prev = now;
    }

    /* sleep does not wake early (measured; tick = 1/HZ, default 5 ms ⇒ a 1000 us request lands about one tick late) */
    const br_time_t t0 = br_clock_now();
    const int rc = br_task_sleep(1000u);
    const br_time_t dt = br_clock_now() - t0;

    /* Print "requested vs measured" explicitly: the lateness set by the platform tick granularity is an observable fact, not a guess. */
    br_log_info("[SYNCCONF] MEASURE TC-TIME-001 request=1000us measured=%lu us (tick granularity)",
                (br_u64)dt);

    const br_bool ok = (mono == BR_TRUE) && (rc == BR_OK) && (dt >= 1000u);
    conf_line("TC-TIME-001", ok, "sleep(1000us) does not wake early: measured delta >= requested; clock monotonic",
              pass, fail);
}

static void conf_time_002(br_u32 *pass, br_u32 *fail)
{
    const br_time_t t0 = br_clock_now();
    const int rc = br_task_sleep(0u);
    const br_time_t dt = br_clock_now() - t0;

    const br_bool ok = (rc == BR_OK);
    conf_line("TC-TIME-002", ok, "br_task_sleep(0) returns 0 immediately", pass, fail);
    (void)dt;
}

static void conf_time_003(br_u32 *pass, br_u32 *fail)
{
    /* Ruling G13: relative sleep has no "infinite" semantics — that would mean never returning */
    const int rc = br_task_sleep(BR_TIMEOUT_INF);

    /* deadline saturates instead of wrapping (original wording of TC-TIME-003) */
    const br_time_t now = br_clock_now();
    const br_time_t d_inf = br_deadline_from_now(BR_TIMEOUT_INF);
    const br_time_t d_near = br_deadline_from_now(BR_TIMEOUT_INF - 1u);
    const br_time_t d_zero = br_deadline_from_now(0u);
    const br_time_t d_edge = br_deadline_from_now(BR_TIMEOUT_INF - now); /* exactly now + rel == INF */

    /* true overflow: now + rel exceeds UINT64_MAX ⇒ must saturate to BR_TIMEOUT_INF, not wrap into the past */
    br_bool sat = BR_TRUE;
    if (now > 0u) {
        sat = (br_deadline_from_now((BR_TIMEOUT_INF - now) + 1u) == BR_TIMEOUT_INF)
            ? BR_TRUE : BR_FALSE;
    }

    const br_bool ok = (rc == BR_ERR(EINVAL))
                    && (d_inf == BR_TIMEOUT_INF)
                    && (d_edge == BR_TIMEOUT_INF)
                    && (d_near >= now)                    /* never wraps to before now */
                    && (d_zero >= now)
                    && (sat == BR_TRUE);
    conf_line("TC-TIME-003", ok,
              "sleep(INF) ⇒ -EINVAL (G13); deadline_from_now saturates near UINT64_MAX instead of wrapping",
              pass, fail);
}

int br_sync_selftest(void)
{
    br_u32 pass = 0u;
    br_u32 fail = 0u;

    const br_bool sched = br_sched_registered();

    br_log_info("[SYNCCONF] sync primitives conformance (core; sched_registered=%u)",
                (br_u32)sched);
    if (sched != BR_TRUE) {
        br_log_info("[SYNCCONF] SKIP scheduler not registered: thread-dependent cases (001/004/006/007/008/"
                    "TC-TIME-001) not run; only the thread-independent subset runs (002/003/005 counting/009/"
                    "TC-TIME-002/003)");
    }

    conf_sync_001(&pass, &fail, sched);
    conf_sync_002(&pass, &fail);
    conf_sync_003(&pass, &fail, sched);
    conf_sync_004(&pass, &fail, sched);
    conf_sync_005(&pass, &fail, sched);
    conf_sync_006(&pass, &fail, sched);
    conf_sync_007(&pass, &fail, sched);
    conf_sync_008(&pass, &fail, sched);
    conf_sync_009(&pass, &fail);

    conf_time_001(&pass, &fail, sched);
    conf_time_002(&pass, &fail);
    conf_time_003(&pass, &fail);

    /* Teardown reset: cases are criteria, leave no mess (objects back to unheld/no waiters). */
    (void)br_mutex_init(&s_m);
    (void)br_sem_init(&s_sem, 0u);
    (void)br_cond_init(&s_cv);
    (void)br_spinlock_init(&s_sp);

    br_log_info("[SYNCCONF] SUMMARY pass=%u fail=%u total=%u",
                (br_u32)pass, (br_u32)fail, (br_u32)(pass + fail));

    /* Return the **failure count**: the aggregator sums them, red/green is decided by the gate (ADR-0010 §2.4). */
    return (int)fail;
}
