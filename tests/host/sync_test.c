/*
 * Host-side sync primitive semantics cases (TC-SYNC/TC-TIME). The hard part of mutex/sem/cond is
 * the blocking path, so a fake ucontext scheduler really runs br_sched_block_current() /
 * br_sched_wake() / timeout scanning (a test harness, not a second target implementation). The
 * fake scheduler follows the ICB handshake sync.c relies on: br_sched_wake() writes
 * wait_status=0 first, br_sched_block_current() then does not switch away, and timeouts apply only
 * to BLOCKED threads. Output: [SYNCCONF] PASS/FAIL/SKIP, [HOSTTEST] PASS/FAIL + SUMMARY.
 */
#include <br/core/br_error.h>
#include <br/core/br_fault.h>
#include <br/core/br_irq.h>
#include <br/core/br_sched.h>
#include <br/core/br_sync.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

#include <br/platform/br_plat.h>

#include "sched_internal.h"

/* Self-test suite entry (not in br_sync.h; the implementation lives in
 * core/c_core/selftest/sync_selftest.c, compiled in by the gates.toml sync-test sources). */
int br_sync_selftest(void);

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

/* ---- case framework ---- */

static int s_pass;
static int s_fail;

#define CHECK(cond, tag, desc)                                          \
    do {                                                                \
        if ((cond) != 0) {                                              \
            s_pass++;                                                   \
            printf("[HOSTTEST] PASS %s %s\n", (tag), (desc));           \
        } else {                                                        \
            s_fail++;                                                   \
            printf("[HOSTTEST] FAIL %s %s\n", (tag), (desc));           \
        }                                                               \
    } while (0)

static void section(const char *tag, const char *desc)
{
    printf("[HOSTTEST] ---- %s %s\n", tag, desc);
}

/* ---- fake platform clock (stand-in for time.c): 1 MHz => 1 tick == 1 us ---- */

#define FAKE_TICKS_HZ 1000000u

static br_u64 g_ticks;

br_u64 br_plat_ticks_freq(void)
{
    return FAKE_TICKS_HZ;
}

br_u64 br_plat_ticks_now(void)
{
    return g_ticks;
}

/* host stand-in for panic (only the spinlock path with "magic stomped" reaches here). */
BR_NORETURN void br_panic_bare(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    (void)fputs("[HOSTTEST] PANIC ", stderr);
    (void)vfprintf(stderr, fmt, ap);
    (void)fputc('\n', stderr);
    va_end(ap);
    abort();
}

/* Fake L2 interrupt lock: cooperative scheduling plus the real state encoding (I bit + depth)
 * keeps the spinlock pairing cases meaningful on the host. */

static br_u32 g_irq_depth;

br_irq_state_t br_irq_lock(void)
{
    br_irq_state_t st = (br_irq_state_t)(g_irq_depth << BR_IRQ_ST_DEPTH_SHIFT);

    if (g_irq_depth != 0u) {
        st |= BR_IRQ_ST_I_MASK;      /* already off ⇒ "I bit before entry = 1" */
    }
    g_irq_depth++;
    return st;
}

void br_irq_unlock(br_irq_state_t st)
{
    (void)st;
    if (g_irq_depth == 0u) {
        return;                      /* underflow guard (same discipline as the real implementation) */
    }
    g_irq_depth--;
}

/* ---- the four wait-chain primitives (per the contract in sched_internal.h) ---- */

void br_waitq_push(void **head, br_thread_t *t)
{
    if (head == NULL || t == NULL) {
        return;
    }
    t->wait_next = NULL;
    if (*head == NULL) {
        *head = t;
        return;
    }
    br_thread_t *p = (br_thread_t *)*head;
    while (p->wait_next != NULL) {
        p = p->wait_next;
    }
    p->wait_next = t;
}

br_thread_t *br_waitq_pop(void **head)
{
    if (head == NULL || *head == NULL) {
        return NULL;
    }
    br_thread_t *t = (br_thread_t *)*head;
    *head = t->wait_next;
    t->wait_next = NULL;
    return t;
}

