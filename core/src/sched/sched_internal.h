/*
 * Scheduler framework internal contract (core-private, only for core/src/sched/): pins the TCB
 * common-head layout (tcb_size = head + plugin tail) and the wait-chain nodes/ops embedded in the
 * TCB, so the sync path never allocates. switch.S reads fixed ctx offsets.
 */
#ifndef BR_SCHED_INTERNAL_H
#define BR_SCHED_INTERNAL_H

#include <br/core/br_sched.h>
#include <br/core/br_types.h>

/* TCB magic word ("THB!"): recognizes a TCB for observation/selftest. */
#define BR_THREAD_MAGIC 0x54484221u

/* Context save area (callee-saved + sp; COOP switches at C call boundaries): [0..9] x19..x28,
 * [10] x29(fp; -fno-omit-frame-pointer premise of backtrace), [11] x30(lr; trampoline on first
 * entry), [12] sp. */
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
    /* ICB's only handshake slot: 0 = already woken, non-zero (-ETIMEDOUT) = not yet. Caller sets
     * -ETIMEDOUT before the window, br_sched_wake sets 0, timeout scan sets -ETIMEDOUT; decides the
     * winner when handoff and timeout race. */
    int          wait_status;
    struct br_thread *wait_next;/* wait chain: shared by mutex/sem/cond/join */
    struct br_thread *all_next; /* global TCB chain (traversal/observation) */
    br_u64       ctx[BR_THREAD_CTX_WORDS];
    /* Plugin-private tail starts here (ops.tcb_size - sizeof(struct br_thread)). */
};

/* Save from's callee-saved + sp, restore to's, switch stacks. See sched_ctx.h for the ctx
 * layout. */
void br_sched_switch_to(struct br_thread *from, struct br_thread *to);

/* First entry of a new thread (written into ctx[x30] at create); x19 holds the TCB pointer. */
BR_NORETURN void br_sched_thread_trampoline(void);

/* C half of the trampoline: calls entry(arg), and on return br_task_exit(0) (does not return). */
BR_NORETURN void br_sched_thread_entry(struct br_thread *t);

/*
 * Wait chain: FIFO nodes live inside threads -- push appends to the tail, pop unlinks the head.
 * Zero allocation, so the sync path cannot fail on allocation.
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

/* Registered scheduler ops, BR_NULL when unregistered; read-only, for the selftest evidence
 * line. */
const br_sched_ops_t *br_sched_ops_get(void);

#endif /* BR_SCHED_INTERNAL_H */
