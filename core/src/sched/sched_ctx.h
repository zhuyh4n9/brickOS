/*
 * brickOS prototype v0.2.0 — 上下文保存区的**机器可见**常量(asm ↔ C 的唯一真值桥)
 *
 * 为什么单独一个头: `sched_internal.h` 的 `struct br_thread` 是 C 语法, 汇编器(cpp 之后的
 * gas)看不懂; 而 `switch.S` 又必须知道 `ctx[]` 的字节偏移与各槽位下标。若在 .S 里再抄一份
 * 数字, 就有了第二处真值 —— 布局一漂(比如有人往公共头中间插字段), 换栈之后才会以
 * "随机崩溃"暴露。这里把偏移写成宏, 由 `sched_core.c` 的 `_Static_assert` 在**编译期**
 * 与真布局对照(布局漂移 = 编译红, 不是运行期崩)。
 *
 * 槽位语义(与 `sched_internal.h` 的 `ctx[13]` 注释**逐字一致**):
 *   [0..9]  x19..x28
 *   [10]    x29(fp)   —— `-fno-omit-frame-pointer` 是 backtrace 的前提, 也是它的前提
 *   [11]    x30(lr)   —— 新线程首次进入时指向 trampoline
 *   [12]    sp        —— 线程自己的栈
 */
#ifndef BR_SCHED_CTX_H
#define BR_SCHED_CTX_H

/* `offsetof(struct br_thread, ctx)`: 由 sched_core.c 静态断言执法。 */
#define BR_THREAD_CTX_OFF 96u

/* ctx[] 的槽位下标 */
#define BR_CTX_X19_BASE 0u    /* [0..9] = x19..x28 */
#define BR_CTX_X29      10u   /* fp */
#define BR_CTX_X30      11u   /* lr; 新线程 = trampoline */
#define BR_CTX_SP       12u   /* sp */

/* 新线程首次进入时 x19 里放的就是它的 TCB 指针(trampoline 读它); 与 x19 同槽。 */
#define BR_CTX_X19_TCB  BR_CTX_X19_BASE

#endif /* BR_SCHED_CTX_H */
