/*
 * Machine-visible constants of the context save area: the single truth bridge between asm and C.
 * switch.S needs the byte offset of ctx[] and every slot index; sched_core.c checks these macros
 * against the real layout with _Static_assert, so layout drift fails the build instead of the
 * stack switch.
 * Slots: [0..9] x19..x28, [10] x29(fp), [11] x30(lr; new thread = trampoline), [12] sp.
 */
#ifndef BR_SCHED_CTX_H
#define BR_SCHED_CTX_H

/* `offsetof(struct br_thread, ctx)`: enforced by a static assertion in sched_core.c. */
#define BR_THREAD_CTX_OFF 96u

/* ctx[] slot indices */
#define BR_CTX_X19_BASE 0u
#define BR_CTX_X29      10u
#define BR_CTX_X30      11u
#define BR_CTX_SP       12u

/* On first entry x19 holds the thread's TCB pointer (read by the trampoline); alias of x19. */
#define BR_CTX_X19_TCB  BR_CTX_X19_BASE

#endif /* BR_SCHED_CTX_H */