int br_waitq_remove(void **head, br_thread_t *t)
{
    if (head == NULL || t == NULL) {
        return 0;
    }
    br_thread_t *prev = NULL;
    br_thread_t *p = (br_thread_t *)*head;

    while (p != NULL) {
        if (p == t) {
            if (prev == NULL) {
                *head = p->wait_next;
            } else {
                prev->wait_next = p->wait_next;
            }
            p->wait_next = NULL;
            return 1;
        }
        prev = p;
        p = p->wait_next;
    }
    return 0;
}

br_u32 br_waitq_len(const void *head)
{
    br_u32 n = 0u;
    const br_thread_t *p = (const br_thread_t *)head;

    while (p != NULL) {
        n++;
        p = p->wait_next;
    }
    return n;
}

void *br_thread_priv(br_thread_t *t)
{
    return (void *)((br_u8 *)t + sizeof(struct br_thread));
}

/* ---- fake scheduler (ucontext) ---- */

#define FAKE_MAX          12u
#define FAKE_STACK_BYTES  (128u * 1024u)
#define FAKE_RQ_CAP       (FAKE_MAX * 4u)

static struct br_thread g_tcb[FAKE_MAX];
static ucontext_t      g_uc[FAKE_MAX];
static ucontext_t      g_sched_uc;
static ucontext_t      g_boot_uc;
static br_u8           g_tstack[FAKE_MAX][FAKE_STACK_BYTES] BR_ALIGN(16);
static br_u8           g_sched_stack[64u * 1024u] BR_ALIGN(16);

static br_thread_t *g_current;
static br_thread_t *g_join_waiter[FAKE_MAX];
static br_thread_t *g_rq[FAKE_RQ_CAP];
static br_u32       g_rq_head;
static br_u32       g_rq_tail;
static br_u32       g_rq_len;

static br_u32 tcb_index(const br_thread_t *t)
{
    return (br_u32)(t - &g_tcb[0]);
}

static br_bool tcb_live(const struct br_thread *t)
{
    return (t->magic == BR_THREAD_MAGIC) ? BR_TRUE : BR_FALSE;
}

static void rq_push(br_thread_t *t)
{
    if (g_rq_len >= FAKE_RQ_CAP) {
        printf("[HOSTTEST] FAIL SCHED ready queue overflow\n");
        exit(2);
    }
    g_rq[g_rq_tail % FAKE_RQ_CAP] = t;
    g_rq_tail++;
    g_rq_len++;
}

static br_thread_t *rq_pop(void)
{
    if (g_rq_len == 0u) {
        return NULL;
    }
    br_thread_t *t = g_rq[g_rq_head % FAKE_RQ_CAP];
    g_rq_head++;
    g_rq_len--;
    return t;
}

static br_bool sched_all_done(void)
{
    for (br_u32 i = 0u; i < FAKE_MAX; i++) {
        if (tcb_live(&g_tcb[i]) == BR_TRUE && g_tcb[i].state != BR_TASK_ZOMBIE) {
            return BR_FALSE;
        }
    }
    return BR_TRUE;
}

/*
 * Timeout scan: with no ready thread, push the virtual clock to the nearest deadline and mark
 * every expired BLOCKED thread -ETIMEDOUT + READY. A thread already READY/woken is not timed out
 * again, so "timeout vs ownership handoff" cannot produce two winners.
 */
static br_bool advance_timeouts(void)
{
    br_time_t best = BR_TIMEOUT_INF;

    for (br_u32 i = 0u; i < FAKE_MAX; i++) {
        if (tcb_live(&g_tcb[i]) == BR_TRUE && g_tcb[i].state == BR_TASK_BLOCKED
            && g_tcb[i].wake_at != BR_TIMEOUT_INF && g_tcb[i].wake_at < best) {
            best = g_tcb[i].wake_at;
        }
    }
    if (best == BR_TIMEOUT_INF) {
        return BR_FALSE;
    }

    if (best > br_clock_now()) {
        g_ticks = best;                 /* 1 tick == 1 us */
    }
    const br_time_t now = br_clock_now();

    for (br_u32 i = 0u; i < FAKE_MAX; i++) {
        if (tcb_live(&g_tcb[i]) == BR_TRUE && g_tcb[i].state == BR_TASK_BLOCKED
            && g_tcb[i].wake_at != BR_TIMEOUT_INF && g_tcb[i].wake_at <= now) {
            g_tcb[i].wait_status = BR_ERR(ETIMEDOUT);
            g_tcb[i].state       = BR_TASK_READY;
            g_tcb[i].wake_at     = BR_TIMEOUT_INF;
            rq_push(&g_tcb[i]);
        }
    }
    return BR_TRUE;
}

