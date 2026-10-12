/*
 * brickOS prototype v0.2.0 — scheduling framework (core): threads/tasks + scheduler registration
 *
 * Design basis: `3-01` §5.1 (`br_sched_ops` registration point), §6 (task/thread), §14 CA-1 (time
 *   units), `3-03` (scheduler overview), `3-02` §16.4 (IRQ ↔ scheduling seam).
 *
 * ## Split (ADR-0006 §1; the two deviations from the design are stated here)
 *
 * | face | owner |
 * |---|---|
 * | context-switch asm / TCB common header / stack and trampoline / timeout wakeup framework / idle / first schedule | **core** |
 * | ready queue / `pick_next` / tick policy / preemption decision / scheduling table | **scheduler plugin** |
 * | block and wake **mechanism** (block_current / wake / wait chain) | **core** (see deviation 1) |
 * | lock and semaphore **objects and semantics** (mutex/sem/cond) | **core** (`br_sync.h`) |
 *
 * **Deviation 1**: design `3-01` §5.1 puts `sem_take/mutex_lock/…` into `br_sched_ops` (i.e.
 * "sync implemented in the scheduler plugin", `3-01` line 115). This prototype instead has
 * **core implement the sync primitives**; the plugin only provides "who runs next". Reasons:
 * (a) lock objects must be visible to **all** plugins, and putting them in the scheduler plugin
 * would make every user depend back on the plugin header; (b) the block/wake mechanism is the
 * same for coop and preempt, only "who runs first" differs; (c) the design's ops field table is
 * itself a **draft with ellipsis** (`3-03` §2, pending). Recorded item by item in ADR-0006 §2.
 *
 * **Deviation 2**: the `br_sched_ops` field table is **finalized** here (filling the 5 slot
 * classes the design omitted): `tcb_size` (total TCB size: core common header + plugin private
 * tail), `sleep_until` (register a wake deadline), `idle` (plugin-specific idle; default is
 * core's WFI loop). The original draft's `sem_take/sem_give/mutex_lock/mutex_unlock/work_submit/
 * irq_epilogue` no longer enter ops (see deviation 1 and `br_sched_irq_epilogue` below).
 */
#ifndef BR_CORE_BR_SCHED_H
#define BR_CORE_BR_SCHED_H

#include <br/core/br_types.h>
#include <br/core/br_time.h>

/* ==================================================================== threads */

/* The TCB is **opaque**: the common header is in `core/src/sched/sched_internal.h`, the plugin
 * private tail follows it (`tcb_size` comes from the plugin). Plugin headers need only this
 * forward declaration. */
typedef struct br_thread br_thread_t;

/* Thread states (observation and self-test; the state machine of design `3-01` §6). */
#define BR_TASK_NEW      0u
#define BR_TASK_READY    1u
#define BR_TASK_RUNNING  2u
#define BR_TASK_BLOCKED  3u
#define BR_TASK_ZOMBIE   4u

/* Minimum thread stack size (design `3-01` §2.2 `BR_STACK_MIN [?]`; prototype uses 2 KiB).
 * Smaller ⇒ `br_task_create` returns `-EINVAL`. */
#define BR_STACK_MIN 2048u

/* Creation parameters. `stack` is provided by the **caller** (no dynamic allocation — prototype
 * scope, ADR-0006 §2). `stack` points at the **high-address end** of the stack range (stack top);
 * range = `[stack - stack_size, stack)`. */
typedef struct br_task_attr {
    const char *name;
    void       *stack;        /* stack top (high address); aarch64 stacks grow down */
    br_size_t   stack_size;
    br_u8       prio;         /* 0 = lowest; only meaningful for preempt/tt (coop ignores) */
    br_u32      flags;        /* reserved (0) */
} br_task_attr_t;

/* Create a thread: NEW → READY (enters the ready queue). Failure: -EINVAL / -ENOMEM (TCB pool full) / -ENOTSUP. */
int br_task_create(br_thread_t **out, const br_task_attr_t *attr,
                   void (*entry)(void *), void *arg);

/* Wait for a thread to finish (ZOMBIE → reclaim TCB). `exit_code` may be null. */
int br_task_join(br_thread_t *t, int *exit_code);

/* End the current thread (does not return). */
BR_NORETURN void br_task_exit(int code);

/* Yield the CPU (tail of READY; triggers a schedule immediately). Under coop this is the only "voluntary yield". */
void br_task_yield(void);

/* Sleep: become READY after `rel_us` microseconds. 0 ⇒ return 0 immediately; BR_TIMEOUT_INF ⇒
 * -EINVAL (ruling G13: relative sleep has no "infinite" meaning — that is not returning).
 * Guaranteed **not to wake early**; no upper bound on lateness (`3-01` INV-2). */
int br_task_sleep(br_time_t rel_us);
int br_task_sleep_until(br_time_t abs_us);

br_thread_t *br_task_self(void);
const char *br_task_name(const br_thread_t *t);
br_u32 br_task_state(const br_thread_t *t);

/* ==================================================================== registration */

/* Scheduler kind (corresponds to plugin-side `[plugin].sched_kind`). */
#define BR_SCHED_KIND_COOP    0u
#define BR_SCHED_KIND_PREEMPT 1u
#define BR_SCHED_KIND_TT      2u

