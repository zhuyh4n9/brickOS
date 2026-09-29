# 10 — 调度框架(schedule)

> 分类: **2-os-core**。状态: **骨架**。
> 来源: 主文档 §5(调度框架与 sched_class); API: `docs/2-os-core/08-core-api-list.md` §2–§5; 版本排序: `docs/1-architecture/02-roadmap.md` §2。

## 1. 范围与现状

**语义在 core, 策略在插件**: core 拥有上下文切换 asm、tickless 超时框架、锁 API 契约; 调度策略是插件(恰一个)。

已有决策(待深化时继承):
- **演进序**(D2, 用户定序): sched-coop v1.0 → sched-preempt v2.0(+SMP, PI 互斥) → sched-tt v3.0(时间触发表)
- **sched_class 声明**(D10): SAFE_PREEMPT / COOP_ONLY / TT_SAFE——插件声明 + 组合期校验 + 静态分析(D10 双保险)+ conformance 矩阵(R1 执法)
- **锁退化**(§5.2): coop 下 mutex ≈ irq 锁包装——同一 API, 不同成本
- **锁对象布局**(CA-2): core 公共头 + 调度插件私有尾, `TG_*_DEFINE` 静态宏
- **超时框架**: tickless, 绝对期限(`tg_deadline_from_now`), 平台 timer 供节拍

## 2. 大纲(待成文)

1. **`tg_sched_ops` 完整规格**(DoD 第 3 项): 全字段表 + 三形态差异矩阵(coop/preempt/tt × ops 槽位语义)
2. 上下文切换: aarch64 asm 桩、callee-saved 约定、栈切换、`tg_task_yield` 语义(coop=强制切换点 / preempt=提示)
3. tickless 超时队列: 绝对期限、平台 timer 事件注入、idle 循环(WFI)
4. 锁语义矩阵: mutex/sem/cond × 三调度器的实现差异与成本
5. work queue(bh)实现归属: 调度插件(08 §5)
6. SMP(v2b): per-CPU 就绪队列、IPI、锁核间语义——独立分期(R8: v2b 单独隔离)
7. 时间触发(sched-tt, v3.0): 调度表格式、与 CAN/仪表场景的对接

## 3. 开放问题

| # | 问题 |
|---|---|
| — | 抢占点粒度: preempt 是否需要显式 `TG_PREEMPT_POINT()` 宏(长无阻塞计算段/长临界路径) |
| — | 优先级范围与反转策略(preempt: PI; coop: N/A; tt: 表驱动) |
| — | idle 功耗路径: WFI 与 v2 PM(O-S6)的衔接 |

## 4. DoD 关联

`docs/1-architecture/02-roadmap.md` §5 第 3 项: **tg_sched_ops 完整规格**(三形态差异矩阵)——本篇成文的核心交付。