static void fake_trampoline(void)
{
    br_thread_t *t = g_current;

    t->entry(t->arg);
    br_task_exit(0);
}

static void sched_loop(void)
{
    for (;;) {
        br_thread_t *next = rq_pop();

        if (next == NULL) {
            if (advance_timeouts() == BR_TRUE) {
                continue;
            }
            if (sched_all_done() == BR_TRUE) {
                (void)swapcontext(&g_sched_uc, &g_boot_uc);
                return;                                 /* unreachable */
            }
            printf("[HOSTTEST] FAIL SCHED deadlock: no ready thread and no deadline\n");
            exit(3);
        }

        g_current    = next;
        next->state  = BR_TASK_RUNNING;
        (void)swapcontext(&g_sched_uc, &g_uc[tcb_index(next)]);
    }
}

/* ---- Scheduler framework contract (same signatures as br_sched.h/sched_internal.h) ---- */

br_bool br_sched_registered(void)
{
    return BR_TRUE;
}

BR_NORETURN void br_sched_run(void)
{
    sched_loop();
    for (;;) {
    }
}

int br_sched_block_current(br_time_t abs_deadline)
{
    br_thread_t *t = g_current;

    if (t == NULL) {
        return BR_ERR(ENOTSUP);
    }
    if (t->wait_status == 0) {
        return 0;                    /* ICB: already woken after linking but before switching ⇒ do not switch */
    }

    t->wait_status = BR_ERR(ETIMEDOUT);
    t->state       = BR_TASK_BLOCKED;
    t->wake_at     = abs_deadline;
    (void)swapcontext(&g_uc[tcb_index(t)], &g_sched_uc);

    /* g_current was set back to this thread by the scheduler; re-read it rather than keep a local
     * across swapcontext (GCC treats it like setjmp). */
    return g_current->wait_status;
}

void br_sched_wake(br_thread_t *t)
{
    if (t == NULL) {
        return;
    }
    t->wait_status = 0;
    if (t->state == BR_TASK_BLOCKED) {
        t->state   = BR_TASK_READY;
        t->wake_at = BR_TIMEOUT_INF;
        rq_push(t);
    }
}

br_thread_t *br_task_self(void)
{
    return g_current;
}

br_u32 br_sched_task_count(void)
{
    br_u32 n = 0u;
    for (br_u32 i = 0u; i < FAKE_MAX; i++) {
        if (tcb_live(&g_tcb[i]) == BR_TRUE) {
            n++;
        }
    }
    return n;
}

br_u32 br_sched_ready_count(void)
{
    return g_rq_len;
}

br_u32 br_thread_pool_used(void)
{
    return br_sched_task_count();
}

/* Empty-TCB scan, noinline: GCC treats getcontext/makecontext like setjmp, and inlining would
 * trip -Wclobbered on the loop variable. */
#if defined(__GNUC__)
#  define HOST_NOINLINE __attribute__((noinline))
#else
#  define HOST_NOINLINE
#endif

static HOST_NOINLINE br_u32 tcb_alloc_slot(void)
{
    for (br_u32 i = 0u; i < FAKE_MAX; i++) {
        if (tcb_live(&g_tcb[i]) == BR_FALSE) {
            return i;
        }
    }
    return FAKE_MAX;
}

