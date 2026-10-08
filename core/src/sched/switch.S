/*
 * brickOS prototype v0.2.0 — coop 上下文切换(aarch64)
 *
 * 设计依据: `3-03` §2 第 2 项(上下文切换: aarch64 asm 桩、callee-saved 约定、栈切换、
 * 首次进入的 trampoline); `3-01` §2.2("初始栈帧由 create 构造, entry 经 trampoline")。
 *
 * 两个入口:
 *   br_sched_switch_to(from, to) —— 保存 from 的 callee-saved + sp, 恢复 to 的, 换栈。
 *   br_sched_thread_trampoline() —— 新线程的**首次**入口(x30 初值); 它把 x19 里的 TCB
 *                                   交给 C 侧 br_sched_thread_entry(), 由后者调
 *                                   entry(arg), entry 返回则 br_task_exit(0)。
 *
 * 为什么只存 callee-saved(AAPCS64): 调用者保存的寄存器由编译器在 `bl` 前后自己处理;
 * 协作式切换只发生在函数调用边界(yield/block/exit 都是普通 C 调用), 所以 13 个字足够。
 * 这与 `sched_internal.h` 的 ctx[13] 布局是同一处真值(偏移与下标在 sched_ctx.h)。
 *
 * 本文件由 cpp 预处理(`.S` 的后缀让 GCC 自动这么做), 所以能用 `#if`/`#include`。
 */
#if defined(__aarch64__)

#include "sched_ctx.h"

    .text
    .align  2

/* ---------------------------------------------------------------------------
 * void br_sched_switch_to(struct br_thread *from, struct br_thread *to);
 *   调用约定: x0 = from, x1 = to。
 *
 * 注意: 新线程的 ctx 由 br_task_create 预先填好(x19 = TCB, x30 = trampoline,
 *       sp = 栈顶), 所以本函数不需要区分"新/旧"—— 恢复即首次进入。
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

    ret                                     /* 旧线程 = 回到 bl 之后; 新线程 = 进 trampoline */
    .size   br_sched_switch_to, . - br_sched_switch_to

/* ---------------------------------------------------------------------------
 * 新线程的首次入口。约定: 进入时 x19 = 该线程的 TCB(create 时写进 ctx[0])。
 * 我们不在这里直接 ldr entry/arg —— 字段偏移留一处真值(C 侧), 于是这里只有
 * 一条 mov + 一条 bl, 不会随 TCB 布局漂移。
 * ------------------------------------------------------------------------- */
    .globl  br_sched_thread_trampoline
    .type   br_sched_thread_trampoline, %function
br_sched_thread_trampoline:
    mov     x0, x19
    bl      br_sched_thread_entry           /* C: entry(arg); 返回则 br_task_exit(0) —— 不返回 */
    /* 兜底: 万一 C 侧违反了"不返回"的约定, 也绝不落进野地址。 */
    b       br_sched_thread_trampoline
    .size   br_sched_thread_trampoline, . - br_sched_thread_trampoline

#endif /* __aarch64__ */