/*
 * The **entire** interface a scheduler plugin implements (the finalized table of deviation 2).
 * core calls back only at these points. All but `idle` must be non-null (null ⇒ registration
 * rejected, -EINVAL).
 */
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

/* EARLY-phase registration, **exactly once** (second ⇒ panic, `3-01` §5.1). */
void br_sched_register(const br_sched_ops_t *ops);

/* Whether registration happened (the plugin manager's transition bridge uses it: with no scheduler, the APP start holds the CPU). */
br_bool br_sched_registered(void);

/* First schedule + idle loop (does not return). Called by the plugin manager after all start(). */
BR_NORETURN void br_sched_run(void);

/* IRQ exit decision point (`core/src/irq/irq_core.c` already calls it on the IRQ exit path).
 * Under COOP it only scans the expiry table (no stack switch — I4); when a PREEMPT-class
 * scheduler has set need_resched (see `br_sched_request_resched`) it **really switches stacks**
 * here. */
void br_sched_irq_epilogue(void);

/*
 * Request "schedule once at this interrupt exit" (ADR-0011; ISR-safe). **The only preemption entry**.
 *
 * Semantics: set only a need_resched bit (no stack switch, no ready-structure change — I2 says
 * only core changes state). When core sees it at IRQ exit (after eoi, before ERET), it puts the
 * **current thread back into the ready structure (tail)**, picks the next one per `pick_next`,
 * and calls `br_sched_switch_to` to switch stacks when needed.
 *
 * Who should call: schedulers with a tick policy (e.g. `sched/rr` slice expiry).
 * **COOP never calls it** ⇒ coop behavior matches the ADR-0006 cut verbatim (no preemption).
 * Why "set a bit + core executes uniformly" instead of letting the plugin switch itself: stack
 * switching must be bound to the state machine and the exception exit order (after eoi) — that
 * is mechanism (core); the plugin only answers "when to switch".
 */
void br_sched_request_resched(void);

/* Current need_resched state (observation/self-test; consumed by `br_sched_irq_epilogue`). */
br_bool br_sched_resched_pending(void);

/* Timer tick driver point (called by the platform timer ISR): advance the kernel time base and
 * wake expired threads, then call back `ops.on_tick`. */
void br_sched_on_tick(br_time_t now);

/* Current thread blocks voluntarily (mechanism face of the wait chain; `br_sync.h` primitives
 * build on it). `abs_deadline == BR_TIMEOUT_INF` ⇒ wait forever. Return 0 = woken,
 * -ETIMEDOUT = timed out.
 *
 * ★ **ICB handshake pre-obligation** (ADR-0006 §3b; item by item with `sync.c`): the caller must,
 *   **before opening the window** (inside the IRQ-off critical section), first set
 *   `br_task_self()->wait_status` to `-ETIMEDOUT`, and only then push the wait chain, enable
 *   IRQs and call this function. If the entry reads `wait_status == 0`, it assumes "already
 *   handed over by `br_sched_wake` inside the window" and returns 0 directly **without setting
 *   BLOCKED / registering a deadline / switching stacks** — the only way to close the window
 *   between "push" and "actually blocked". `wait_status` is the single handshake bit: first
 *   writer wins.
 *
 * ★ **bh prohibition** (ADR-0011): bottom halves (bh) run on the IRQ-off exception exit with
 *   **no schedulable context to fall back to** ⇒ calling this function inside bh returns
 *   `-EPERM` (no state change, no stack switch). This is runtime enforcement, not paper
 *   discipline. */
int br_sched_block_current(br_time_t abs_deadline);

/* Turn t back to READY (ISR-safe: only state change + chain attach, no stack change).
 * ICB step 3: **first** write `t->wait_status = 0`, and set READY + enqueue only if
 * `t->state == BLOCKED`; otherwise only the handshake bit is written (early wake inside the
 * window). Idempotent. */
void br_sched_wake(br_thread_t *t);

/* Byte size of the TCB common header (= offset of the plugin private tail). Plugins use it when
 * filling `br_sched_ops.tcb_size`:
 *   `tcb_size = br_sched_tcb_size() + sizeof(private tail)`
 * (same shape as CA-2 in design `3-01` §2.2; the private tail is reached via `br_thread_priv`.) */
br_size_t br_sched_tcb_size(void);

/* Plugin private tail pointer (immediately after the common header). core does not touch it; a plugin only reads/writes its own tail. */
void *br_thread_priv(br_thread_t *t);

/* Ready/blocked/live counts (observation; dump and self-test). */
br_u32 br_sched_task_count(void);
br_u32 br_sched_ready_count(void);

/*
 * The scheduling suite entry `br_sched_selftest()` is deliberately **not** declared here.
 * Reason: a self-test entry is **not a plugin's external capability**, and keeping it here would
 * put it on the golden interface surface — turning "change one test case" into an interface
 * change (same root as ADR-0005 ruling 9 on hooks). Declarations are centralized in
 * `core/selftest/core_selftest.c`, and the implementation lives with the suite in
 * `core/selftest/sched_selftest.c` (ADR-0010).
 */

#endif /* BR_CORE_BR_SCHED_H */