int br_task_create(br_thread_t **out, const br_task_attr_t *attr,
                   void (*entry)(void *), void *arg)
{
    if (out == NULL || attr == NULL || entry == NULL) {
        return BR_ERR(EINVAL);
    }

    const br_u32 slot = tcb_alloc_slot();
    if (slot == FAKE_MAX) {
        return BR_ERR(ENOMEM);
    }

    memset(&g_tcb[slot], 0, sizeof(g_tcb[slot]));
    g_tcb[slot].magic       = BR_THREAD_MAGIC;
    g_tcb[slot].state       = BR_TASK_READY;
    g_tcb[slot].prio        = attr->prio;
    g_tcb[slot].name        = attr->name;
    g_tcb[slot].stack_top   = (void *)g_tstack[slot];
    g_tcb[slot].stack_size  = FAKE_STACK_BYTES;
    g_tcb[slot].entry       = entry;
    g_tcb[slot].arg         = arg;
    g_tcb[slot].wake_at     = BR_TIMEOUT_INF;
    g_tcb[slot].wait_status = BR_ERR(ETIMEDOUT);
    g_join_waiter[slot]     = NULL;

    (void)getcontext(&g_uc[slot]);
    g_uc[slot].uc_stack.ss_sp   = g_tstack[slot];
    g_uc[slot].uc_stack.ss_size = FAKE_STACK_BYTES;
    g_uc[slot].uc_link          = NULL;
    makecontext(&g_uc[slot], fake_trampoline, 0);

    rq_push(&g_tcb[slot]);
    *out = &g_tcb[slot];
    return BR_OK;
}

BR_NORETURN void br_task_exit(int code)
{
    br_thread_t *t = g_current;

    if (t != NULL) {
        const br_u32 i = tcb_index(t);
        t->exit_code = code;
        t->state     = BR_TASK_ZOMBIE;
        t->wake_at   = BR_TIMEOUT_INF;
        if (g_join_waiter[i] != NULL) {
            br_sched_wake(g_join_waiter[i]);
            g_join_waiter[i] = NULL;
        }
    }
    (void)swapcontext(&g_uc[tcb_index(t)], &g_sched_uc);
    for (;;) {
    }
}

int br_task_join(br_thread_t *t, int *exit_code)
{
    if (t == NULL || tcb_live(t) == BR_FALSE || t == g_current) {
        return BR_ERR(EINVAL);
    }

    g_join_waiter[tcb_index(t)] = g_current;
    while (t->state != BR_TASK_ZOMBIE) {
        /* ICB: set wait_status to the pessimistic default before yielding, else a stale 0 from a
         * previous wake makes block_current never switch away (busy-wait deadlock). */
        g_current->wait_status = BR_ERR(ETIMEDOUT);
        (void)br_sched_block_current(BR_TIMEOUT_INF);
    }
    if (exit_code != NULL) {
        *exit_code = t->exit_code;
    }
    t->magic = 0u;                   /* reclaim the TCB */
    return BR_OK;
}

void br_task_yield(void)
{
    br_thread_t *t = g_current;

    if (t == NULL) {
        return;
    }
    t->state = BR_TASK_READY;
    rq_push(t);
    (void)swapcontext(&g_uc[tcb_index(t)], &g_sched_uc);
}

int br_task_sleep(br_time_t rel_us)
{
    if (rel_us == BR_TIMEOUT_INF) {
        return BR_ERR(EINVAL);    /* ruling G13 */
    }
    if (rel_us == 0u) {
        return BR_OK;
    }
    const int rc = br_sched_block_current(br_deadline_from_now(rel_us));
    return (rc == 0 || rc == BR_ERR(ETIMEDOUT)) ? BR_OK : rc;
}

int br_task_sleep_until(br_time_t abs_us)
{
    if (abs_us == BR_TIMEOUT_INF) {
        return BR_ERR(EINVAL);
    }
    if (abs_us <= br_clock_now()) {
        return BR_OK;
    }
    const int rc = br_sched_block_current(abs_us);
    return (rc == 0 || rc == BR_ERR(ETIMEDOUT)) ? BR_OK : rc;
}

const char *br_task_name(const br_thread_t *t)
{
    return (t != NULL && t->name != NULL) ? t->name : "?";
}

br_u32 br_task_state(const br_thread_t *t)
{
    return (t != NULL) ? t->state : BR_TASK_NEW;
}

/* ---- host-side supplementary cases ---- */

static br_mutex_t    s_def_m  = BR_MUTEX_INIT_VALUE;
static br_sem_t      s_def_s  = BR_SEM_INIT_VALUE(3u);
static br_cond_t     s_def_c  = BR_COND_INIT_VALUE;
static br_spinlock_t s_def_sp = BR_SPINLOCK_INIT_VALUE;

