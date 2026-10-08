/*
 * sched/coop — 对外声明面(人写; 能力面 = `br_sched_ops_t` 的一份实现)
 *
 * 本插件的"对外面"不是一组新函数, 而是**填给 core 的调度器 ops 表**(设计 `3-01` §5.1:
 * "core 拥有语义, 插件拥有策略")。所以这里只声明三个生命周期回调 + ops 的注册点,
 * 真正的策略在 `src/coop.c`。
 *
 * 设计依据: `3-03-sched.md`(调度总纲)、`3-01` §5.1/§6、`3-02` §11.3(coop 的中断-调度语义)、
 *           `docs/decisions/0006-scheduler-framework.md`(ops 成文表 + 逐条裁定)。
 *
 * COOP 的语义(一句话): 只在**显式点**(yield / 阻塞 / 退出 / 首次调度)换栈;
 * 中断返回永远回到被打断的线程(即使 ISR 唤醒了别的线程)—— 这是设计内行为, 不是 bug。
 */
#ifndef BR_SCHED_COOP_H
#define BR_SCHED_COOP_H

#include <br/core/br_sched.h>
#include <br/core/br_types.h>

/* EARLY 相: 填 ops 表并 `br_sched_register()`(注册恰一次; 3-01 §5.1)。
 * ★ 注册会把"调用者所在的启动上下文"物质化成 main 线程(ADR-0006 §1 的 I1),
 *   所以 run 之前的 create/join/yield 立刻可用。 */
int coop_early_init(void);

/* CORE 相: 无动作 —— 注册已在 EARLY 完成(调度器必须在任何线程创建之前就位)。 */
int coop_init(void);

/* start 相: 无动作 —— 全局开中断与"进入调度循环(`br_sched_run`)"由插件管理器做。 */
int coop_start(void);

#endif /* BR_SCHED_COOP_H */
