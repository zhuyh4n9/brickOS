/*
 * tests/host/host_sched.c — host-side scheduler test harness (implementation; see host_sched.h)
 *
 * Symbols referenced by core on target that the host must supply itself:
 *   br_clock_now / br_deadline_from_now / br_irq_lock / br_irq_unlock /
 *   br_irq_lock_depth / br_irq_in_isr / br_irq_in_atomic / br_irq_cpu_enable /
 *   br_log_write / br_panic / br_sched_switch_to / br_sched_thread_trampoline
 * plus one host-only idle hook `br_sched_host_wait()` (a weak symbol in sched_core.c):
 * the host has no real interrupt source, so it advances the fake clock and delivers one
 * `br_sched_on_tick()` while core is idle, making "timeout scan on periodic tick"
 * testable on the host.
 *
 * The ucontext slots and the "context already materialized" marker are identical to
 * before (see comments in the file): the TCB pool reuses addresses, and caching
 * ucontext by pointer would attach a new thread to the previous tenant's stale context.
 */
#include "host_sched.h"

#include <br/core/br_error.h>
#include <br/core/br_fault.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_work.h>

/* TCB common header and switch-layer internal prototypes (core-private surface; the host
 * is "the second leg of the same semantics", included as tests/host/sched_test.c did). */
#include "sched_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

/* =====================================================================
 * fake clock + idle hook
 * ===================================================================== */

static br_time_t s_fake_now;
static br_u32    s_idle_calls;

/* Auto tick source (see host_sched.h): off by default ⇒ existing cases' clock semantics are unchanged. */
#define HOST_CLOCK_STEP_US 50u       /* "real time" advanced per clock read (must be well under 1 ms: TC-TIME-002's criterion is "two reads < 1 ms") */
#define HOST_TICK_US       100000u   /* the host case's own virtual tick; decoupled from product [kernel].hz (the criterion counts ticks) */

static br_bool   s_auto_tick;
static br_time_t s_since_tick;
static int       s_in_clock;         /* re-entrancy guard: the tick delivery path reads the clock too */

br_time_t host_sched_now(void) { return s_fake_now; }
void      host_sched_advance_us(br_time_t us) { s_fake_now += us; }
br_u32    host_sched_idle_calls(void) { return s_idle_calls; }

void host_sched_tick(void) { br_sched_on_tick(s_fake_now); }

void host_sched_auto_tick(br_bool on)
{
    s_auto_tick = on;
    s_since_tick = 0u;
}

br_time_t br_clock_now(void)
{
    /*
     * ★ Auto tick (off by default): see the long comment in host_sched.h. All three
     *   conditions are required —— the switch is on, we are not already delivering
     *   (re-entrancy guard), and we are NOT in an L2 critical section (= interrupts
     *   masked on target). Inside L2 time still advances but no tick is delivered: the
     *   accumulated amount is repaid as one tick on the first clock read after release.
     */
    if ((s_auto_tick != BR_FALSE) && (s_in_clock == 0)) {
        s_in_clock = 1;
        s_fake_now   += HOST_CLOCK_STEP_US;
        s_since_tick += HOST_CLOCK_STEP_US;

        if ((s_since_tick >= HOST_TICK_US) && (br_irq_lock_depth() == 0u)) {
            s_since_tick = 0u;
            br_sched_on_tick(s_fake_now);
            br_sched_irq_epilogue();
        }

        s_in_clock = 0;
    }
    return s_fake_now;
}

br_time_t br_deadline_from_now(br_time_t rel_us)
{
    if (rel_us > (BR_TIMEOUT_INF - s_fake_now)) {
        return BR_TIMEOUT_INF;          /* saturate, do not wrap into "timed out immediately" */
    }
    return s_fake_now + rel_us;
}

/* Called by core when entering idle (weak symbol in sched_core.c): advance 100 us and deliver one tick. */
void br_sched_host_wait(void);
void br_sched_host_wait(void)
{
    s_idle_calls++;
    s_fake_now += 100u;
    br_sched_on_tick(s_fake_now);
}