static void host_static_define(void)
{
    section("TC-SYNC-STATIC", "BR_*_DEFINE static objects vs dynamic init (CA-2 equivalence)");

    CHECK(s_def_m.magic == BR_SYNC_MAGIC_MUTEX, "TC-SYNC-STATIC", "static mutex magic in place");
    CHECK(br_mutex_lock(&s_def_m) == BR_OK, "TC-SYNC-STATIC", "static mutex can lock");
    CHECK(br_mutex_is_locked(&s_def_m) == BR_TRUE, "TC-SYNC-STATIC", "static mutex state changes");
    CHECK(br_mutex_unlock(&s_def_m) == BR_OK, "TC-SYNC-STATIC", "static mutex can unlock");

    CHECK(br_sem_count(&s_def_s) == 3u, "TC-SYNC-STATIC", "static sem initial value = macro argument");
    CHECK(br_sem_take(&s_def_s, BR_TIMEOUT_ZERO) == BR_OK, "TC-SYNC-STATIC", "static sem can take");
    CHECK(br_sem_give(&s_def_s) == BR_OK, "TC-SYNC-STATIC", "static sem can give");

    CHECK(br_cond_signal(&s_def_c) == BR_OK, "TC-SYNC-STATIC", "static cond signal works");

    CHECK(br_spinlock_trylock(&s_def_sp, &(br_irq_state_t){0u}) == BR_OK,
          "TC-SYNC-STATIC", "static spinlock can lock");
    CHECK(br_spinlock_is_locked(&s_def_sp) == BR_TRUE, "TC-SYNC-STATIC", "static spinlock state changes");
    br_spinlock_unlock(&s_def_sp, 0u);

    br_mutex_t dyn;
    CHECK(br_mutex_init(&dyn) == BR_OK, "TC-SYNC-STATIC", "dynamic mutex init");
    CHECK(br_mutex_lock(&dyn) == BR_OK, "TC-SYNC-STATIC", "dynamic mutex lock");
    CHECK(br_mutex_unlock(&dyn) == BR_OK, "TC-SYNC-STATIC", "dynamic mutex unlock");
    CHECK(br_mutex_is_locked(&dyn) == br_mutex_is_locked(&s_def_m),
          "TC-SYNC-STATIC", "static/dynamic objects behave alike (both unlocked after unlock)");
}

static void host_magic_corrupt(void)
{
    section("SYNC-MAGIC", "stomped magic (non-zero garbage) must still give -EINVAL, not silently 'unlocked'");

    br_mutex_t m;
    br_sem_t   s;
    br_cond_t  c;
    br_spinlock_t sp;
    memset(&m, 0xAB, sizeof(m));
    memset(&s, 0xAB, sizeof(s));
    memset(&c, 0xAB, sizeof(c));
    memset(&sp, 0xAB, sizeof(sp));

    CHECK(br_mutex_lock(&m) == BR_ERR(EINVAL), "SYNC-MAGIC", "garbage mutex ⇒ -EINVAL");
    CHECK(br_mutex_unlock(&m) == BR_ERR(EINVAL), "SYNC-MAGIC", "garbage mutex unlock ⇒ -EINVAL");
    CHECK(br_sem_take(&s, BR_TIMEOUT_ZERO) == BR_ERR(EINVAL), "SYNC-MAGIC", "garbage sem ⇒ -EINVAL");
    CHECK(br_sem_give(&s) == BR_ERR(EINVAL), "SYNC-MAGIC", "garbage sem give ⇒ -EINVAL");
    CHECK(br_cond_signal(&c) == BR_ERR(EINVAL), "SYNC-MAGIC", "garbage cond ⇒ -EINVAL");
    CHECK(br_spinlock_trylock(&sp, &(br_irq_state_t){0u}) == BR_ERR(EINVAL),
          "SYNC-MAGIC", "garbage spinlock trylock ⇒ -EINVAL");
    CHECK(br_mutex_is_locked(&m) == BR_FALSE, "SYNC-MAGIC", "garbage mutex query returns FALSE");
    CHECK(br_mutex_owner(&m) == NULL, "SYNC-MAGIC", "garbage mutex owner returns NULL");
}

