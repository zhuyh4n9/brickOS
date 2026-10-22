/*
 * Scheduler framework (core): thread state machine, timeout wakeups, idle, first schedule.
 * Only core writes `state`; plugins only change ready-queue membership. br_sched_wake() writes
 * wait_status = 0 first and requeues only when BLOCKED (ICB handshake, wins a wake/timeout race).
 * Switches happen at yield/block/exit/first schedule plus the IRQ exit when need_resched was set;
 * bh is not a thread context (block = -EPERM, yield = reject + trace, exit = panic).
 */
#include <br/core/br_error.h>
#include <br/hal/br_fault.h>
#include <br/hal/br_irq.h>
#include <br/core/br_log.h>
#include <br/hal/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_trace.h>
#include <br/core/br_types.h>
#include <br/hal/br_work.h>

#include "sched_ctx.h"
#include "sched_internal.h"

/* ---- TCB pool (static; no dynamic allocation) ----
 * Slot = common header + plugin tail; register fails loudly if ops.tcb_size > BR_TCB_SLOT_BYTES
 * rather than truncating silently. */
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
static br_thread_t         *s_current;    /* current thread (single core) */
static const br_sched_ops_t *s_ops;
static br_thread_t         *s_joiner[BR_TASK_MAX];  /* target -> its joiner (one at a time) */

/* Preemption request bit: set by the plugin's tick policy (ISR), consumed at the IRQ exit.
 * volatile covers the "ISR writes / exit reads" pair; single core needs no atomic instruction. */
static volatile br_bool s_need_resched;

/* Host-side idle wait hook (weak): target idle issues wfi; the host test advances a fake clock and
 * calls br_sched_on_tick() here, making the tick timeout scan testable on the host. The weak
 * declaration lives in sched_internal.h (ADR-0038 / R1). */

/* ---- small helpers ---- */

br_size_t br_sched_tcb_size(void)
{
    return (br_size_t)sizeof(struct br_thread);
}

void *br_thread_priv(br_thread_t *t)
{
    return (void *)((br_u8 *)t + sizeof(struct br_thread));
}

/* ---- wait-chain primitives (zero allocation: the node is wait_next in the TCB) ----
 * They live in core because the node layout belongs to the frozen TCB and the block return path
 * needs unchain + handshake bit together; they do not depend on scheduler state. */

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

/* ---- wakeup / timeout (ICB handshake) ---- */

