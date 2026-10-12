/*
 * brickOS prototype v0.2.0 — scheduler framework **internal** contract (core-private, not for plugins)
 *
 * This header is only for the .c files under `core/src/sched/`. It freezes two cross-file facts:
 *   1. the **common head** layout of the TCB (the real body of `br_thread_t`) — scheduler plugins get only an opaque pointer, but
 *      `tcb_size` must cover "common head + plugin-private tail";
 *   2. wait-chain nodes and operations (`br_waitq_*`) — synchronization primitives (the `br_sync.h` implementation) attach/detach
 *      waiters with zero allocation; the node is embedded in the TCB.
 *
 * Why the TCB layout is pinned: the context-switch assembly (`switch.S`) accesses fixed offsets, and the synchronization primitives
 * modify `state`/`wake_at`/`wait_next` directly. If all three computed their own offsets, there would be three sources of truth.
 */
#ifndef BR_SCHED_INTERNAL_H
#define BR_SCHED_INTERNAL_H

#include <br/core/br_sched.h>
#include <br/core/br_types.h>

/* TCB magic word (recognizes "is this a TCB"; for observation/selftest). */
#define BR_THREAD_MAGIC 0x54484221u /* "THB!" */

/*
 * Context save area. COOP switch = save callee-saved + stack pointer:
 *   [0..9]  x19..x28
 *   [10]    x29(fp)   — `-fno-omit-frame-pointer` is a premise of backtrace, and of this save area too
 *   [11]    x30(lr)   — points at the trampoline when a new thread first enters
 *   [12]    sp        — the thread's own stack
 */
#define BR_THREAD_CTX_WORDS 13u

struct br_thread {
    br_u32       magic;
    br_u32       state;         /* BR_TASK_* */
    br_u8        prio;
    const char  *name;
    void        *stack_top;
    br_size_t    stack_size;
    void       (*entry)(void *);
    void        *arg;
    int          exit_code;
    br_time_t    wake_at;       /* BR_TIMEOUT_INF = no deadline */
    /*
     * ★ **ICB's only handshake slot** (ADR-0006 §3b): all-zero = "already woken", non-zero (by convention -ETIMEDOUT)
     * = "not yet woken". Three write points: the caller sets -ETIMEDOUT before opening the window; `br_sched_wake` sets 0;
     * the timeout scan sets -ETIMEDOUT. `br_sched_block_current` returns 0 immediately at entry only when it is 0
     * (early wake inside the window), otherwise it sets BLOCKED. Read point: on blocking return the value becomes the return value.
     * It is also the sole winner when "unlock ownership handoff" and "timeout" race.
     */
    int          wait_status;
    struct br_thread *wait_next;/* wait chain: shared by mutex/sem/cond/join */
    struct br_thread *all_next; /* global TCB chain (traversal/observation) */
    br_u64       ctx[BR_THREAD_CTX_WORDS];
    /* Plugin-private tail **starts here** (size = ops.tcb_size - sizeof(struct br_thread)).
     * core never touches it; plugins get it via `br_thread_priv(t)` (declared in `br_sched.h` — it is the
     * plugin-visible face; `br_sched_tcb_size()` pairs with it = the two halves of `ops.tcb_size`). */
};

/* ------------------------------------------------------------------ switch layer
 * Provided by `switch.S` (on target); host tests provide a fake with the same signature (ucontext, see tests/host). */

/* Save from's callee-saved + sp, restore to's, switch stacks. See `sched_ctx.h` for the ctx layout. */
void br_sched_switch_to(struct br_thread *from, struct br_thread *to);

/* First entry of a new thread (written into ctx[x30] at create); x19 holds the TCB pointer. */
BR_NORETURN void br_sched_thread_trampoline(void);

/* C half of the trampoline: calls entry(arg), and on return br_task_exit(0) (does not return). */
BR_NORETURN void br_sched_thread_entry(struct br_thread *t);

/* ==================================================================== wait chain */
/*
 * The wait chain is a **singly linked list with nodes inside threads**: enqueue appends t to the tail, dequeue unlinks the head.
 * Zero allocation is a key property of this design (the sync path cannot allocate — allocation can fail, a lock cannot).
 */
void          br_waitq_push(void **head, br_thread_t *t);
br_thread_t  *br_waitq_pop(void **head);
int           br_waitq_remove(void **head, br_thread_t *t); /* 1 = removed, 0 = not on the chain */
br_u32        br_waitq_len(const void *head);

/* TCB pool (static upper bound; for observation). */
#ifndef BR_TASK_MAX
#define BR_TASK_MAX 8u
#endif
br_u32 br_thread_pool_used(void);

/*
 * Registered scheduler ops (sched_core.c's source of truth; unregistered ⇒ BR_NULL).
 * Why this opening exists here: the selftest suite (`core/selftest/sched_selftest.c`) must print the evidence
 * line `scheduler=%s kind=%u spawn=%u` — name/kind/tcb_size exist only in the registry.
 * Letting it read the **same** production truth beats copying a plugin table into the suite (that would be two sources of truth).
 * Read-only: this function gives the caller no write path.
 */
const br_sched_ops_t *br_sched_ops_get(void);

#endif /* BR_SCHED_INTERNAL_H */
