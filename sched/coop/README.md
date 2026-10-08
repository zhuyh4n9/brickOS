# sched/coop

> **协作式调度器(coop)**: FIFO 就绪队列 + 只在**显式点**换栈。core 拥有语义(线程状态机 /
> 超时唤醒 / idle / 上下文切换 asm), 本插件只回答"下一个该跑谁"。
> 声明面 `plugin.toml` 是唯一真值; 本 README 只写实现者要记住的东西。

## 能力面(填给 core 的 `br_sched_ops_t`, 见 `include/br/sched/coop.h`)

| ops 槽位 | 本插件的实现 | 说明 |
|---|---|---|
| `name` | `"sched/coop"` | 呈现用 |
| `kind` | `BR_SCHED_KIND_COOP` | 与 `[plugin].sched_kind = "coop"` 对应 |
| `tcb_size` | `br_sched_tcb_size() + sizeof(struct coop_priv)` | 私有尾 = 1 个 `ready_next` 指针(8 B) |
| `thread_ready` | FIFO 入队(尾插) | 在 `br_irq_lock()` 的 L2 临界区里(ISR 也会调它) |
| `thread_block` | 防御性线性摘除 | coop 下 RUNNING 者本就不在队列里, 通常是空操作 |
| `pick_next` | 出队; 空 ⇒ `BR_NULL` | **不改 `state`** —— 置 RUNNING 是 core 的事 |
| `on_tick` | `BR_NULL` | coop 不做时间片轮转; 无 tick 策略 |
| `sleep_until` | 返回 0(接受登记) | 超时唤醒由 core 的 `wake_at` 表统一负责 |
| `idle` | `BR_NULL` | 缺省走 core 的"关中断 → 检查 → WFI → 开中断"(3-02 §11.1) |

生命周期回调:

| 函数 | 相位 | 做什么 |
|---|---|---|
| `coop_early_init()` | EARLY | 填 ops 表 + `br_sched_register()`(恰一次) |
| `coop_init()` | CORE | 无动作(注册已在 EARLY 完成) |
| `coop_start()` | start | 无动作(进入调度循环由插件管理器的 `br_sched_run()` 做) |

## coop 的语义边界(反直觉但**是设计内行为**)

- **无抢占**: 只在 `br_task_yield()` / 阻塞 / 退出 / 首次调度这几个显式点换栈。
  一个不 yield 的忙循环线程**不会被切走** —— `TC-TASK-101` 就是这条的机械判据
  (它同时是"coop 与 preempt 的分界": 换成 preempt 调度器, 这条用例应当变红)。
- **中断返回永远回到被打断的线程**(`3-02` §11.3): 即使 ISR 里 `br_sem_give()` 唤醒了一个
  更高优先级的线程, 它也要等到下一次显式调度点才运行。⇒ ISR → 线程的**延迟没有上界承诺**。
- **`yield` = 强制切换点**(`3-01` §2.1): 让出者回就绪队尾 ⇒ FIFO 天然 round-robin,
  不需要时间片; 但"同级公平"完全依赖各线程自觉让出。
- `priority` 在 coop 下**被忽略**(`3-01` §2.1: 属性是建议, 语义归调度插件)。

## 与 core 的接缝(为什么这么分)

- TCB 的**公共头**在 `core/src/sched/sched_internal.h`(冻结布局): 状态/名字/栈/退出码/
  `wait_status`/`wait_next`/`ctx[13]`。插件只拿不透明指针 + `br_thread_priv()` 的私有尾。
- **ICB 握手**(ADR-0006 §3b): `wait_status` 是唯一的唤醒/超时赢家位。本插件不碰它 ——
  只负责"BLOCKED 的人被唤醒后回到队列", 握手由 core 的 `br_sched_wake` / 超时扫描做。
- `br_sched_irq_epilogue()` 在 coop 下**只扫超时表、不切栈**(`3-02` §11.2: Stage 1 的空接缝)。

## 诚实声明(没做的部分, 与 ADR-0006 §5 一致)

| 设计里的东西 | 本原型 |
|---|---|
| tickless("按最近期限装弹比较器") | ❌ 只做**周期 tick(100 ms)上的到期扫描** —— 分辨率就是 tick 周期 |
| `sched-preempt`(v2.0) / PI 互斥 | ❌ 未做 |
| `sched-tt`(v3.0, 调度表) | ❌ 未做 |
| `br_work_submit` / bh | ❌ 未做(`3-01` §5 属后续刀) |
| SMP(per-CPU 队列 / IPI) | ❌ 未做(单核; `current` 是 core 静态量) |

## 设计出处

- `Design/docs/3-os-core/3-03-sched.md`(总纲; §2 的待成文清单由 ADR-0006 成文)
- `Design/docs/3-os-core/3-01-core-api-list.md` §5.1 / §6 / §14(CA-1/CA-4)
- `Design/docs/3-os-core/3-02-int.md` §11.1–§11.3(IRQ ↔ 调度接缝; idle 的 L2 临界区处方)
- `Design/docs/1-architecture/1-01-architecture.md` §9(启动序列)
- `Design/docs/6-test/6-01-test.md` §3.1/§3.3(TC-TASK-* / TC-TIME-*)
- `docs/decisions/0006-scheduler-framework.md`(本原型的逐条裁定)