/* Requeue t: core writes state, the plugin only enqueues. Caller holds L2, or atomicity is not
 * required. */
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

    /* ICB step 3: write the handshake bit first. If t is still RUNNING this is an early wake inside
     * the window -- bit only, no enqueue; br_sched_block_current returns 0 without switching. */
    t->wait_status = 0;

    if (t->state == BR_TASK_BLOCKED) {
        t->wake_at = BR_TIMEOUT_INF;   /* stop tracking the deadline */
        make_ready(t);
    } else {
        /* Already READY / RUNNING / ZOMBIE: handshake bit only (idempotent). */
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

/* ---- switch: pick_next -> stack switch; nothing ready -> idle ---- */

static void idle_wait(void)
{
    if (s_ops->idle != BR_NULL) {
        s_ops->idle();
        return;
    }

    /* Disable IRQs -> check no ready work -> WFI -> enable IRQs. The check and the WFI must not be
     * interrupted, or an interrupt landing between them sleeps until the next one (lost wakeup).
     * WFI still returns immediately on a pending interrupt even with PSTATE.I == 1. */
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
            /* Only self is runnable (timeout wakeup, or yield with no peer): switching to self is
             * meaningless, so set RUNNING and continue. */
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

/* ---- register point ---- */

br_bool br_sched_registered(void)
{
    return (s_ops != BR_NULL) ? BR_TRUE : BR_FALSE;
}

/* Read-only registry view for the selftest evidence line (name/kind/tcb_size); no write path. */
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
    /* Required slots (per-field rules take precedence over "all non-null except idle"). */
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

    /* Materialize the caller's boot context as the main thread: after this br_task_self() and
     * br_task_create/join work before br_sched_run(). */
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

/* Preemptive stack switch at the IRQ exit; preconditions and safety argument at the definition. */
static void resched_from_irq(void);

void br_sched_irq_epilogue(void)
{
    if (s_ops == BR_NULL) {
        return;
    }

    /* Expiry scan, as on_tick: covers non-timer interrupts, so timeout wakeups do not depend on
     * which IRQ woke the CPU. Must run before preemption, so a woken thread is selectable now. */
    scan_timeouts(br_clock_now());

    resched_from_irq();
}

/* ---- preemption ---- */

void br_sched_request_resched(void)
{
    s_need_resched = BR_TRUE;
}

br_bool br_sched_resched_pending(void)
{
    return s_need_resched;
}

/*
 * Preemptive stack switch at the IRQ exit; call site guarantees exception context, already eoi'd,
 * irq_depth == 0, PSTATE.I == 1. from is saved as callee-saved + sp, and sp points at
 * br_irq_enter's frame (exception frame below), so the thread resumes inside the IRQ-exit C code
 * with its own ELR/SPSR intact on its own stack. A new thread's first entry bypasses ERET, so
 * br_sched_thread_entry corrects PSTATE.I.
 */
static void resched_from_irq(void)
{
    if (s_need_resched == BR_FALSE) {
        return;
    }
    s_need_resched = BR_FALSE;

    br_thread_t *cur = s_current;

    /* Preempt only while the current thread is still running (block/exit already changed state). */
    if (s_ops == BR_NULL || cur == BR_NULL || cur->state != BR_TASK_RUNNING) {
        return;
    }

    /* Back to the ready-structure tail: this is where round-robin lands (exhausted timeslice goes
     * to the next round); core still writes state. */
    cur->state = BR_TASK_READY;
    if (s_ops->thread_ready != BR_NULL) {
        (void)s_ops->thread_ready(cur);
    }

    br_thread_t *to = (s_ops->pick_next != BR_NULL) ? s_ops->pick_next() : BR_NULL;

    if (to == BR_NULL) {
        /* Defensive (broken plugin): cur was queued but pick_next says nobody -- remove cur and
         * keep running, else it stays queued while RUNNING. */
        if (s_ops->thread_block != BR_NULL) {
            s_ops->thread_block(cur);
        }
        cur->state = BR_TASK_RUNNING;
        br_trace_emit(BR_TRACE_SCHED_PREEMPT, 0u, 0u);
        return;
    }
    if (to == cur) {
        /* Only self is runnable (already dequeued): no stack switch, continue as in switch_next. */
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
    /* bh ban: the bottom half runs on the IRQ-disabled exception exit with no schedulable context,
     * so blocking loses the exception frame. -EPERM not -EINVAL: arguments fine, context wrong. */
    if (br_work_in_bh() != BR_FALSE) {
        br_trace_emit(BR_TRACE_BH_CTX_VIOLATION, 2u, 0u);
        return BR_ERR(EPERM);
    }
    if (t->state != BR_TASK_RUNNING) {
        return BR_ERR(EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();

    /* ICB step 2: the caller pre-set wait_status = -ETIMEDOUT before opening the window; 0 here
     * means br_sched_wake() came after the window opened and before IRQs were disabled -- return 0
     * without setting BLOCKED, recording a deadline or switching. */
    if (t->wait_status == 0) {
        br_irq_unlock(st);
        return 0;
    }

    /* Deadline already passed (including relative 0): finish as timeout now, no stack switch and
     * no sleeping until the next tick. */
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

/* Pool observation for sched_internal.h: occupied slots (= live threads). */
br_u32 br_thread_pool_used(void)
{
    return br_sched_task_count();
}

br_u32 br_sched_ready_count(void)
{
    return ready_count_now();
}

/* ---- thread face: NEW -> READY <-> RUNNING / BLOCKED / ZOMBIE ---- */

BR_NORETURN void br_sched_thread_entry(struct br_thread *t)
{
    /*
     * A new thread's first entry may happen at the IRQ exit (preempt-class scheduler), where
     * PSTATE.I == 1 and there is no ERET: enable IRQs when irq_depth == 0, else trace and leave
     * them alone -- enabling inside an L2 critical section would tear it open.
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

    /* Initial context (switch.S's ctx layout): x19 = TCB, x30 = trampoline, sp = stack top
     * (16-byte aligned), x29 = 0 so the fp chain terminates for backtrace. */
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

    /* bh ban: ending a thread from the bottom half leaves its exception frame on the stack and ERET
     * would never return, so panic rather than pretend the exit succeeded. */
    if (br_work_in_bh() != BR_FALSE) {
        br_trace_emit(BR_TRACE_BH_CTX_VIOLATION, 3u, 0u);
        br_panic("[sched] br_task_exit(): must not end a thread in the interrupt bottom half (bh) (ADR-0011 §4)");
    }

    if (t == BR_NULL || s_ops == BR_NULL) {
        /* Not in a thread context: stop honestly, there is no "exit" to speak of. */
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

    /* Wake the joiner (ICB: bit only if it is still inside the RUNNING window). */
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
    /* bh ban: yielding in the bottom half loses the exception exit path. void return, so refuse and
     * trace rather than report a switch that did not happen. */
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

    /* Sleep expiry is a success path => 0; -ETIMEDOUT belongs to APIs that wait for an event and
     * time out (mutex/sem/cond/join). */
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