static void host_sleep_resolution(void)
{
    section("TC-TIME-SLEEP", "sleep resolution under the host virtual clock (1 us/tick)");

    const br_time_t t0 = br_clock_now();
    const int rc1 = br_task_sleep(1000u);
    const br_time_t d1 = br_clock_now() - t0;

    const br_time_t t1 = br_clock_now();
    const int rc2 = br_task_sleep(0u);
    const br_time_t d2 = br_clock_now() - t1;

    printf("[HOSTTEST] sleep(1000us) -> rc=%d measured=%lu us; sleep(0) -> rc=%d measured=%lu us\n",
           rc1, (unsigned long)d1, rc2, (unsigned long)d2);

    CHECK(rc1 == BR_OK && d1 >= 1000u, "TC-TIME-SLEEP", "sleep(1000us) does not wake early");
    CHECK(d1 <= 1100u, "TC-TIME-SLEEP", "lateness is tiny under the virtual clock (resolution ~1 us)");
    CHECK(rc2 == BR_OK && d2 == 0u, "TC-TIME-SLEEP", "sleep(0) returns immediately, does not advance the clock");
    CHECK(br_task_sleep(BR_TIMEOUT_INF) == BR_ERR(EINVAL), "TC-TIME-SLEEP", "sleep(INF) -EINVAL(G13)");
}

/* ---- randomized stress ---- */

#define STRESS_WORKERS 3u
#define STRESS_OPS     50000u

static br_mutex_t s_str_m;
static br_sem_t   s_str_sem;
static volatile br_u32 s_str_guard;
static volatile br_u32 s_str_overlap;
static volatile br_u32 s_str_incs;
static br_u32 s_str_worker_incs[STRESS_WORKERS];
static br_u32 s_str_errs;
static br_u32 s_str_rng = 0x12345678u;

static br_u32 str_rng(void)
{
    s_str_rng = (s_str_rng * 1103515245u) + 12345u;
    return (s_str_rng >> 8);
}

static void stress_worker(void *arg)
{
    br_u32 *mine = (br_u32 *)arg;

    for (br_u32 i = 0u; i < STRESS_OPS; i++) {
        const br_u32 r = str_rng();

        switch (r % 4u) {
        case 0u:
        case 1u:
            if (br_mutex_lock(&s_str_m) != BR_OK) {
                s_str_errs++;
                return;
            }
            if (s_str_guard != 0u) {
                s_str_overlap++;
            }
            s_str_guard = 1u;
            (*mine)++;
            s_str_incs++;
            if ((r & 0x100u) != 0u) {
                br_task_yield();
            }
            s_str_guard = 0u;
            if (br_mutex_unlock(&s_str_m) != BR_OK) {
                s_str_errs++;
                return;
            }
            break;
        case 2u:
            if (br_sem_take(&s_str_sem, BR_TIMEOUT_ZERO) == BR_OK) {
                if (br_sem_give(&s_str_sem) != BR_OK) {
                    s_str_errs++;
                    return;
                }
            }
            break;
        default:
            br_task_yield();
            break;
        }
    }
}