/* =====================================================================
 * L2 interrupt lock (host stand-in)
 *
 * The host has no maskable interrupts; but scheduling only switches stacks at explicit
 * points or at IRQ exit, so "no preemption inside an L2 critical section" is expressible
 * on the host via depth as well. depth must be real: ADR-0011 ruling P-1 (fix the
 * interrupt shape by depth when a new thread first enters) and bh's
 * `br_irq_in_atomic()` both read it —— returning a fake 0 would make those two criteria
 * vacuously true.
 * ===================================================================== */

static br_u32 s_irq_depth;

br_irq_state_t br_irq_lock(void)
{
    br_irq_state_t st = (br_irq_state_t)(s_irq_depth << BR_IRQ_ST_DEPTH_SHIFT);

    if (s_irq_depth != 0u) {
        st |= BR_IRQ_ST_I_MASK;          /* already masked ⇒ "I bit before entry = 1" */
    }
    s_irq_depth++;
    return st;
}

void br_irq_unlock(br_irq_state_t st)
{
    (void)st;
    if (s_irq_depth == 0u) {
        return;                          /* underflow guard (same discipline as the real implementation) */
    }
    s_irq_depth--;
}

br_u32 br_irq_lock_depth(void)
{
    return s_irq_depth;
}

/* The host has no ISR (tests are single-threaded and cooperative) ⇒ always false. bh is answered by work_core.c's real value. */
br_bool br_irq_in_isr(void)
{
    return BR_FALSE;
}

br_bool br_irq_in_atomic(void)
{
    return (br_work_in_bh() != BR_FALSE) ? BR_TRUE : BR_FALSE;
}

/* Called by `br_sched_thread_entry` when a new thread first enters (on target = release PSTATE.I). The host has no interrupts. */
void br_irq_cpu_enable(void)
{
}

/* =====================================================================
 * log / panic stubs
 * ===================================================================== */

static const char *s_watch;
static br_u32      s_watch_hits;

void host_sched_watch(const char *needle)
{
    s_watch = needle;
    s_watch_hits = 0u;
}

br_u32 host_sched_watch_hits(void) { return s_watch_hits; }

void br_log_write(br_log_level_t level, const char *fmt, ...)
{
    static const char *const tag[BR_LOG_LEVEL_COUNT] = { "DEBUG", "INFO ", "WARN ", "ERROR" };
    char    buf[512];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    const char *t = ((unsigned)level < (unsigned)BR_LOG_LEVEL_COUNT) ? tag[level] : "?";
    printf("[HOST %s] %s\n", t, buf);

    if (s_watch != NULL && strstr(buf, s_watch) != NULL) {
        s_watch_hits++;
    }
}

BR_NORETURN void br_panic(const char *fmt, ...)
{
    va_list ap;

    fputs("[HOSTPANIC] ", stderr);
    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(3);
}

/* =====================================================================
 * host switch layer (ucontext)
 * ===================================================================== */

#define HOST_SLOT_MAX 32u

/*
 * The TCB pool REUSES addresses: after a thread is joined and reclaimed, the next create
 * very likely gets the same `struct br_thread *`. If the host cached ucontext by pointer,
 * it would attach the "new thread" to the previous tenant's stale context (manifesting as
 * random wandering/hangs). So we borrow a ctx[] slot core does NOT use on the host path
 * (x20) as an "already materialized" marker: a new/reused TCB has 0 there (create zeroes
 * the whole slot), and after materializing once we write the magic ⇒ on reuse it is
 * correctly rebuilt.
 */
#define HOST_CTX_READY_MAGIC ((br_u64)0x484F5354u)   /* "HOST" */

typedef struct host_slot {
    br_thread_t *t;
    ucontext_t   uc;
} host_slot_t;

