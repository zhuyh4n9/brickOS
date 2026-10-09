/*
 * sched/rr — 对外声明面(人写; 能力面 = `br_sched_ops_t` 的一份实现)
 *
 * 本插件的"对外面"不是一组新函数, 而是**填给 core 的调度器 ops 表**(设计 `3-01` §5.1:
 * "core 拥有语义, 插件拥有策略")。所以这里只声明三个生命周期回调, 真正的策略在 `src/rr.c`。
 *
 * 设计依据: `3-03-sched.md`(调度总纲)、`3-01` §5.1/§2.1、`3-02` §11.2/§11.3、
 *           `docs/decisions/0011-round-robin-bh-workqueue.md`(抢占接缝的成文表 + 逐条裁定)。
 *
 * RR 的语义(一句话): **在显式点之外, 每个 tick 也会换栈**。当前线程的时间片用尽 ⇒
 * 排到就绪队尾 + core 在 IRQ 出口换到下一个同优先级线程; 没有优先级, 没有 PI。
 */
#ifndef BR_SCHED_RR_H
#define BR_SCHED_RR_H

#include <br/core/br_sched.h>
#include <br/core/br_types.h>

/* 时间片: 以**平台 tick** 为单位(本原型 tick = 100 ms; 见 platform 的
 * BR_BOARD_TIMER_PERIOD_US)。取 2 ⇒ 一个线程连续跑 200 ms 后被轮换。
 * 为什么用 tick 而不是微秒: 递减发生在 `on_tick` 里, tick 就是它的自然粒度 ——
 * 用微秒只会逼出一个"读时钟做除法"的热路径, 而精度不会更好。 */
#define RR_SLICE_TICKS 2u

/* EARLY 相: 填 ops 表并 `br_sched_register()`(注册恰一次; 3-01 §5.1)。
 * ★ 注册会把"调用者所在的启动上下文"物质化成 main 线程(ADR-0006 §1 的 I1),
 *   所以 run 之前的 create/join/yield 立刻可用。 */
int rr_early_init(void);

/* CORE 相: 无动作 —— 注册已在 EARLY 完成(调度器必须在任何线程创建之前就位)。 */
int rr_init(void);

/* start 相: 无动作 —— 全局开中断与"进入调度循环(`br_sched_run`)"由插件管理器做。 */
int rr_start(void);

#endif /* BR_SCHED_RR_H */
