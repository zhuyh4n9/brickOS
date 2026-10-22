/*
 * Host-side scheduler test harness: the target symbols the host must define itself (clock, IRQ
 * lock, log/panic, switch layer) plus the host-only idle hook br_sched_host_wait().
 */
#include "host_sched.h"

#include <br/core/br_error.h>
#include <br/hal/br_fault.h>
#include <br/hal/br_irq.h>
#include <br/core/br_log.h>
#include <br/hal/br_sched.h>
#include <br/core/br_time.h>
#include <br/hal/br_work.h>

/* TCB common header and switch-layer internals (core-private surface). */
#include "sched_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

/* ---- fake clock + idle hook ---- */

static br_time_t s_fake_now;
static br_u32    s_idle_calls;

/* Clock step per read: must stay well under 1 ms (TC-TIME-002: two reads < 1 ms). */
#define HOST_CLOCK_STEP_US 50u
/* Host case's own virtual tick, decoupled from product [kernel].hz (ticks are counted). */
#define HOST_TICK_US       100000u

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
     * Auto tick requires: switch on, not already delivering, depth 0 (L2). Inside L2 time still
     * advances, and the accumulated amount is repaid as one tick after release.
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

/* Called by core on idle entry (weak symbol in sched_core.c): advance 100 us and tick once. */
void br_sched_host_wait(void);
void br_sched_host_wait(void)
{
    s_idle_calls++;
    s_fake_now += 100u;
    br_sched_on_tick(s_fake_now);
}

/*
 * L2 interrupt lock (host stand-in). Depth must be real: br_irq_in_atomic() and the new-thread
 * interrupt-shape fix-up read it, so a fake 0 would make those criteria vacuously true.
 */

static br_u32 s_irq_depth;

br_irq_state_t br_irq_lock(void)
{
    br_irq_state_t st = (br_irq_state_t)(s_irq_depth << BR_IRQ_ST_DEPTH_SHIFT);

    if (s_irq_depth != 0u) {
        st |= BR_IRQ_ST_I_MASK;          /* already masked => "I bit before entry = 1" */
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

/* Host has no ISR (single-threaded, cooperative) => always false; bh is set in work_core.c. */
br_bool br_irq_in_isr(void)
{
    return BR_FALSE;
}

br_bool br_irq_in_atomic(void)
{
    return (br_work_in_bh() != BR_FALSE) ? BR_TRUE : BR_FALSE;
}

/* Called by `br_sched_thread_entry` when a new thread first enters (target: release PSTATE.I). */
void br_irq_cpu_enable(void)
{
}

/* ---- log / panic stubs ---- */

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

/* ---- host switch layer (ucontext) ---- */

#define HOST_SLOT_MAX 32u

/*
 * The TCB pool reuses addresses, so caching ucontext by pointer would attach a new thread to the
 * previous tenant's stale context. Instead a ctx[] slot core does not use on the host path (x20)
 * carries an "already materialized" marker; create zeroes it, materialization writes it.
 */
/* Magic in the borrowed ctx[] slot: "HOST". */
#define HOST_CTX_READY_MAGIC ((br_u64)0x484F5354u)

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

/* Entry of a new thread on ucontext; core already set current to this thread. */
static void host_thread_tramp(void)
{
    br_sched_thread_entry(br_task_self());
}

void br_sched_switch_to(br_thread_t *from, br_thread_t *to)
{
    host_slot_t *sf = slot_of(from);
    host_slot_t *st = slot_of(to);

    /* First entry only (stack set, marker unset); a reused TCB has 0 there, so it is rebuilt. */
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

/* Provided only for linking; the host entry is host_thread_tramp. */
void br_sched_thread_trampoline(void)
{
    fputs("[HOSTPANIC] host br_sched_thread_trampoline must not be called\n", stderr);
    abort();
}

/* ---- entry ---- */

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
