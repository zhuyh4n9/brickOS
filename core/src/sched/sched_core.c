/*
 * brickOS prototype v0.2.0 — scheduler framework (core): thread state machine + timeout wakeup + idle + first schedule
 *
 * Design basis: `3-03-sched.md` (scheduler overview), `3-01-core-api-list.md` §2/§5.1/§14 (CA-1/CA-4),
 *           `3-02-int.md` §11.1–§11.3 (IRQ ↔ scheduler seam; the L2 critical-section idle recipe is at the end of §11.1),
 *           `1-01-architecture.md` §9 (boot sequence), `6-01-test.md` §3.1/§3.3.
 * Per-item decisions and design-gap rulings: `docs/decisions/0006-scheduler-framework.md` (this file implements it).
 *
 * ## Split
 *   core   : context-switch asm (`switch.S`) / TCB common header (`sched_internal.h`) / stack + trampoline /
 *            timeout wakeup framework / idle / **thread state machine** / conformance
 *   plugin : ready structure / `pick_next` / tick policy / preemption decision (`sched/coop` = FIFO + switch only at explicit points)
 *
 * ## Key invariants (ADR-0006 §1; each has a mechanical check)
 *   I1  `br_sched_register()` materializes the **caller's boot context** as the main thread
 *       (RUNNING, name "main"). Before register `br_task_self() == BR_NULL`, afterwards always == main.
 *       ⇒ `br_task_create/join` already work **before** `br_sched_run()` (APP's start() phase);
 *          `br_sched_run()` is only the entry to "enter idle / first schedule", not a prerequisite for the scheduler.
 *   I2  Only core writes `state`. Plugins only change "ready-structure membership": READY ⇄ queue;
 *       RUNNING ⇒ not queued; `pick_next` only dequeues, core sets RUNNING.
 *   I3  `br_sched_wake(t)` first writes `t->wait_status = 0`, and only requeues when `state == BLOCKED`
 *       (otherwise it writes only the handshake bit). The timeout scan touches only threads with `state == BLOCKED` that are due.
 *   I4  **Switches happen only at these explicit points**: yield / block / exit / first schedule, **plus** —
 *       after ADR-0011 — **preemption at the IRQ exit** (only when the scheduler set need_resched).
 *       COOP never sets it ⇒ for coop the old wording of I4 (no preemption, no stack switch at the IRQ exit) holds verbatim.
 *       The IRQ-exit switch point is at the C call boundary (`br_sched_switch_to`); the preempted thread's exception frame
 *       stays intact on its own stack — see the long comment in `resched_from_irq()`.
 *   I5  ICB handshake (ADR-0006 §3b, aligned with F3's sync.c): `wait_status` is the only handshake bit —
 *       the caller pre-sets -ETIMEDOUT before opening the window, wake sets 0, timeout sets -ETIMEDOUT;
 *       whoever writes first wins, both orders converge to the same result (see `br_sched_block_current`).
 *   I6  **bh is not a thread context** (ADR-0011): the bottom half runs on the IRQ-disabled exception exit; blocking/
 *       yielding/exiting inside it loses the exception frame's return path ⇒ all three paths are rejected at runtime
 *       (`br_sched_block_current` = -EPERM, `br_task_yield` = reject + trace,
 *        `br_task_exit` = panic).
 */
#include <br/core/br_error.h>
#include <br/core/br_fault.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_trace.h>
#include <br/core/br_types.h>
#include <br/core/br_work.h>

#include "sched_ctx.h"
#include "sched_internal.h"

/* =====================================================================
 * TCB pool (static; prototype rule "no dynamic allocation", ADR-0006 §2 ruling S-3)
 *
 * Slot = common header + plugin private tail. The plugin reports the total size in `ops.tcb_size`; the pool is
 * sized by the **compile-time bound** `BR_TCB_SLOT_BYTES` and register checks `tcb_size <= BR_TCB_SLOT_BYTES` --
 * exceeding it means the plugin and the core pool config disagree, so register fails loudly (no silent truncation).
 * ===================================================================== */
#define BR_TCB_SLOT_BYTES 256u

typedef union br_tcb_slot {
    struct br_thread t;
    br_u64           align;
    br_u8            raw[BR_TCB_SLOT_BYTES];
} br_tcb_slot_t;

