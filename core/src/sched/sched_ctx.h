/*
 * brickOS prototype v0.2.0 — **machine-visible** constants of the context save area
 * (the single truth bridge between asm and C)
 *
 * Why a separate header: `sched_internal.h`'s `struct br_thread` is C syntax, which the assembler
 * (gas after cpp) cannot read, while `switch.S` must know the byte offset of `ctx[]` and the index
 * of each slot. Copying the numbers again into the .S file would create a second truth — and once
 * the layout drifts (say someone inserts a field in the middle of a public header), it surfaces
 * only after the stack switch as "random crashes". Here the offsets are macros, checked against
 * the real layout at **compile time** by `sched_core.c`'s `_Static_assert` (layout drift = a red
 * build, not a runtime crash).
 *
 * Slot semantics (word-for-word identical to the `ctx[13]` comment in `sched_internal.h`):
 *   [0..9]  x19..x28
 *   [10]    x29(fp)   —— `-fno-omit-frame-pointer` is the premise of backtrace, and vice versa
 *   [11]    x30(lr)   —— points at the trampoline on a new thread's first entry
 *   [12]    sp        —— the thread's own stack
 */
#ifndef BR_SCHED_CTX_H
#define BR_SCHED_CTX_H

/* `offsetof(struct br_thread, ctx)`: enforced by a static assertion in sched_core.c. */
#define BR_THREAD_CTX_OFF 96u

/* ctx[] slot indices */
#define BR_CTX_X19_BASE 0u    /* [0..9] = x19..x28 */
#define BR_CTX_X29      10u   /* fp */
#define BR_CTX_X30      11u   /* lr; new thread = trampoline */
#define BR_CTX_SP       12u   /* sp */

/* On a new thread's first entry x19 holds its TCB pointer (read by the trampoline); same slot as x19. */
#define BR_CTX_X19_TCB  BR_CTX_X19_BASE

#endif /* BR_SCHED_CTX_H */
