/*
 * brickOS prototype v0.2.0 — coop context switch (aarch64)
 *
 * Design basis: `3-03` §2 item 2 (context switch: aarch64 asm stub, callee-saved convention, stack switch,
 * first-entry trampoline); `3-01` §2.2 ("the initial stack frame is built by create, entry goes through the
 * trampoline").
 *
 * Two entries:
 *   br_sched_switch_to(from, to) -- save from's callee-saved + sp, restore to's, switch stacks.
 *   br_sched_thread_trampoline() -- the **first** entry of a new thread (initial x30 value); it hands the TCB in
 *                                   x19 to the C-side br_sched_thread_entry(), which calls entry(arg); if entry
 *                                   returns, br_task_exit(0).
 *
 * Why only callee-saved (AAPCS64): caller-saved registers are handled by the compiler around `bl`; cooperative
 * switching only happens at function call boundaries (yield/block/exit are all ordinary C calls), so 13 words
 * suffice. This is the same source of truth as the ctx[13] layout in `sched_internal.h` (offsets and indices in
 * sched_ctx.h).
 *
 * This file is preprocessed by cpp (the `.S` suffix makes GCC do that automatically), so `#if`/`#include` work.
 */
#if defined(__aarch64__)

#include "sched_ctx.h"

    .text
    .align  2

/* ---------------------------------------------------------------------------
 * void br_sched_switch_to(struct br_thread *from, struct br_thread *to);
 *   calling convention: x0 = from, x1 = to.
 *
 * Note: a new thread's ctx is pre-filled by br_task_create (x19 = TCB, x30 = trampoline,
 *       sp = stack top), so this function need not distinguish "new/old" -- restoring is the first entry.
 * ------------------------------------------------------------------------- */
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

    ret                                     /* old thread = back after the bl; new thread = enter trampoline */
    .size   br_sched_switch_to, . - br_sched_switch_to

/* ---------------------------------------------------------------------------
 * First entry of a new thread. Convention: on entry x19 = that thread's TCB (written into ctx[0] by create).
 * We do not ldr entry/arg here -- field offsets keep a single source of truth (the C side), so this stays
 * one mov + one bl and does not drift with the TCB layout.
 * ------------------------------------------------------------------------- */
    .globl  br_sched_thread_trampoline
    .type   br_sched_thread_trampoline, %function
br_sched_thread_trampoline:
    mov     x0, x19
    bl      br_sched_thread_entry           /* C: entry(arg); on return br_task_exit(0) -- does not return */
    /* Safety net: if the C side ever breaks the "does not return" contract, never fall into a wild address. */
    b       br_sched_thread_trampoline
    .size   br_sched_thread_trampoline, . - br_sched_thread_trampoline

#endif /* __aarch64__ */