static void host_stress(void)
{
    section("TC-SYNC-STRESS", "randomized stress: 3 threads x 50k iterations (mutex/sem/yield)");

    CHECK(br_mutex_init(&s_str_m) == BR_OK, "TC-SYNC-STRESS", "stress mutex init");
    CHECK(br_sem_init(&s_str_sem, STRESS_WORKERS) == BR_OK, "TC-SYNC-STRESS", "stress sem init(3)");

    s_str_guard = 0u;
    s_str_overlap = 0u;
    s_str_incs = 0u;
    s_str_errs = 0u;
    for (br_u32 i = 0u; i < STRESS_WORKERS; i++) {
        s_str_worker_incs[i] = 0u;
    }

    br_thread_t *ts[STRESS_WORKERS];
    for (br_u32 i = 0u; i < STRESS_WORKERS; i++) {
        ts[i] = NULL;
        br_task_attr_t attr;
        attr.name       = "stress";
        attr.stack      = (void *)g_tstack[0];
        attr.stack_size = FAKE_STACK_BYTES;
        attr.prio       = 0u;
        attr.flags      = 0u;
        (void)br_task_create(&ts[i], &attr, stress_worker, &s_str_worker_incs[i]);
    }

    br_u32 joined = 0u;
    for (br_u32 i = 0u; i < STRESS_WORKERS; i++) {
        int code = 0;
        if (br_task_join(ts[i], &code) == BR_OK) {
            joined++;
        }
    }

    br_u32 expect = 0u;
    for (br_u32 i = 0u; i < STRESS_WORKERS; i++) {
        expect += s_str_worker_incs[i];
    }

    printf("[HOSTTEST] stress: joined=%u incs=%lu expect=%lu overlap=%lu errs=%lu sem_count=%lu\n",
           joined, (unsigned long)s_str_incs, (unsigned long)expect,
           (unsigned long)s_str_overlap, (unsigned long)s_str_errs,
           (unsigned long)br_sem_count(&s_str_sem));

    CHECK(joined == STRESS_WORKERS, "TC-SYNC-STRESS", "all 3 stress threads joined successfully");
    CHECK(s_str_errs == 0u, "TC-SYNC-STRESS", "no primitive error returns during stress");
    CHECK(s_str_overlap == 0u, "TC-SYNC-STRESS", "no critical-section overlap during stress (mutual exclusion holds)");
    CHECK(s_str_incs == expect, "TC-SYNC-STRESS", "stress counts are exact (nothing lost)");
    CHECK(br_mutex_is_locked(&s_str_m) == BR_FALSE, "TC-SYNC-STRESS", "mutex not held after stress");
    CHECK(br_sem_count(&s_str_sem) == STRESS_WORKERS, "TC-SYNC-STRESS", "sem count back to its initial value after stress");
    CHECK(br_waitq_len(s_str_m.head) == 0u && s_str_m.waiters == 0u,
          "TC-SYNC-STRESS", "mutex wait chain empty after stress (no ghost nodes)");
    CHECK(br_waitq_len(s_str_sem.head) == 0u && s_str_sem.waiters == 0u,
          "TC-SYNC-STRESS", "sem wait chain empty after stress (no ghost nodes)");
}

/* ---- main flow (runs in the fake scheduler's main thread) ---- */

static void host_body(void)
{
    /* The same self-test suite as on target: it really creates and joins threads and the blocking
     * path really runs (return value = failure count; red/green from the logs). */
    br_sync_selftest();

    host_static_define();
    host_magic_corrupt();
    host_sleep_resolution();
    host_stress();

    /* global wrap-up: no case may leave a waiter behind */
    printf("[HOSTTEST] final: task_count=%lu ready=%lu\n",
           (unsigned long)br_sched_task_count(), (unsigned long)br_sched_ready_count());
}

static void host_main_trampoline(void)
{
    host_body();
    br_task_exit(0);
}

int main(void)
{
    (void)setvbuf(stdout, NULL, _IONBF, 0u);   /* if a case hangs, we must still see which line it hung on */
    printf("[HOSTTEST] brickOS prototype — host-side sync primitive gate (ucontext fake scheduler, blocking path really runs)\n");
    (void)br_clock_init();

    (void)getcontext(&g_sched_uc);
    g_sched_uc.uc_stack.ss_sp   = g_sched_stack;
    g_sched_uc.uc_stack.ss_size = sizeof(g_sched_stack);
    g_sched_uc.uc_link          = NULL;
    makecontext(&g_sched_uc, sched_loop, 0);

    memset(&g_tcb[0], 0, sizeof(g_tcb[0]));
    g_tcb[0].magic       = BR_THREAD_MAGIC;
    g_tcb[0].state       = BR_TASK_READY;
    g_tcb[0].name        = "host-main";
    g_tcb[0].wake_at     = BR_TIMEOUT_INF;
    g_tcb[0].wait_status = BR_ERR(ETIMEDOUT);
    (void)getcontext(&g_uc[0]);
    g_uc[0].uc_stack.ss_sp   = g_tstack[0];
    g_uc[0].uc_stack.ss_size = FAKE_STACK_BYTES;
    g_uc[0].uc_link          = NULL;
    makecontext(&g_uc[0], host_main_trampoline, 0);
    rq_push(&g_tcb[0]);

    /* Switch to the scheduler, which runs the main thread and returns here when all are done. */
    (void)swapcontext(&g_boot_uc, &g_sched_uc);

    printf("[HOSTTEST] SUMMARY pass=%d fail=%d total=%d\n", s_pass, s_fail, s_pass + s_fail);
    return (s_fail == 0) ? 0 : 1;
}
