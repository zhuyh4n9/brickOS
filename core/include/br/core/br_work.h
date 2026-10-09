/*
 * brickOS prototype v0.2.0 — 延迟工作(bottom half / workqueue, core): ISR → bh 的唯一正规路径
 *
 * 设计依据: `3-01-core-api-list.md` §5("延迟工作(br-sched 组; bottom half)"),
 *           §1 的 API 总览(`br_work_submit` 在 ISR-safe 白名单里)、§11 CA-3;
 *           `3-02-int.md` §11.1(路径一: ISR → 延迟工作)、§11.4.1(`br_irq_disable` 在
 *           BH 形态下的语义: 抑制而不取消、不重放)、§12.1/§12.4(分发形态与执行时机)、
 *           §9.4(SLOW 级联域的 demux 走 bh)、IR-10(提交失败必须回滚)。
 *           逐条裁定: `docs/decisions/0011-round-robin-bh-workqueue.md`。
 *
 * ## 归属(与本仓其它"机制/策略"切法一致)
 *
 * 设计 `3-01` §5 把 work queue 的**实现**放在调度插件里(因为 bh 的"何时跑"是策略)。
 * 本原型按用户裁定把 **workqueue 整体放在 core**:
 *   - 队列本体(有界静态环)与 `br_work_submit` 是**机制**(热路径、ISR 里调用);
 *   - 执行时机 = **IRQ 出口**(eoi 之后、ERET 之前), 因此它不依赖调度器是否注册 ——
 *     这与 `br_sched_irq_epilogue()` 的独立性同源(§11.2 的接缝在 Stage 1 就必须存在)。
 * 代价(如实登记): 设计 §11.3 的"ISR → bh 延迟没有上界承诺"在本原型**不成立** ——
 * 我们给出的是**有界**延迟(下一个中断出口), 见 `br_work_submit` 的注释与 ADR-0011 §3。
 *
 * ## bh 上下文是什么(一句话)
 *   "已经 eoi、还没 ERET"的那一小段: 在**被中断线程自己的栈**上、`PSTATE.I == 1`、
 *   `br_irq_in_isr() == 0`(ISR 已返回)、`br_work_in_bh() == 1`。
 *   由此**推论(契约)**:
 *     - bh 体内**禁止**阻塞(`br_sched_block_current` 直接 `-EPERM`)、禁止让出
 *       (`br_task_yield` 拒绝并留痕)、禁止结束线程(panic) —— 它没有可回退的调度上下文;
 *     - bh 体内**禁止**取会长期持有的锁(它是"关中断"的), 工作项必须短(§9.4 的 bh 契约);
 *     - 阻塞类/配置类 native API 在 bh 内一律 `-EPERM`/`-EINVAL`(运行期守卫)。
 *   这是一次**收紧**(比设计的"线程上下文 bh 契约"更严): 换来的是"不引入 worker 线程、
 *   不依赖调度器、延迟有界"三件事, 且 SLOW 域的 demux 仍满足"状态寄存器只能在非 ISR
 *   上下文读"这条真正的硬要求。
 */
#ifndef BR_CORE_BR_WORK_H
#define BR_CORE_BR_WORK_H

#include <br/core/br_types.h>

/*
 * 队列深度(编译期静态上界; 设计 `3-01` §5: "队列深度 = manifest 静态声明(资源预算)"。
 * 本原型按 4 KiB 页口径取 16 —— 16 × (fn,arg) = 256 B, 与其它静态池同量级)。
 * 满 ⇒ `br_work_submit` 返回 `-EAGAIN`(**绝不静默丢弃**: 调用方必须处理, 见 IR-10)。
 */
#define BR_WORKQ_DEPTH 16u

/*
 * 单次 IRQ 出口的执行上限(工作项条数)。
 * 为什么必须有上限: drain 发生在**关中断**的 IRQ 出口 ⇒ 无上限就等于"一条中断可以
 * 把中断延迟拉成任意长"。取 4 使"最坏 IRQ 出口增量"= 4 个工作项;
 * 余下的留给**下一次中断出口**(本原型 tick = 100 ms, 因此"队列排空"的上界是
 * `ceil(待处理数 / 4) × tick`)。
 */
#define BR_WORK_BH_BUDGET 4u

/* 工作项函数形态(设计 §5: v1 无 work 对象/cancel —— fn + arg 裸提交)。 */
typedef void (*br_work_fn_t)(void *arg);

/*
 * 提交一个工作项: `fn(arg)` 将在 **bh 上下文**执行一次。
 *
 * **ISR-safe**(CA-3 的白名单第一件: 只取 L2 临界区 + 一次槽位写)。
 * 返回: 0 = 已入队; `-EINVAL` = fn 为 NULL; `-EAGAIN` = 队列满(**必须处理的唯一失败**)。
 *
 * ⚠ 调用方义务(IR-10): 若这次提交是"某个中断事件的唯一载体"(如 SLOW 域的 demux),
 *   `-EAGAIN` 必须**回滚**已经做的动作(清 busy / 放行父线), 否则该事件永久静默 ——
 *   这是本机制唯一的死锁入口。`irq_core.c` / `irq_domain.c` 的两处调用点都这么做。
 *
 * ⚠ 执行时机: 下一个中断出口(而不是"立刻")。线程上下文提交 ⇒ 上界 = 一个 tick。
 *   自检/宿主可以显式调 `br_work_drain()` 立刻跑(见 `core/selftest/work_selftest.c`)。
 */
int br_work_submit(br_work_fn_t fn, void *arg);

/*
 * 取出并执行至多 `max_items` 项, 返回实际执行数。
 *
 * 由 core 的 IRQ 出口调用(`irq_core.c`, 传 `BR_WORK_BH_BUDGET`); 也是宿主/自检的
 * 显式驱动点。**非重入**: 已经在 bh 里(某个工作项又调它)⇒ 返回 0(由外层的循环继续),
 * 这样"工作项里再提交"不会把栈吃掉。
 */
br_u32 br_work_drain(br_u32 max_items);

/* 当前是否在 bh 上下文(工作项体内为 `BR_TRUE`)。供守卫与用例判定。 */
br_bool br_work_in_bh(void);

/* 观测快照(非热路径; 与 br_irq_stat_t 同型: 只作诊断, 不进 golden 布局承诺)。 */
typedef struct br_work_stat {
    br_u32 submitted;      /* 累计入队成功次数 */
    br_u32 ran;            /* 累计执行次数 */
    br_u32 dropped_full;   /* 队列满被拒的次数(-EAGAIN) */
    br_u32 dropped_invalid;/* fn == NULL 被拒的次数(-EINVAL) */
    br_u32 pending;        /* 当前待执行条数 */
    br_u32 max_pending;    /* 历史最大待执行条数(队列深度是否够用的证据) */
    br_u32 drain_passes;   /* drain 被调用的次数 */
} br_work_stat_t;

void br_work_stats_get(br_work_stat_t *out);

/*
 * 这里**不再**声明工作队列的"套件入口": 与 `br_sched.h`/`br_sync.h` 同源(ADR-0010)——
 * 自检入口不是插件的对外能力, 声明集中在 `core/selftest/core_selftest.c`。
 */

#endif /* BR_CORE_BR_WORK_H */
