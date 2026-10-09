# sched/rr

> **时间片轮转(round-robin)抢占调度器**: FIFO 就绪队列 + **每线程时间片**。core 拥有语义
> (线程状态机 / 超时唤醒 / idle / 上下文切换 asm / **IRQ 出口的换栈**), 本插件只回答
> "下一个该跑谁"和"什么时候该换"。声明面 `plugin.toml` 是唯一真值; 本 README 只写实现者
> 要记住的东西。

## 与 `sched/coop` 的关系(唯一差别就是时间片)

| | `sched/coop` | `sched/rr` |
|---|---|---|
| 就绪结构 | FIFO 单链 | FIFO 单链(同一形态) |
| 换栈点 | yield / 阻塞 / 退出 / 首次调度 | 上面这些 **+ 每个 tick 的时间片用尽** |
| `on_tick` | `BR_NULL`(无 tick 策略) | 递减时间片, 归零即 `br_sched_request_resched()` |
| 不 yield 的忙循环 | **不会被切走**(设计内行为, TC-TASK-101) | **会被切走**(TC-TASK-102) |
| 中断返回 | 永远回到被打断的线程 | 若同时有待跑的线程, 出口即可能换到它 |
| `sched_kind` | `coop` | `preempt` |

⇒ 本插件是"同一套 core 机制换一个策略"的**证据**: 两件插件加起来 ~150 行插件代码,
core 里没有一处 `if (调度器是 rr)`。

## 能力面(填给 core 的 `br_sched_ops_t`)

| ops 槽位 | 本插件的实现 | 说明 |
|---|---|---|
| `name` | `"sched/rr"` | 呈现用 |
| `kind` | `BR_SCHED_KIND_PREEMPT` | 与 `[plugin].sched_kind = "preempt"` 对应 |
| `tcb_size` | `br_sched_tcb_size() + sizeof(struct rr_priv)` = 216 B | 私有尾 = `ready_next` + `slice_left` + `turns`(16 B) |
| `thread_ready` | FIFO 入队(尾插), **不动时间片** | 在 `br_irq_lock()` 的 L2 临界区里(ISR 也会调它) |
| `thread_block` | 防御性线性摘除 | RR 下 RUNNING 者本就不在队列里 |
| `pick_next` | 出队 **+ 重新装载时间片** | **不改 `state`** —— 置 RUNNING 是 core 的事(I2) |
| `on_tick` | 递减当前线程的时间片; 归零 ⇒ `br_sched_request_resched()` | 本插件全部的"抢占策略" |
| `sleep_until` | 返回 0(接受登记) | 超时唤醒由 core 的 `wake_at` 表统一负责 |
| `idle` | `BR_NULL` | 缺省走 core 的"关中断 → 检查 → WFI → 开中断"(3-02 §11.1) |

时间片粒度: `RR_SLICE_TICKS = 2` 个**平台 tick**(本原型 tick = 100 ms)⇒ 一个线程连续跑
200 ms 后被轮换。装载点选在 **`pick_next`**(被选中)而不是 `thread_ready`(进入就绪):
否则"每 yield 一次就白送一个整片"。

## core 侧的接缝(为什么本插件里没有一行切换代码)

```
timer ISR ──br_sched_on_tick──▶ scan_timeouts + rr_on_tick
                                         │  时间片归零
                                         ▼
                              br_sched_request_resched()      ← 只置一个位
                                         │
   IRQ 出口(eoi 之后、ERET 之前) ────────▶ br_sched_irq_epilogue()
                                         │  need_resched?
                                         ▼
              resched_from_irq(): 当前线程回队尾 → pick_next → br_sched_switch_to
```

* **"什么时候换"归插件**(时间片), **"怎么换"归 core**(state 机 + 异常出口顺序 + asm)。
* 被抢占线程的现场完整地留在**它自己的 IRQ 出口调用链**上(异常帧在它自己的栈里)——
  它被再次选中时从 `br_sched_switch_to` 返回, 走完 IRQ 出口并 ERET 回被打断处。
  所以 `switch.S` 只存 callee-saved 依然够, `vectors.S` 不必改。
* 新线程若是在 IRQ 出口被选中的(`PSTATE.I == 1`), 它的首次进入由 core 在
  `br_sched_thread_entry` 里把中断放行(ADR-0011 裁定 P-1)。

## 契约与边界

* **`prio` 仍被忽略**: 本件是"**同优先级**轮转"; 优先级/PI 属将来的 `sched-preempt`。
* **时间片的粒度 = tick 周期**(100 ms): 一个刚被唤醒的高优先级线程最多等一个时间片
  (≤200 ms); 需要更低延迟就用 `sched-preempt`(v2)。
* **IRQ 出口换栈与 bh 的顺序**: IRQ 出口先跑下半部(`br_work_drain`), 再做抢占决策
  (`br_sched_irq_epilogue`)⇒ bh 里唤醒的线程能被**同一次出口**的抢占看到。
* **不等于实时**: 没有 WCET 承诺; `TT_SAFE` 类插件的周期保证要等 `sched-tt`。

## 从 1 到 0 的覆盖变化(诚实条款, ADR-0011 §5)

本原型一次只能构建**一个**产品镜像, 而 `br_sched_register()` 二次注册 = panic ⇒
镜像里只能装一个调度器。ADR-0011 的裁定是**镜像选 `sched/rr`**(因为"IRQ 出口换栈"
是这一刀真正的新机制, 必须在目标上验证), 于是:

* `sched/rr` —— QEMU 门禁 `sched-test` + 宿主 `rr-test`;
* `sched/coop` —— **留在树里**、`brickie check` 照旧校验声明面、宿主 `sched-test` 覆盖
  它的全部分界(含 `TC-TASK-101`), 但**不再进 QEMU 镜像**。要换回去只需改
  `product.toml [select].plugins` 里的一行。

## 设计出处

- `Design/docs/3-os-core/3-03-sched.md`(总纲)
- `Design/docs/3-os-core/3-01-core-api-list.md` §2.1 / §5.1 / §14(CA-1/CA-4)
- `Design/docs/3-os-core/3-02-int.md` §11.1–§11.4(IRQ ↔ 调度接缝)
- `docs/decisions/0006-scheduler-framework.md`(coop 与调度框架的成文表)
- `docs/decisions/0011-round-robin-bh-workqueue.md`(本刀的逐条裁定与证据)
