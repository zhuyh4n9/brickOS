# 09 — 中断管理(int 框架)

> 分类: **2-os-core**。状态: **骨架**(范围/大纲已定, 待深化)。
> 来源: 主文档 §4.2(int 框架)、§8(PIC = platform 三层模式); API: `docs/2-os-core/08-core-api-list.md` §8/§11。

## 1. 范围与现状

core 拥有 **IRQ 框架**(策略外最小的机制); 中断控制器实现归 Platform 插件(PIC ops 填表), 驱动只面对 `tg_irq_register`。

已有决策(待深化时继承):
- **ISR 契约**(主文档 §4.2): ISR 最小工作 → `tg_work_submit`; 禁阻塞/malloc/持锁跨返回; 静态扫描执法(D10 双保险之一)+ conformance 矩阵(R1 执法)
- **ISR-safe 白名单**(08 §11): `tg_work_submit` / `tg_sem_give` / `tg_irq_lock/unlock` + trace 宏, 其余 thread-only
- **bottom half**: `tg_work_submit` = ISR 的唯一正规延迟路径(实现在调度插件, 08 §5); 队列深度 manifest 静态
- **PIC 抽象**: prio/trigger 解释权在 platform(主文档 §8 三层模式); core 只传递

## 2. 大纲(待成文)

1. 向量与入口: aarch64 EL1 VBAR、异常向量布局、IRQ 入口 asm→C 桩
2. IRQ 注册表与使能: `tg_irq_register/enable/disable`(08 §8)、共享 IRQ(已定: v1 非目标——每根物理线恰一属主, 复用走级联域, 08 §8.1)
3. 临界区: `tg_irq_lock/unlock` 嵌套计数在 core; 与调度器内部锁/抢占禁止的关系(coop/preempt 下的差异)
4. bottom half: work queue 深度、满队列策略(-EAGAIN)、与 ISR 的交接时序
5. fault 路径(v2): `tg_fault_handler_register`(同步异常钩子)→ ramdump 挂接(`docs/4-debug/03-debug.md` §3)
6. SMP(v2b): per-CPU 中断、IPI、亲和性

## 3. 开放问题

| # | 问题 |
|---|---|
| **D8** | **中断线程化**(最后两个未落定决策之一, 与 D7 并列——主文档 §17): bh 之上是否再提供"线程化 IRQ"(Linux threaded IRQ 型)——仪表/CAN 高频中断场景; 与 sched_class 交互 |
| — | 共享 IRQ(已定: v1 非目标, 见 08 §8.1——每根物理线恰一属主, 复用走级联域) |
| — | softirq/tasklet 型第二级延迟层是否需要(倾向: 不需要, work queue 够用) |

## 4. DoD 关联

`docs/1-architecture/02-roadmap.md` §5: D8 落定是 README 触发条件之一; 本篇成文 = D8 的设计载体。