_Static_assert(sizeof(struct br_thread) <= BR_TCB_SLOT_BYTES,
               "BR_TCB_SLOT_BYTES cannot hold the TCB common header -- raise the pool bound in sched_core.c");
_Static_assert(__builtin_offsetof(struct br_thread, ctx) == BR_THREAD_CTX_OFF,
               "ctx offset drifted from switch.S / sched_ctx.h (stack switch would hit the wrong words)");

static br_tcb_slot_t        s_pool[BR_TASK_MAX];
static br_thread_t         *s_all;        /* global TCB chain (walk/observe/timeout scan) */
static br_thread_t         *s_current;    /* current thread (single core; TPIDR_EL1 is the v2/SMP landing spot) */
static const br_sched_ops_t *s_ops;
static br_thread_t         *s_joiner[BR_TASK_MAX];  /* target -> its joiner (one at a time) */

/*
 * Preemption request bit (ADR-0011). Written by the scheduler plugin's tick policy (`br_sched_request_resched`,
 * inside the ISR); consumed at the IRQ exit by `resched_from_irq()`. **This is the only bit**
 * (`volatile` is for the "ISR writes / exit reads" pair; on a single core it needs no atomic instruction).
 */
static volatile br_bool s_need_resched;

/*
 * Host-side idle wait hook (weak symbol). The target has no such symbol: idle simply issues `wfi`
 * (3-02 §11.1 recipe: disable IRQs -> check nothing ready -> WFI -> enable IRQs).
 * The host has no real interrupt source and no WFI -- the test advances a fake clock and calls
 * `br_sched_on_tick()` here, making "timeout scan on periodic tick" testable on the host (see tests/host/sched_test.c).
 */
#if !defined(__aarch64__)
void br_sched_host_wait(void) __attribute__((weak));
#endif

/* =====================================================================
 * Small helpers
 * ===================================================================== */

br_size_t br_sched_tcb_size(void)
{
    return (br_size_t)sizeof(struct br_thread);
}

void *br_thread_priv(br_thread_t *t)
{
    return (void *)((br_u8 *)t + sizeof(struct br_thread));
}

/* =====================================================================
 * Four wait-chain primitives (zero allocation: the node is `wait_next` in the TCB; sync primitives attach/detach waiters here)
 *
 * Why in core rather than in each sync object: the node layout belongs to the frozen TCB (`sched_internal.h`),
 * and `br_sched_block_current()`'s return path requires "unchain" and "handshake bit" to work together.
 * These four do not depend on scheduler state (usable with no READY thread), so the sync path is always available.
 * ===================================================================== */

void br_waitq_push(void **head, br_thread_t *t)
{
    if (head == BR_NULL || t == BR_NULL) {
        return;
    }
    t->wait_next = BR_NULL;
    if (*head == BR_NULL) {
        *head = t;
        return;
    }
    br_thread_t *p = (br_thread_t *)*head;
    while (p->wait_next != BR_NULL) {
        p = p->wait_next;
    }
    p->wait_next = t;
}

br_thread_t *br_waitq_pop(void **head)
{
    if (head == BR_NULL || *head == BR_NULL) {
        return BR_NULL;
    }
    br_thread_t *t = (br_thread_t *)*head;

    *head = t->wait_next;
    t->wait_next = BR_NULL;
    return t;
}