static host_slot_t s_slots[HOST_SLOT_MAX];

static host_slot_t *slot_of(br_thread_t *t) __attribute__((noinline));

static host_slot_t *slot_of(br_thread_t *t)
{
    host_slot_t *free_slot = NULL;

    for (br_u32 i = 0u; i < HOST_SLOT_MAX; i++) {
        if (s_slots[i].t == t) {
            return &s_slots[i];
        }
        if (s_slots[i].t == BR_NULL && free_slot == NULL) {
            free_slot = &s_slots[i];
        }
    }
    if (free_slot == NULL) {
        fputs("[HOSTPANIC] ucontext slot pool full\n", stderr);
        exit(3);
    }
    free_slot->t = t;
    return free_slot;
}

/* Entry of a new thread on ucontext. core already set current to this thread before switching stacks, so it can fetch itself here. */
static void host_thread_tramp(void)
{
    br_sched_thread_entry(br_task_self());
}

void br_sched_switch_to(br_thread_t *from, br_thread_t *to)
{
    host_slot_t *sf = slot_of(from);
    host_slot_t *st = slot_of(to);

    /*
     * First-entry conditions: this TCB has a stack (excluding main —— its context is saved
     * by the first swapcontext), and the "already materialized" marker has not been written.
     * For a reused TCB the marker is 0 ⇒ rebuild.
     */
    if ((to->stack_top != BR_NULL) && (to->ctx[1] != HOST_CTX_READY_MAGIC)) {
        getcontext(&st->uc);
        st->uc.uc_stack.ss_sp   = (void *)((br_u8 *)to->stack_top - to->stack_size);
        st->uc.uc_stack.ss_size = (size_t)to->stack_size;
        st->uc.uc_link          = NULL;
        makecontext(&st->uc, host_thread_tramp, 0);
        to->ctx[1] = HOST_CTX_READY_MAGIC;
    }

    (void)swapcontext(&sf->uc, &st->uc);
}

/* The host needs no real trampoline (the entry is host_thread_tramp); provided only for linking. */
void br_sched_thread_trampoline(void)
{
    fputs("[HOSTPANIC] host br_sched_thread_trampoline must not be called\n", stderr);
    abort();
}

/* =====================================================================
 * entry
 * ===================================================================== */

#define HOST_SUP_STACK_BYTES 16384u
static br_u8 s_sup_stack[HOST_SUP_STACK_BYTES] BR_ALIGN(16);

void host_sched_main(void (*supervisor)(void *))
{
    printf("[HOSTTEST] brickOS prototype — host-side scheduler test harness\n");

    if (br_sched_registered() != BR_TRUE) {
        printf("[HOSTTEST] FAIL scheduler not registered (call the plugin's *_early_init first)\n");
        exit(1);
    }

    /* I1: after register, the caller's boot context is the main thread */
    const char *mn = br_task_name(br_task_self());
    if (mn == BR_NULL || strcmp(mn, "main") != 0) {
        printf("[HOSTTEST] FAIL boot context not materialized as main (got '%s')\n",
               (mn != BR_NULL) ? mn : "(null)");
        exit(1);
    }
    printf("[HOSTTEST] after register br_task_self()=%s (I1 holds)\n", mn);

    const br_task_attr_t a = {
        .name       = "supervisor",
        .stack      = (void *)(s_sup_stack + HOST_SUP_STACK_BYTES),
        .stack_size = HOST_SUP_STACK_BYTES,
        .prio       = 0u,
        .flags      = 0u,
    };
    br_thread_t *sup = BR_NULL;
    const int rc = br_task_create(&sup, &a, supervisor, BR_NULL);
    if (rc != 0) {
        printf("[HOSTTEST] FAIL create supervisor rc=%d\n", rc);
        exit(1);
    }

    printf("[HOSTTEST] entering br_sched_run() (current=main, ready=supervisor)\n");
    br_sched_run();                     /* does not return: exit() in supervisor */
}
