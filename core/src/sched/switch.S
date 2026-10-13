/*
 * Coop context switch (aarch64). br_sched_switch_to(from, to) saves from's callee-saved + sp,
 * restores to's and switches stacks. br_sched_thread_trampoline() is a new thread's first entry
 * (initial x30): it hands the TCB in x19 to br_sched_thread_entry(), which calls entry(arg) and
 * exits. Only callee-saved are saved (AAPCS64) -- cooperating switches happen at C call
 * boundaries; ctx layout and offsets are in sched_ctx.h. Preprocessed by cpp (.S).
 */
#if defined(__aarch64__)

#include "sched_ctx.h"

    .text
    .align  2

/* void br_sched_switch_to(struct br_thread *from, struct br_thread *to); x0 = from, x1 = to.
 * A new thread's ctx is pre-filled by br_task_create (x19 = TCB, x30 = trampoline, sp = stack
 * top). */
    .globl  br_sched_switch_to
    .type   br_sched_switch_to, %function
br_sched_switch_to:
    add     x2, x0, #BR_THREAD_CTX_OFF      /* x2 = &from->ctx[0] */

    stp     x19, x20, [x2, #(BR_CTX_X19_BASE + 0) * 8]
    stp     x21, x22, [x2, #(BR_CTX_X19_BASE + 2) * 8]
    stp     x23, x24, [x2, #(BR_CTX_X19_BASE + 4) * 8]
    stp     x25, x26, [x2, #(BR_CTX_X19_BASE + 6) * 8]
    stp     x27, x28, [x2, #(BR_CTX_X19_BASE + 8) * 8]
    stp     x29, x30, [x2, #BR_CTX_X29 * 8]
    mov     x3, sp
    str     x3, [x2, #BR_CTX_SP * 8]

    add     x2, x1, #BR_THREAD_CTX_OFF      /* x2 = &to->ctx[0] */

    ldp     x19, x20, [x2, #(BR_CTX_X19_BASE + 0) * 8]
    ldp     x21, x22, [x2, #(BR_CTX_X19_BASE + 2) * 8]
    ldp     x23, x24, [x2, #(BR_CTX_X19_BASE + 4) * 8]
    ldp     x25, x26, [x2, #(BR_CTX_X19_BASE + 6) * 8]
    ldp     x27, x28, [x2, #(BR_CTX_X19_BASE + 8) * 8]
    ldp     x29, x30, [x2, #BR_CTX_X29 * 8]
    ldr     x3, [x2, #BR_CTX_SP * 8]
    mov     sp, x3

    ret                                     /* old thread: back after the bl; new thread: enter trampoline */
    .size   br_sched_switch_to, . - br_sched_switch_to

/* First entry of a new thread: x19 = its TCB (ctx[0], written by create). Entry/arg are not loaded
 * here, so field offsets keep one source of truth on the C side. */
    .globl  br_sched_thread_trampoline
    .type   br_sched_thread_trampoline, %function
br_sched_thread_trampoline:
    mov     x0, x19
    bl      br_sched_thread_entry           /* C: entry(arg); on return br_task_exit(0) -- does not return */
    /* Safety net: never fall into a wild address if "does not return" is broken. */
    b       br_sched_thread_trampoline
    .size   br_sched_thread_trampoline, . - br_sched_thread_trampoline

#endif /* __aarch64__ */
