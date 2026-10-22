/*
 * Scheduling framework: threads/tasks plus scheduler registration. Core owns the context-switch
 * asm, TCB common header, stacks, timeout wakeup, idle and first schedule; the scheduler plugin
 * owns the ready queue, pick_next, tick policy and preemption decision. Core also implements the
 * block/wake mechanism and the sync primitives (br_sync.h), so the plugin only answers "who runs
 * next"; br_sched_ops covers tcb_size/sleep_until/idle plus the ready/block/pick/tick calls.
 */
#ifndef BR_CORE_BR_SCHED_H
#define BR_CORE_BR_SCHED_H

#include <br/core/br_types.h>
#include <br/core/br_time.h>

/* ---- threads ---- */

/* TCB is opaque: the common header lives in `core/c_core/src/sched/sched_internal.h` and the plugin
 * private tail follows it (`tcb_size` comes from the plugin). */
typedef struct br_thread br_thread_t;

/* Thread states (observation and self-test). */
#define BR_TASK_NEW      0u
#define BR_TASK_READY    1u
#define BR_TASK_RUNNING  2u
#define BR_TASK_BLOCKED  3u
#define BR_TASK_ZOMBIE   4u

/* Minimum thread stack size (2 KiB); smaller => br_task_create returns -EINVAL. */
#define BR_STACK_MIN 2048u

/* Creation parameters. `stack` is caller-provided (no dynamic allocation) and points at the
 * high-address end of the range `[stack - stack_size, stack)`. */
typedef struct br_task_attr {
    const char *name;
    void       *stack;        /* stack top (high address); aarch64 stacks grow down */
    br_size_t   stack_size;
    br_u8       prio;         /* 0 = lowest; only meaningful for preempt/tt (coop ignores) */
    br_u32      flags;        /* reserved (0) */
} br_task_attr_t;

/* Create a thread: NEW -> READY. Failure: -EINVAL / -ENOMEM (TCB pool full) / -ENOTSUP. */
int br_task_create(br_thread_t **out, const br_task_attr_t *attr,
                   void (*entry)(void *), void *arg);

/* Wait for a thread to finish (ZOMBIE -> reclaim TCB). `exit_code` may be null. */
int br_task_join(br_thread_t *t, int *exit_code);

/* End the current thread (does not return). */
BR_NORETURN void br_task_exit(int code);

/* Yield the CPU (tail of READY; schedules immediately). Under coop the only voluntary yield. */
void br_task_yield(void);

/* Sleep: become READY after `rel_us` microseconds. 0 => return 0 immediately; BR_TIMEOUT_INF =>
 * -EINVAL (relative sleep has no "infinite" meaning). Never wakes early; lateness unbounded. */
int br_task_sleep(br_time_t rel_us);
int br_task_sleep_until(br_time_t abs_us);

br_thread_t *br_task_self(void);
const char *br_task_name(const br_thread_t *t);
br_u32 br_task_state(const br_thread_t *t);

/* ---- registration ---- */

/* Scheduler kind (plugin-side `[plugin].sched_kind`). */
#define BR_SCHED_KIND_COOP    0u
#define BR_SCHED_KIND_PREEMPT 1u
#define BR_SCHED_KIND_TT      2u

/* The entire interface a scheduler plugin implements; core calls back only here. All but `idle`
 * must be non-null (null => registration rejected with -EINVAL). */
typedef struct br_sched_ops {
    const char *name;         /* for display (e.g. "sched/coop") */
    br_u32      kind;         /* BR_SCHED_KIND_* */
    br_u32      tcb_size;     /* total bytes per TCB (>= core common header size) */

    /* Called on NEW/wakeup → READY (attach t to the ready structure). */
    int  (*thread_ready)(br_thread_t *t);
    /* Called on RUNNING → BLOCKED (detach from the ready structure). */
    void (*thread_block)(br_thread_t *t);
    /* Next thread to run; BR_NULL ⇒ "nobody runnable" (core enters idle/WFI). */
    br_thread_t *(*pick_next)(void);
    /* Tick driver point (called from the timer interrupt; may be BR_NULL for COOP). */
    void (*on_tick)(void);
    /* Register "wake t at abs_us" (core's timeout framework walks these). May be BR_NULL (no timeout support). */
    int  (*sleep_until)(br_thread_t *t, br_time_t abs_us);
    /* Custom idle (may be BR_NULL ⇒ use core's WFI critical section). */
    void (*idle)(void);
} br_sched_ops_t;

/* EARLY-phase registration, exactly once (second => panic). */
void br_sched_register(const br_sched_ops_t *ops);

/* Whether registration happened (with no scheduler the APP start holds the CPU). */
br_bool br_sched_registered(void);

/* First schedule + idle loop (does not return). Called by the plugin manager after all start(). */
BR_NORETURN void br_sched_run(void);

/* IRQ exit decision point (called by core's IRQ exit path). Under COOP it only scans the expiry
 * table (no stack switch); a PREEMPT-class scheduler that set need_resched really switches stacks
 * here. */
void br_sched_irq_epilogue(void);

/*
 * Request "schedule once at this interrupt exit" (ISR-safe); the only preemption entry. Sets only
 * a need_resched bit -- no stack switch, no ready-structure change. At IRQ exit core puts the
 * current thread back at the ready tail, picks the next per pick_next and switches stacks when
 * needed. Tick-policy schedulers call it; COOP never does (no preemption).
 */
void br_sched_request_resched(void);

/* Current need_resched state (observation/self-test). */
br_bool br_sched_resched_pending(void);

/* Timer tick (platform timer ISR): advance time base, wake expired threads, then ops.on_tick. */
void br_sched_on_tick(br_time_t now);

/* Block the current thread voluntarily (mechanism face of the wait chain; br_sync.h builds on it).
 * `abs_deadline == BR_TIMEOUT_INF` => wait forever; returns 0 woken, -ETIMEDOUT timed out.
 * Caller pre-obligation: before opening the window (inside the IRQ-off critical section) set
 * br_task_self()->wait_status to -ETIMEDOUT, then push the wait chain, enable IRQs and call this.
 * If wait_status is already 0 on entry the wake happened inside the window: return 0 directly,
 * without setting BLOCKED, registering a deadline or switching stacks (first writer wins).
 * Calling this inside bh returns -EPERM: bh has no schedulable context to fall back to. */
int br_sched_block_current(br_time_t abs_deadline);

/* Turn t back to READY (ISR-safe: state change + chain attach only). Writes t->wait_status = 0
 * first, then sets READY and enqueues only if t->state == BLOCKED; otherwise only the handshake
 * bit is written (early wake inside the window). Idempotent. */
void br_sched_wake(br_thread_t *t);

/* Byte size of the TCB common header (= offset of the plugin private tail); plugins use it as
 * tcb_size = br_sched_tcb_size() + sizeof(private tail). */
br_size_t br_sched_tcb_size(void);

/* Plugin private tail, right after the common header; only the owning plugin touches it. */
void *br_thread_priv(br_thread_t *t);

/* Ready/blocked/live counts (observation; dump and self-test). */
br_u32 br_sched_task_count(void);
br_u32 br_sched_ready_count(void);

/* The scheduling selftest entry is declared in core/c_core/selftest/, not here (test surface). */

#endif /* BR_CORE_BR_SCHED_H */