int br_waitq_remove(void **head, br_thread_t *t)   /* 1 = removed, 0 = not on the chain */
{
    if (head == BR_NULL || t == BR_NULL) {
        return 0;
    }
    br_thread_t *prev = BR_NULL;
    br_thread_t *p = (br_thread_t *)*head;

    while (p != BR_NULL) {
        if (p == t) {
            if (prev == BR_NULL) {
                *head = p->wait_next;
            } else {
                prev->wait_next = p->wait_next;
            }
            p->wait_next = BR_NULL;
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

    for (const br_thread_t *p = (const br_thread_t *)head; p != BR_NULL; p = p->wait_next) {
        n++;
    }
    return n;
}

/* TCB pointer -> pool index (not in the pool ⇒ BR_TASK_MAX). */
static br_u32 tcb_index(const br_thread_t *t)
{
    const br_uintptr_t base = (br_uintptr_t)(const void *)&s_pool[0];
    const br_uintptr_t p    = (br_uintptr_t)(const void *)t;

    if (p < base || p >= (base + (br_uintptr_t)sizeof(s_pool))) {
        return BR_TASK_MAX;
    }
    return (br_u32)((p - base) / (br_uintptr_t)BR_TCB_SLOT_BYTES);
}

/* CPU wait (target = WFI; host = weak hook). Caller must have IRQs disabled (L2). */
static void cpu_wait(void)
{
#if defined(__aarch64__)
    __asm__ volatile("wfi");
#else
    if (br_sched_host_wait != BR_NULL) {
        br_sched_host_wait();
    }
#endif
}

static br_u32 ready_count_now(void)
{
    br_u32 n = 0u;

    for (const br_thread_t *t = s_all; t != BR_NULL; t = t->all_next) {
        if (t->state == BR_TASK_READY) {
            n++;
        }
    }
    return n;
}

/* Clear the slot (whole slot, including the private tail -- no stale queue pointers on reuse). */
static void tcb_clear(br_thread_t *t)
{
    br_u8 *raw = (br_u8 *)(void *)t;

    for (br_size_t i = 0u; i < BR_TCB_SLOT_BYTES; i++) {
        raw[i] = 0u;
    }
}

static br_thread_t *tcb_alloc(void)
{
    for (br_u32 i = 0u; i < BR_TASK_MAX; i++) {
        if (s_pool[i].t.magic != BR_THREAD_MAGIC) {
            br_thread_t *t = &s_pool[i].t;

            tcb_clear(t);
            t->magic   = BR_THREAD_MAGIC;
            t->state   = BR_TASK_NEW;
            t->wake_at = BR_TIMEOUT_INF;

            const br_irq_state_t st = br_irq_lock();
            t->all_next = s_all;
            s_all = t;
            br_irq_unlock(st);
            return t;
        }
    }
    return BR_NULL;
}

static void tcb_free(br_thread_t *t)
{
    const br_u32 i = tcb_index(t);

    if (i < BR_TASK_MAX) {
        s_joiner[i] = BR_NULL;
    }

    const br_irq_state_t st = br_irq_lock();
    br_thread_t **pp = &s_all;
    while (*pp != BR_NULL) {
        if (*pp == t) {
            *pp = t->all_next;
            break;
        }
        pp = &(*pp)->all_next;
    }
    tcb_clear(t);              /* magic zeroed = slot free */
    br_irq_unlock(st);
}

/* =====================================================================
 * Wakeup / timeout (ICB handshake, I3/I5)
 * ===================================================================== */

/* Put t back into the ready structure (core writes state; the plugin only enqueues). Caller holds L2 or atomicity is not required. */
static void make_ready(br_thread_t *t)
{
    t->state = BR_TASK_READY;
    if (s_ops != BR_NULL && s_ops->thread_ready != BR_NULL) {
        (void)s_ops->thread_ready(t);
    }
}

void br_sched_wake(br_thread_t *t)
{
    if (t == BR_NULL || t->magic != BR_THREAD_MAGIC || s_ops == BR_NULL) {
        return;
    }

    const br_irq_state_t st = br_irq_lock();

    /*
     * ICB step 3: write the handshake bit **first**. If t is still RUNNING (the caller is
     * "after opening the window, before actually BLOCKED"), this is an "early wake inside the window" -- write the
     * bit only, do not enqueue; `br_sched_block_current` sees 0 at its entry and returns without switching stacks.
     */
    t->wait_status = 0;

    if (t->state == BR_TASK_BLOCKED) {
        t->wake_at = BR_TIMEOUT_INF;   /* stop tracking the deadline */
        make_ready(t);
    } else {
        /* Already READY / RUNNING / ZOMBIE: hand over the handshake bit only (idempotent) */
    }

    br_irq_unlock(st);
}

/* Timeout expiry: only for BLOCKED threads that are due (ICB step 4). */
static void make_timed_out(br_thread_t *t)
{
    t->wait_status = BR_ERR(ETIMEDOUT);
    t->wake_at = BR_TIMEOUT_INF;
    make_ready(t);
}

static void scan_timeouts(br_time_t now)
{
    if (s_ops == BR_NULL) {
        return;
    }

    for (br_thread_t *t = s_all; t != BR_NULL; t = t->all_next) {
        if (t->state != BR_TASK_BLOCKED) {
            continue;                  /* already READY / already woken must not time out again */
        }
        if (t->wake_at == BR_TIMEOUT_INF || t->wake_at > now) {
            continue;
        }
        make_timed_out(t);
    }
}

/* =====================================================================
 * Switch: pick_next -> stack switch; nothing ready -> idle
 * ===================================================================== */

static void idle_wait(void)
{
    if (s_ops->idle != BR_NULL) {
        s_ops->idle();
        return;
    }

    /*
     * 3-02 §11.1 recipe (verbatim): **disable IRQs -> check "no ready work" -> WFI -> enable IRQs**.
     * The check and the WFI must not be interrupted: if an interrupt arrives after the check and before the WFI
     * and wakes a thread, that WFI sleeps until the next interrupt (possibly a long time) -- the classic idle lost wakeup.
     * WFI returns immediately when an interrupt is pending (even with PSTATE.I == 1), so disabling IRQs here does
     * not stop it from being woken by an interrupt.
     */
    const br_irq_state_t st = br_irq_lock();
    if (ready_count_now() != 0u) {
        br_irq_unlock(st);
        return;
    }
    cpu_wait();
    br_irq_unlock(st);
}

static void switch_next(void)
{
    for (;;) {
        br_thread_t *to = (s_ops->pick_next != BR_NULL) ? s_ops->pick_next() : BR_NULL;

        if (to == BR_NULL) {
            /* nobody runnable ⇒ idle (WFI critical section); re-pick after waking */
            idle_wait();
            continue;
        }
        if (to == s_current) {
            /*
             * The only runnable thread is self: a sleeper woken by timeout, or yield with no peer to give way to.
             * Do not switch stacks (switching to self is meaningless), set state back to RUNNING and continue.
             */
            to->state = BR_TASK_RUNNING;
            return;
        }

        br_thread_t *from = s_current;
        to->state = BR_TASK_RUNNING;
        s_current = to;
        br_sched_switch_to(from, to);   /* return = someone picked from again */
        return;
    }
}

/* =====================================================================
 * Register point
 * ===================================================================== */

br_bool br_sched_registered(void)
{
    return (s_ops != BR_NULL) ? BR_TRUE : BR_FALSE;
}

/*
 * **Read-only** view of the registry (`sched_internal.h`). Why it exists: the self-test suite must print the
 * evidence line "which scheduler got registered" (name/kind/tcb_size), and those fields live only here -- rather than
 * have the suite copy the plugin table (giving "what was registered" two sources of truth), open a read-only accessor.
 * Read-only: callers get no write path (changing the registration only goes through br_sched_register's "exactly once").
 */
const br_sched_ops_t *br_sched_ops_get(void)
{
    return s_ops;
}

void br_sched_register(const br_sched_ops_t *ops)
{
    if (s_ops != BR_NULL) {
        br_panic("[sched] br_sched_register(): double registration (3-01 §5.1 'exactly once')");
    }
    if (ops == BR_NULL) {
        br_panic("[sched] br_sched_register(): ops == NULL");
    }
    /* Required slots (ADR-0006 §2 ruling S-1: per-field comments take precedence over the blanket "all non-null except idle") */
    if (ops->name == BR_NULL || ops->thread_ready == BR_NULL ||
        ops->thread_block == BR_NULL || ops->pick_next == BR_NULL) {
        br_panic("[sched] br_sched_register(): name/thread_ready/thread_block/pick_next are required");
    }
    if (ops->kind > BR_SCHED_KIND_TT) {
        br_panic("[sched] br_sched_register(): illegal kind");
    }
    if (ops->tcb_size < (br_u32)sizeof(struct br_thread) ||
        ops->tcb_size > BR_TCB_SLOT_BYTES) {
        br_panic("[sched] br_sched_register(): tcb_size does not match the core TCB pool");
    }

    s_ops = ops;

    /*
     * I1: materialize the caller's boot context as the main thread.
     * After this step `br_task_self()` always returns it -- so threads created in APP's start() phase
     * and join work immediately (3-01 §9 boot sequence), without waiting for `br_sched_run()`.
     */
    br_thread_t *m = tcb_alloc();
    if (m == BR_NULL) {
        br_panic("[sched] br_sched_register(): TCB pool full, cannot create main");
    }
    m->state   = BR_TASK_RUNNING;
    m->name    = "main";
    m->prio    = 0u;
    m->wake_at = BR_TIMEOUT_INF;
    s_current  = m;
}

BR_NORETURN void br_sched_run(void)
{
    if (s_ops == BR_NULL) {
        br_panic("[sched] br_sched_run(): no scheduler registered (the plugin manager transition bridge should not reach here)");
    }
    if (s_current == BR_NULL) {
        br_panic("[sched] br_sched_run(): register did not create the main thread");
    }

    for (;;) {
        switch_next();
    }
}

/* Preemptive stack switch at the IRQ exit (defined at the end of this section; preconditions and safety argument at the definition). */
static void resched_from_irq(void);

void br_sched_irq_epilogue(void)
{
    if (s_ops == BR_NULL) {
        return;
    }

    /*
     * Expiry scan (same recipe as on_tick): covers exits from "non-timer interrupts" so timeout wakeups do not depend
     * on which particular IRQ woke the CPU out of idle.
     * ★ Must run before preemption: a thread woken by timeout must be selectable by this same exit's pick_next.
     */
    scan_timeouts(br_clock_now());

    resched_from_irq();
}

/* ------------------------------------------------------------- preemption */

void br_sched_request_resched(void)
{
    s_need_resched = BR_TRUE;
}

br_bool br_sched_resched_pending(void)
{
    return s_need_resched;
}

/*
 * Preemptive stack switch at the IRQ exit (ADR-0011; called by `br_sched_irq_epilogue`).
 *
 * Preconditions (guaranteed by the call site, not guessed): we are in an **exception context**, already eoi'd,
 * `irq_depth == 0` (INV-C just checked), `PSTATE.I == 1`.
 *
 * ★ Why this switch is safe (the one subtle point of the mechanism, must be pinned down):
 *   `from`'s context is saved by `br_sched_switch_to` as "callee-saved + sp" -- and sp points to
 *   **`br_irq_enter`'s stack frame** (the exception frame is below it). So the preempted thread stops "inside the IRQ-exit
 *   C code", and when it is selected again it returns from `br_sched_switch_to` and walks back through
 *   `resched_from_irq` -> `br_sched_irq_epilogue` -> `br_irq_enter` -> the asm stub's
 *   "restore context + ERET". ⇒ **its own ELR/SPSR values stay in the exception frame on its own stack**,
 *   no need to save them elsewhere on switch (on a single core those registers are global, but they are read only at
 *   the moment of "returning to its own stub").
 *   ⇒ Hence `switch.S` saving only callee-saved still suffices and vectors.S need not change -- the switch point is not in
 *   those asm lines at all (consistent with vectors.S header P-IRQ-ASM-2's "v2 obligation", which targets the other
 *   implementation that switches stacks in asm before ERET).
 *
 * ★ First entry of a new thread needs one more fix: here `PSTATE.I == 1` (still in exception handling) while the new
 *   thread's entry does not go through ERET ⇒ it must be explicitly corrected to "IRQs enabled". See `br_sched_thread_entry` (ruling P-1).
 */
static void resched_from_irq(void)
{
    if (s_need_resched == BR_FALSE) {
        return;
    }
    s_need_resched = BR_FALSE;

    br_thread_t *cur = s_current;

    /* Preemption only makes sense while the current thread is still running (block/exit paths already changed state). */
    if (s_ops == BR_NULL || cur == BR_NULL || cur->state != BR_TASK_RUNNING) {
        return;
    }

    /*
     * Put the preempted thread back into the ready structure (tail). **This is where round-robin semantics land**:
     * timeslice exhausted ⇒ go to the tail for the next round; `state` is still written by core (I2).
     */
    cur->state = BR_TASK_READY;
    if (s_ops->thread_ready != BR_NULL) {
        (void)s_ops->thread_ready(cur);
    }

    br_thread_t *to = (s_ops->pick_next != BR_NULL) ? s_ops->pick_next() : BR_NULL;

    if (to == BR_NULL) {
        /* Defensive (only reachable if the plugin is broken): cur was just queued but pick_next says nobody -- remove
         * cur and keep running. Must not "do nothing": that would leave cur queued while RUNNING (violates I2). */
        if (s_ops->thread_block != BR_NULL) {
            s_ops->thread_block(cur);
        }
        cur->state = BR_TASK_RUNNING;
        br_trace_emit(BR_TRACE_SCHED_PREEMPT, 0u, 0u);
        return;
    }
    if (to == cur) {
        /* Only self is runnable (pick_next already dequeued it): no stack switch, just continue (isomorphic to switch_next). */
        cur->state = BR_TASK_RUNNING;
        br_trace_emit(BR_TRACE_SCHED_PREEMPT, 0u, 0u);
        return;
    }

    to->state = BR_TASK_RUNNING;
    s_current = to;
    br_trace_emit(BR_TRACE_SCHED_PREEMPT, 0u, 1u);   /* evidence: the IRQ exit really switched stacks */
    br_sched_switch_to(cur, to);        /* return = someone picked cur again */
}

void br_sched_on_tick(br_time_t now)
{
    if (s_ops == BR_NULL) {
        return;
    }

    scan_timeouts(now);

    if (s_ops->on_tick != BR_NULL) {
        s_ops->on_tick();
    }
}

int br_sched_block_current(br_time_t abs_deadline)
{
    br_thread_t *t = s_current;

    if (t == BR_NULL || s_ops == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    /*
     * ★ bh ban (I6, ADR-0011): the bottom half runs on the IRQ-disabled exception exit and has **no** schedulable
     *   context to fall back to -- blocking here means the exception frame never returns (and the ISR's eoi is already
     *   done, presenting as "the system silently loses a line"). Rejected at runtime, returning -EPERM not -EINVAL:
     *   the arguments are fine, the context is wrong (callers must be able to tell these two bugs apart).
     */
    if (br_work_in_bh() != BR_FALSE) {
        br_trace_emit(BR_TRACE_BH_CTX_VIOLATION, 2u, 0u);
        return BR_ERR(EPERM);
    }
    if (t->state != BR_TASK_RUNNING) {
        return BR_ERR(EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();

    /*
     * ICB step 2 (ADR-0006 §3b): check the handshake bit at entry. The caller already set it to
     * -ETIMEDOUT **before opening the window**; reading 0 here means `br_sched_wake()` came after the caller opened
     * the window and before this function disabled IRQs -- return 0 directly, do not set BLOCKED, do not record a deadline, do not switch stacks.
     * This eliminates the lost-wakeup window "after pushing onto the wait chain / before actually BLOCKED".
     */
    if (t->wait_status == 0) {
        br_irq_unlock(st);
        return 0;
    }

    /* Deadline already passed (including the absolute form of relative 0) ⇒ finish as timeout immediately (INV-1), no stack switch, no sleeping until the next tick */
    if (abs_deadline != BR_TIMEOUT_INF && abs_deadline <= br_clock_now()) {
        t->wait_status = BR_ERR(ETIMEDOUT);
        br_irq_unlock(st);
        return BR_ERR(ETIMEDOUT);
    }

    t->wake_at = abs_deadline;          /* BR_TIMEOUT_INF = no deadline */
    t->state   = BR_TASK_BLOCKED;
    if (s_ops->thread_block != BR_NULL) {
        s_ops->thread_block(t);
    }
    br_irq_unlock(st);

    switch_next();                      /* give up the CPU; on return the pick side has set state back to RUNNING */
    return t->wait_status;
}

br_u32 br_sched_task_count(void)
{
    br_u32 n = 0u;

    for (br_u32 i = 0u; i < BR_TASK_MAX; i++) {
        if (s_pool[i].t.magic == BR_THREAD_MAGIC) {
            n++;
        }
    }
    return n;
}

/* Pool observation for the internal contract (`sched_internal.h`): number of occupied slots (= live threads). */
br_u32 br_thread_pool_used(void)
{
    return br_sched_task_count();
}

br_u32 br_sched_ready_count(void)
{
    return ready_count_now();
}

/* =====================================================================
 * Thread face (state machine: NEW -> READY ⇄ RUNNING / BLOCKED / ZOMBIE)
 * ===================================================================== */

BR_NORETURN void br_sched_thread_entry(struct br_thread *t)
{
    /*
     * ★ Ruling P-1 (ADR-0011): a new thread's first entry must put the L2 state right.
     *
     * Problem: a preempt-class scheduler may pick a thread that "never ran" at the **IRQ exit**
     * (`resched_from_irq` -> `br_sched_switch_to` -> trampoline). At that moment `PSTATE.I == 1`
     * (we are still in exception handling) and the new thread enters **without ERET** ⇒ if ignored it runs on with
     * IRQs disabled: the timer never fires again ⇒ preemption and timeouts both break (with no error reported).
     *
     * Handling: `irq_depth == 0` ⇒ the thread's proper state is "IRQs enabled", so explicitly enable them.
     *   `irq_depth > 0` ⇒ **do not touch** (and this is not an opportunistic fix): it means someone gave up the CPU
     *   inside an L2 critical section -- an existing ban (same root as INV-C) -- and enabling IRQs here would tear the critical
     *   section open. Trace without changing state: honest failure beats silently enabling IRQs.
     */
    if (br_irq_lock_depth() == 0u) {
        br_irq_cpu_enable();
    } else {
        br_trace_emit(BR_TRACE_BH_CTX_VIOLATION, 1u, (br_u64)br_irq_lock_depth());
    }

    if (t != BR_NULL && t->entry != BR_NULL) {
        t->entry(t->arg);               /* entry returning normally ⇒ fall through, no wild address */
    }
    br_task_exit(0);
}

int br_task_create(br_thread_t **out, const br_task_attr_t *attr,
                   void (*entry)(void *), void *arg)
{
    if (out == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    *out = BR_NULL;

    if (attr == BR_NULL || entry == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (attr->stack == BR_NULL || attr->stack_size < BR_STACK_MIN) {
        return BR_ERR(EINVAL);
    }
    if (s_ops == BR_NULL) {
        return BR_ERR(ENOTSUP);
    }

    br_thread_t *t = tcb_alloc();
    if (t == BR_NULL) {
        return BR_ERR(ENOMEM);
    }

    t->name       = (attr->name != BR_NULL) ? attr->name : "task";
    t->prio       = attr->prio;
    t->stack_top  = attr->stack;                 /* contract: stack = stack top (high address) */
    t->stack_size = attr->stack_size;
    t->entry      = entry;
    t->arg        = arg;
    t->exit_code  = 0;
    t->wait_status = BR_ERR(ETIMEDOUT);       /* ICB: set "not woken" before opening the window */
    t->wake_at    = BR_TIMEOUT_INF;

    /*
     * Initial context (matches switch.S's ctx layout verbatim):
     *   x19 = TCB (the trampoline reads it)   x30 = trampoline   sp = stack top (16-byte aligned)
     * x29 = 0: the fp chain starts at 0 (backtrace relies on it to stop).
     */
    t->ctx[BR_CTX_X19_TCB] = (br_u64)(br_uintptr_t)t;
    t->ctx[BR_CTX_X29]     = 0u;
    t->ctx[BR_CTX_X30]     = (br_u64)(br_uintptr_t)br_sched_thread_trampoline;
    t->ctx[BR_CTX_SP]      = (br_u64)(((br_uintptr_t)attr->stack) & ~(br_uintptr_t)15u);

    t->state = BR_TASK_READY;
    if (s_ops->thread_ready != BR_NULL) {
        const int r = s_ops->thread_ready(t);
        if (r != 0) {
            tcb_free(t);
            return r;
        }
    }

    *out = t;
    return 0;
}

int br_task_join(br_thread_t *t, int *exit_code)
{
    if (t == BR_NULL || t->magic != BR_THREAD_MAGIC) {
        return BR_ERR(EINVAL);
    }
    if (t == s_current) {
        return BR_ERR(EINVAL);                   /* self join */
    }

    const br_u32 i = tcb_index(t);
    if (i >= BR_TASK_MAX || s_joiner[i] != BR_NULL) {
        return BR_ERR(EINVAL);                   /* outside the pool / already joined by someone else */
    }

    if (t->state != BR_TASK_ZOMBIE) {
        s_joiner[i] = s_current;
        s_current->wait_status = BR_ERR(ETIMEDOUT);   /* ICB: open the window */
        const int w = br_sched_block_current(BR_TIMEOUT_INF);
        s_joiner[i] = BR_NULL;
        if (w != 0) {
            return w;                               /* join has no deadline ⇒ normally unreachable */
        }
    }

    if (exit_code != BR_NULL) {
        *exit_code = t->exit_code;
    }
    tcb_free(t);                                    /* ZOMBIE -> reclaim */
    return 0;
}

BR_NORETURN void br_task_exit(int code)
{
    br_thread_t *t = s_current;

    /*
     * ★ bh ban (I6, ADR-0011): ending a thread from the bottom half has no correct wind-down --
     *   its exception frame is still on the stack, and "switching away" would make ERET never return. The design's
     *   consistent stance is "honest failure beats silent degradation" ⇒ panic (rather than pretending the exit succeeded).
     */
    if (br_work_in_bh() != BR_FALSE) {
        br_trace_emit(BR_TRACE_BH_CTX_VIOLATION, 3u, 0u);
        br_panic("[sched] br_task_exit(): must not end a thread in the interrupt bottom half (bh) (ADR-0011 §4)");
    }

    if (t == BR_NULL || s_ops == BR_NULL) {
        /* Not in a thread context: there is no "exit" to speak of -- stop honestly (no faking) */
        for (;;) {
            cpu_wait();
        }
    }

    const br_u32 i = tcb_index(t);

    t->exit_code = code;
    t->state     = BR_TASK_ZOMBIE;
    t->wake_at   = BR_TIMEOUT_INF;
    if (s_ops->thread_block != BR_NULL) {
        s_ops->thread_block(t);                     /* defensive removal (usually not queued) */
    }

    /* Wake the joiner (ICB: even if it is still inside the RUNNING window, write the handshake bit only) */
    if (i < BR_TASK_MAX && s_joiner[i] != BR_NULL) {
        br_sched_wake(s_joiner[i]);
    }

    for (;;) {
        switch_next();                              /* ZOMBIE is never queued again; switch to someone else or idle */
    }
}

void br_task_yield(void)
{
    if (s_current == BR_NULL || s_ops == BR_NULL) {
        return;
    }
    /*
     * ★ bh ban (I6, ADR-0011): yielding the CPU in the bottom half == losing the exception exit's return path.
     *   `br_task_yield` is void ⇒ it cannot return an error code, so it **refuses and traces** (rather than silently
     *   treating it as a successful yield: the caller would think a switch happened when nothing did -- also a lie).
     */
    if (br_work_in_bh() != BR_FALSE) {
        br_trace_emit(BR_TRACE_BH_CTX_VIOLATION, 4u, 0u);
        return;
    }
    if (s_current->state != BR_TASK_RUNNING) {
        return;
    }

    s_current->state = BR_TASK_READY;
    if (s_ops->thread_ready != BR_NULL) {
        (void)s_ops->thread_ready(s_current);
    }
    switch_next();
}

int br_task_sleep(br_time_t rel_us)
{
    if (rel_us == BR_TIMEOUT_INF) {
        return BR_ERR(EINVAL);       /* ruling G13: relative sleep has no "infinite" semantics */
    }
    if (rel_us == 0u) {
        return 0;                       /* return immediately (TC-TIME-002) */
    }
    return br_task_sleep_until(br_deadline_from_now(rel_us));
}

int br_task_sleep_until(br_time_t abs_us)
{
    if (s_current == BR_NULL || s_ops == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (abs_us == 0u || (abs_us != BR_TIMEOUT_INF && abs_us <= br_clock_now())) {
        return 0;                       /* deadline passed ⇒ return 0 immediately (TC-TASK-005 rule) */
    }

    s_current->wait_status = BR_ERR(ETIMEDOUT);   /* ICB: open the window */

    const int r = br_sched_block_current(abs_us);

    /*
     * Ruling G13b (ADR-0006 §2): sleep **expiry is a success path** ⇒ return 0;
     * -ETIMEDOUT belongs only to APIs that "wait for an event and time out" (mutex/sem/cond/join, INV-1).
     */
    return (r == BR_ERR(ETIMEDOUT)) ? 0 : r;
}

br_thread_t *br_task_self(void)
{
    return s_current;
}

const char *br_task_name(const br_thread_t *t)
{
    if (t == BR_NULL || t->magic != BR_THREAD_MAGIC) {
        return "?";
    }
    return (t->name != BR_NULL) ? t->name : "?";
}

br_u32 br_task_state(const br_thread_t *t)
{
    if (t == BR_NULL || t->magic != BR_THREAD_MAGIC) {
        return BR_TASK_NEW;
    }
    return t->state;
}

