# 0011 — round-robin 抢占调度器(`sched/rr`)+ 中断下半部(bh / workqueue)落地

> 状态: **已落地**(宿主 `sched-test` / `rr-test` / `work-test` 三条 + QEMU `smoke` /
> `irq-test` / `sched-test` 等七条门禁全绿 —— 证据见 §5)。
> 影响面(逐条见 §7):
> `core/src/sched/sched_core.c`(**抢占接缝 + bh 禁令 + 新线程首次进入的 L2 形态**)、
> `core/include/br/core/br_sched.h`(`br_sched_request_resched` / `br_sched_resched_pending`)、
> `core/src/work/work_core.c` + `core/include/br/core/br_work.h`(**新**: 有界队列与 bh 执行)、
> `core/src/irq/irq_core.c`(BH 分发 + §11.4.1 抑制 + IRQ 出口 drain + `br_irq_in_atomic`)、
> `core/src/irq/irq_domain.c`(**打开 SLOW 域**: 父线 ISR = 单飞 + mask + 提交 bh)、
> `core/src/irq/irq_pic.c`(绑定表的 `BR_IRQ_DISPATCH_BH` 折进描述符 flags)、
> `core/include/br/core/br_irq.h`(`br_irq_stat_t` 的 bh 三项 + 上下文查询面 + 文档口径)、
> `core/include/br/core/br_trace.h`(4 个新事件)、`core/src/trace.c`(名字表)、
> `core/selftest/{sched_selftest.c,work_selftest.c,core_selftest.c}`、
> `sched/rr/**`(**新插件**)、`sched/coop/{README.md,tests/smoke.toml}`(退役说明)、
> `platform/qemu-aarch64/{include/br/board_irq.h,src/board_irq.c,src/irq_conf.c}`
> (BH 直连线 + SLOW 父线 + 三条新用例)、
> `tests/host/{host_sched.c,host_sched.h,rr_test.c,work_test.c}`(新)+ `sched_test.c`(改用共享装置)、
> `tests/gates.toml`、`product.toml`(镜像换调度器)、
> `tools/brickie/rust/src/plan.rs`(`gen_total` 口径修正, 见 §3.7)
> 设计依据: `3-03`(调度总纲)、`3-01` §2/§5.1/§5/§11 CA-3/§13.6 P3、
> `3-02` §7.2/§9.3/§9.4/§11.1–§11.4/§12.1/§12.2/§12.4/IR-10/IR-12/IR-13、
> `6-01` §3.1/§3.3/§3.7、`1-01` §9、`1-03` M1
> 相关: ADR-0006(调度框架 + coop; 本刀**不改**它的任何结论, 只把"无抢占"这条**限定为 coop 的性质**)、
> ADR-0007(同步原语的 ICB 握手 —— bh 禁令复用它)、ADR-0010(自检套件与开关)

## 1. 本刀交付的三件东西

用户要求: **① prototype 增加 round robin 调度(插件); ② 支持中断下半部(core);
③ workqueue 机制**。三条在实现里落成:

| # | 要求 | 落点 | 一句话 |
|---|---|---|---|
| ① | RR 调度(插件) | `sched/rr/**` + core 的抢占接缝 | 时间片用尽 ⇒ **IRQ 出口换栈**(设计 §11.2 的接缝第一次真的切栈) |
| ② | 中断下半部(core) | `core/src/work/work_core.c` + `irq_core.c`/`irq_domain.c` | ISR 可把工作**推迟**到 `eoi` 之后、`ERET` 之前执行 |
| ③ | workqueue 机制 | `br_work_submit` / `br_work_drain` / `BR_IRQ_F_DISPATCH_BH` / SLOW 域 | 有界静态队列 + 三种消费者(按线的 BH、SLOW 域 demux、任意 ISR 的裸提交) |

**归属裁定(用户在本刀开头直接给出)**: workqueue **整体在 core**(队列 + `submit` +
bh 执行时机), 不再是"机制在插件"; 见 §3.1 的取舍与代价。

## 2. 分工(谁拥有什么)

| 面 | 归属 | 落点 |
|---|---|---|
| 上下文切换 asm / TCB 公共头 / 状态机 / 超时 / idle | **core**(不变, ADR-0006) | `sched_core.c` + `switch.S` |
| **IRQ 出口的换栈**(谁、何时、怎么换) | **core**(本刀新) | `br_sched_irq_epilogue` → `resched_from_irq` |
| **"什么时候该换"**(时间片) | **调度器插件** | `sched/rr/src/rr.c` 的 `on_tick` → `br_sched_request_resched()` |
| 工作队列本体 / `br_work_submit` / drain 时机 | **core**(本刀新) | `core/src/work/work_core.c` + `irq_core.c` 的出口 |
| 工作项(做什么) | **调用者**(驱动/ISR) | `br_work_submit(fn, arg)` |
| 按线的分发形态(INLINE / BH) | **platform 数据 + 插件 attr** | 绑定表 `dispatch` / `BR_IRQ_F_DISPATCH_BH` |

**新增不变量**(与 §5 的用例一一对应):

* **I4′(取代 ADR-0006 的 I4)**: 换栈点是 {yield, block, exit, 首次调度} **∪**
  {IRQ 出口的抢占, 仅当调度器置了 `need_resched`}。COOP **从不置位** ⇒ 对 coop 而言
  旧措辞(无抢占)逐字成立。**"无抢占"从此是 coop 这条策略的性质, 不是 core 的机制。**
* **I6(bh 不是线程上下文)**: 下半部跑在关中断的异常出口上 ⇒ 在它里面阻塞/让出/退出
  都没有可回退的调度上下文。三条路径**运行期拒绝**: `br_sched_block_current` = `-EPERM`;
  `br_task_yield` = 拒绝 + 留痕; `br_task_exit` = `br_panic`。
* **I7(bh 有界)**: 队列是编译期静态环(满 ⇒ `-EAGAIN`, 绝不静默丢弃), 单次出口执行
  有上界(`BR_WORK_BH_BUDGET`) ⇒ "IRQ 出口的时间"有确定上界。
* **I8(抑制而不取消)**: BH 形态下 `br_irq_disable` 的承诺是"**不再开始**新的 handler
  调用"(§11.4.1); 已排队的那一份被**抑制**(不调用、仍出队、不重放)。

## 3. 逐条"设计没写清/本仓没做过"的裁定

### 3.1 workqueue 整体放 core(与 `3-01` §5 "实现在调度插件" 的偏离)

设计 `3-01` §5 把 work queue 放在"调度插件"里(因为 bh 的"何时跑"是策略)。本刀按用户
裁定改为 **core 拥有整条链**。理由:

1. `br_work_submit` 在 **ISR 白名单**(CA-3)里 ⇒ 它必须在"任何插件都可能用、且调度器
   还没注册时"也成立(§11.1 路径四的 timer ISR 在 M0 就存在)。
2. 它的**执行点**(IRQ 出口)与"哪个调度器在跑"无关 —— 若把它放进插件, 一个没有调度器
   的镜像(裁剪变体)连"ISR → 延迟工作"都不存在, 而设计明说这是 Stage 1 就要有的路径。
3. 队列深度是**产品预算**, 不是调度策略(§5: "队列深度 = manifest 静态声明")。

**代价(如实登记)**: 设计 §11.3 的那条诚实条款("ISR → bh 的延迟**没有上界承诺**")在本
原型**不成立** —— 我们给的是**有界**延迟(下一个中断出口, tick 上界 100 ms)。这是**更强**
的承诺, 不是更弱; 但它同时意味着 **bh 上下文比设计的"线程上下文"更严**: 关中断、
禁阻塞、禁长事务(见 §3.2)。SLOW 域的 demux 仍满足它真正的硬要求("状态寄存器只能在
非 ISR 上下文读")。

### 3.2 bh 的上下文定义: "已经 eoi、还没 ERET"的那一小段

**选择**: bh 在 **IRQ 出口**(`ops->eoi()` 之后、汇编桩恢复现场之前)执行, 跑在**被中断
线程自己的栈**上, `PSTATE.I == 1`, `br_irq_in_isr() == 0`, `br_work_in_bh() == 1`。

**为什么不是 worker 线程**(设计 §12.4 的 v1 形态): worker 需要 core 或插件调
`br_task_create`, 于是 (a) core 要把 `BR_TASK_MAX` 池分一块给 worker, "池上限是硬上界"
这条性质被 bh 偷走一格; (b) 镜像裁剪变体(无调度器)下 bh 直接不存在; (c) "bh 与调度器
共存"的路径变多。三点都指向同一件事: **把 bh 绑在 IRQ 出口上, 它的前提就只有"有中断"**。

**顺序即正确性**(四个位置约束, 逐条都有理由):

1. **在 `eoi` 之后**: bh 里 handler 跑多久都不会让该线停在 Active(running priority 抬高
   ⇒ 全系统 IRQ 饥饿, §4.1.1/§5.3)。
2. **在 `ERET` 之前**(即仍在异常处理里): 不需要 worker、不依赖调度器。
3. **在 `br_sched_irq_epilogue()` 之前**: bh 里唤醒的线程能被**同一次出口**的抢占决策看见
   (否则"ISR 唤醒 → 还要等下一次中断"会白白多一个 tick 的延迟)。
4. **在 INV-C 断言之后**: 出口的 depth 泄漏检查先跑, bh 里的代码不会污染它的判据。

### 3.3 `br_irq_lock_depth` / `br_irq_in_atomic`: thread-only 的执法面收紧

新增两个查询(`br_irq.h` §6 的观测契约, 不是 golden 面):

* `br_irq_in_atomic()` = ISR **或** bh —— thread-only API(`br_irq_register/enable/disable`
  与域子 API)的执法点**从"在 ISR 里"改为"在原子上下文里"**。理由: bh 虽然
  `br_irq_in_isr() == 0`(ISR 已返回), 但它仍在关中断的异常出口上 —— 在那里注册中断或
  阻塞会让异常帧永不返回。**两者分开保留**(而不是把 bh 也算进 `in_isr`): 用例要能机械
  区分"handler 真的不在 ISR 上下文里跑"(§9.4/§12.1 的判据), 那正是 bh 存在的意义。
* `br_irq_lock_depth()` = L2 嵌套深度。**唯一消费者**是 `br_sched_thread_entry`
  (裁定 P-1, §3.5)。

### 3.4 `br_irq_disable` 在 BH 形态下的语义(照 §11.4.1 逐条实现)

| 问题 | 本实现 |
|---|---|
| disable 是否等正在跑的 handler? | **不等**(与 Linux `disable_irq_nosync()` 同语义) |
| disable 返回后正在跑的 handler? | **让它跑完**; 承诺收紧为"不再**开始**新的调用" |
| 已排队但尚未开始的 work? | **抑制**: 派发前查 `depth > 0` ⇒ 跳过 handler, 工作项**仍被消费出队**(不占队列深度), 计数 `suppressed_dispatch` + 留痕 |
| 抑制了本该处理的事件? | 会 —— 与 §6.3 的诚实条款一致; level 型不丢(状态还在寄存器里) |
| enable 后补吗? | **不补、不重放**(避免 `enable` 变成"偷偷重放历史工作"的语义泥潭) |
| 需要"等 handler 结束"? | 用同步原语显式表达(驱动提供 flush/sync), 不做进 `br_irq_disable` |

### 3.5 抢占: "置位 + core 统一执行"(裁定 P-1/P-2)

**P-2(为什么插件不自己切)**: 换栈必须与"state 机 + 异常出口顺序(eoi 之后)"绑在一起 ——
那是机制(core); 插件只回答"什么时候该换"。⇒ `br_sched_request_resched()` 只置一个位
(`volatile br_bool`), core 在出口消费。**COOP 永不调用它** ⇒ coop 的行为与 ADR-0006 那一刀
逐字一致。

**P-1(新线程首次进入的 L2 形态)**: preempt 可能在 **IRQ 出口**选中一个从没跑过的线程
(`resched_from_irq` → `br_sched_switch_to` → trampoline)。那一刻 `PSTATE.I == 1`(我们仍在
异常处理里), 而新线程**不经 ERET** 进入 ⇒ 若不管, 它会带着关中断一路跑下去(timer 再也不
来 ⇒ 抢占与超时同时失效, 且没有任何报错)。处置:

```c
if (br_irq_lock_depth() == 0u) { br_irq_cpu_enable(); }        /* 线程的应然形态 */
else { /* 留痕, 不动状态 */ }                                   /* 有人在临界区里让出: 既有禁令 */
```

**不"顺手修好"** depth > 0 的情形: 那意味着有人在 L2 临界区里让出了 CPU(与 INV-C 同源的
禁令), 此刻打开中断会把临界区撕开 ⇒ 诚实失败(留痕)优于静默地打开中断。

**被抢占线程的现场在哪(唯一难懂的一处, 值得写死)**: `from` 的现场被
`br_sched_switch_to` 保存为"callee-saved + sp", 而 sp 指向 **`br_irq_enter` 的栈帧**
(异常帧在它下面)。于是被抢占的线程此后停在"IRQ 出口的 C 代码里", 等它被再次选中时从
`br_sched_switch_to` 返回, 依次走完 `resched_from_irq` → `br_sched_irq_epilogue` →
`br_irq_enter` → 汇编桩的"恢复现场 + ERET"。⇒ **它自己的 ELR/SPSR 一直躺在它自己栈上的
异常帧里**, 不需要在切换时另存。因此 `switch.S` 只存 callee-saved **依然够**,
`vectors.S` 的 P-IRQ-ASM-2 注释**不必改**(那条"v2 注意义务"针对的是"在 ERET 前用汇编切栈"
的另一种实现)。

### 3.6 SLOW 级联域: 打开它(TC-IRQ-102 判据翻面)

`3-02` §9.4 的 SLOW 域是 bh 的**旗舰消费者**("状态读取需总线事务 ⇒ ISR 内不可读"), 而
ADR-0002 那一刀把它按分期边界直接拒绝(`br_irq_domain_create(F_SLOW)` → NULL + `NOBH` 留痕,
`TC-IRQ-102` 判"明确拒绝")。bh 落地后这条判据**必须翻面**, 否则"bh 已就位"与"其声明的消费者
仍被拒绝"会变成一处不一致。实现照 §9.4 的骨架:

```
父线 ISR(INLINE):  busy 单飞 → mask 整线 → br_work_submit(demux)
bh(工作项):        br_irq_demux() → 逐子 handler → 逐子 ack → 清 busy → 放行父线
提交失败(-EAGAIN): 清 busy + 放行父线 + drop_count++ + IRQ_DOMAIN_DROP 留痕   ← IR-10
```

* **demux 主循环只有一份**(`br_irq_demux`): FAST 域在 ISR 里调、SLOW 域在 bh 里调 ——
  差异**只在调用者**。这正是"SLOW 域的 `pending()` 必须在非 ISR 上下文里读"这条契约的落点,
  而 QEMU 用例用"方言自己在 ISR 上下文被调用时置违规位"把它变成机械判据。
* **顺序**: 先清 busy 再放行父线 —— 否则放行后立刻重新 assert 的那一轮会被单飞挡掉、
  而线已放行 ⇒ 事件要等下一次 assert 才处理(§9.4 伪码同序)。
* 保留 `#if BR_WORKQ_DEPTH == 0` 的拒绝分支: 将来把 workqueue 裁成可选件时, 那里就是
  正确的降级点(而不是"接受但永远不 demux"的静默失效)。

### 3.7 `br_plugin_gen_total` 的口径修正(工具侧, 由本刀的真实冲突暴露)

`tools/brickie` 的 `gen_plan` 原先把 `br_plugin_gen_total` 写成**插件树里的插件数**;
core 的 `TC-PLUG-001` 断言 "`.br_plugins` 段条数 == 本符号的值"。此前两者恒等, 因为
产品把所有插件都选了。本刀第一次出现**"树里有、但没被产品选中"的插件**
(`sched/coop` 与 `sched/rr` 只能选一个): build 按闭包过滤生成物(见 `build.rs` 的
`filter_gen_sources`), 段里 14 条而符号说 15 ⇒ **七条 QEMU 门禁同时红**。

⇒ 口径改为**闭包大小**(没有 `product.toml` 的纯插件树退回整棵树)。这不是"为了让门禁变绿
而改判据": `filter_gen_sources` 的注释早就写明"生成物的 glob 与 `[select]` 必须由工具对齐",
把计数对齐到**真的会进镜像的那些**才是这条断言的原意。工具源码改了 ⇒ `brickie` 与
`prebuilts/seed` 需要重新生成(`make tools` / `make tools-prebuilt`)。

## 4. 本刀**没有**做(与设计的差距, 逐条列清)

| # | 项 | 现状 |
|---|---|---|
| L1 | `BR_IRQ_DISPATCH_THREAD`(线程化 IRQ / 每线 worker) | ❌ 仍未做: 置位**不报错但忽略** + trace(§3.4 的措辞未变)。设计 §11.4 的 D8 结论是"不新增机制", 而 `THREAD` 形态本身属 Stage 2 后段 |
| L2 | §12.3 的**动态升级**(按频率把 INLINE 升级为 BH) | ❌ 未做; 静态声明(绑定表 / attr)已是"默认即静态"的那条路径 |
| L3 | tickless("按最近期限装弹比较器") | ❌ 未做(继承 ADR-0006 的 L1); 超时分辨率仍 = tick = 100 ms |
| L4 | `br_work` 的**取消/等待**(flush/sync) | ❌ 未做: v1 无 work 对象(与 `3-01` §5 "v1 无 work 对象/cancel"一致); 需要 flush 的驱动自建同步 |
| L5 | bh 的**优先级/合并节流**(`BR_IRQ_DF_COALESCE`) | ❌ 未做(§12.2 的 flags 目前无人读) |
| L6 | SMP(per-CPU 队列 / IPI / `current` 用 TPIDR_EL1) | ❌ 未做: bh 队列与 `need_resched` 都是单核全局量 |
| L7 | 优先级/PI(`prio` 仍被忽略) | ❌ RR 是"同优先级轮转": 最小时间片语义 |
| L8 | `br_task_exit` 在 bh 里 = `br_panic` **只有代码判据**, 无用例 | ⚠ 它是"判据会停机"的那类; 宿主不验(会 abort), 目标上不验(会 PANIC) —— 诚实登记 |
| L9 | `sched/coop` 不再进 QEMU 镜像 | ⚠ 见 §5 的覆盖变化; 宿主 `sched-test` 是它的唯一腿 |
| L10 | `tools-test` 的 `快照 new-json` 是**既有**红(与本刀无关) | ⚠ `tests/brickie/tests/snap/new-json.json` 缺 ADR-0010 引入的 `selftest_hook`/`selftest_proto` 两个键(631/632 通过); 本刀未动它 |

## 5. 实测证据(全部来自本刀真跑)

### 5.1 宿主三条(秒级; `ucontext` 真换栈 + 假时钟)

`brickie test sched-test / rr-test / work-test`:

```
[HOSTTEST] ==== coop: 协作式调度器(无抢占) ====
[HOSTTEST] PASS HOST-SCHED-FIFO        coop 就绪队列 FIFO: 三线程严格轮转 ABCABCABC
[HOSTTEST] PASS HOST-SCHED-BLOCKWAKE   ...
[HOSTTEST] PASS HOST-SCHED-IDLE / POOL / CONF / NAME
[HOST INFO] [TASKCONF] PASS TC-TASK-101 coop: 不 yield 的忙线程不被抢占, yield 后 victim 才跑
[HOSTTEST] SUMMARY pass=8 fail=0 total=8

[HOSTTEST] ==== sched/rr: 时间片轮转抢占(kind=preempt) ====
[HOSTTEST] TRACE rr slice seq=AABBAABB (n=8, slice=2 ticks)
[HOSTTEST] PASS HOST-RR-SLICE    不 yield 的忙线程被时间片抢占: 交替序列 == AABBAABB(每段恰 2 tick)
[HOSTTEST] PASS HOST-RR-NOTICK   不发 tick ⇒ 忙线程不被抢占(抢占的唯一来源是 tick)
[HOSTTEST] PASS HOST-RR-RESCHED  need_resched: 置位可见 → 出口消费 → 无别人可跑时不切栈
[HOST INFO] [TASKCONF] PASS TC-TASK-102 preempt: ...(victim 在 busy 让出之前就跑过)
[HOST INFO] [TASKCONF] PASS TC-TASK-103 preempt: L2 临界区内不发生抢占 ... 放开后 victim 立刻能跑
[HOSTTEST] SUMMARY pass=10 fail=0 total=10

[HOSTTEST] ==== 下半部(workqueue / bh)语义 ====
[HOSTTEST] PASS HOST-WQ-BOUND      队列有界: 填满 DEPTH 条成功, 第 DEPTH+1 条 ⇒ -EAGAIN
[HOSTTEST] PASS HOST-WQ-FIFO       drain 按 FIFO 逐条执行(预算=1 时每轮只出一条)
[HOSTTEST] PASS HOST-WQ-BHCTX      工作项体内 br_work_in_bh() 为真
[HOSTTEST] PASS HOST-WQ-NORECURSE  工作项里再调 br_work_drain ⇒ 直接返回 0
[HOSTTEST] PASS HOST-WQ-NOBLOCK    bh 内 br_sched_block_current ⇒ -EPERM
[HOSTTEST] PASS HOST-WQ-NOYIELD    bh 内 br_task_yield 被拒 + 留痕 BH_CTX_VIOLATION
[HOSTTEST] PASS HOST-WQ-BUDGET     drain(max) 严格遵守预算
[HOSTTEST] SUMMARY pass=10 fail=0 total=10
```

宿主侧的 tick 模型(必须说清, 否则"宿主上怎么会有抢占"是个洞): 目标上 timer ISR 是
**异步**的, 而宿主没有中断。装置给了两条腿 ——
(a) **显式注入**(`host_sched_tick()`: `on_tick` + `irq_epilogue`)用于需要**确定性序列**的
用例(AABB 那条); (b) **自动源**(`host_sched_auto_tick(1)`: 打开后
`br_clock_now()` 每次推进 50 us, 累计到一个 tick 周期就在**关中断深度为 0** 时投递一次
tick)—— 不 yield 的忙循环于是能被"自己读时钟"的那一刻打断, 这正是 `TC-TASK-102/103`
在宿主上也能跑的原因。L2 临界区里**不投递**(= 目标上中断被屏蔽)但时间照走。

### 5.2 QEMU(`build/logs/sched.log`, 镜像 = `sched/rr`)

```
[    0.151657] INFO  [TASKCONF] ---- scheduler=sched/rr kind=1 self=main spawn=216
[    0.156768] INFO  [TASKCONF] PASS TC-TASK-104 I2: READY ⇄ 就绪结构, RUNNING/ZOMBIE 都不在里面
[    0.423762] INFO  [TASKCONF] TRACE preempt busy_us=196424 victim_ticks=1 seen=1
[    0.423955] INFO  [TASKCONF] PASS TC-TASK-102 不 yield 的忙线程被时间片抢占
[    0.824419] INFO  [TASKCONF] TRACE crit spins=2052825 violation=0 after_yield=1
[    0.824620] INFO  [TASKCONF] PASS TC-TASK-103 L2 临界区内不发生抢占
[    0.824875] INFO  [TASKCONF] SUMMARY pass=14 fail=0 total=14
[    1.129411] INFO  [WQCONF] SUMMARY pass=8 fail=0 total=8
[    1.129837] INFO  [PLUGIN] manager: 调度器已注册 ⇒ br_sched_run()
```

`TC-TASK-102` 的数字值得读一遍: busy 从 0.2262 s 跑到 0.4238 s(196 ms ≈ **两个 tick =
一个时间片**), 期间 victim 已经跑过(`victim_ticks=1`) —— 而 busy **一次都没 yield**。
这就是"IRQ 出口真的换了栈"的机械证据(宿主上那条 AABB 序列是同一件事的另一种表述)。

### 5.3 QEMU 的中断侧(`build/logs/irq.log`, `[IRQCONF] SUMMARY pass=91 fail=0 total=91`)

```
[IRQCONF] PASS TC-IRQ-015 BH 线的 ISR 被推迟到下半部执行(不是丢失)
[IRQCONF] PASS TC-IRQ-015 handler 里 br_irq_in_isr()==0(真的不在中断上下文)
[IRQCONF] PASS TC-IRQ-015 handler 里 br_work_in_bh()!=0(bh 上下文标志可见)
[IRQCONF] PASS TC-IRQ-015 CPU-local 计数 bh_deferred 递增(不依赖 trace 环)
[IRQCONF] PASS TC-IRQ-015 bh 结束后硬件线已放行(处理期间 mask 同线的骨架)
[IRQCONF] PASS TC-IRQ-016 bh 内调 thread-only API(br_irq_disable)⇒ -EINVAL
[IRQCONF] info bh: submit(trigger)=0 ran_a=1 disable=0 ran_b=1 handler_ran=0 suppressed=1
[IRQCONF] PASS TC-IRQ-017 BH 形态: 派发前已 disable ⇒ handler 不被调用(工作项仍被消费, 计数 +1)
[IRQCONF] PASS TC-IRQ-017 抑制留痕(TRACE_IRQ_DISPATCH_SUPPRESSED)
[IRQCONF] PASS TC-IRQ-017 enable 之后不重放被抑制的工作(§11.4.1)
[IRQCONF] PASS TC-IRQ-102 SLOW 级联域可用(bh 已落地; 此前在 Stage 1 被拒绝)
[IRQCONF] PASS TC-IRQ-102 子 handler 运行在 bh 上下文(br_work_in_bh()!=0)
[IRQCONF] PASS TC-IRQ-102 状态寄存器**从未**在 ISR 上下文里被读(INV-F: SLOW 域的全部理由)
[IRQCONF] PASS TC-IRQ-102 bh 结束后父线已放行
```

**`TC-IRQ-017` 的窗口是怎么"真的出现"的**(不是造出来的状态): 用**耗尽本轮 drain 预算**
这个生产机制 + drain 的**非重入**性质 ——

```
thread: submit(W1="触发该线"); drain(max=1)   → W1 跑: 触发 ⇒ ISR 提交 bh_run
                                                ⇒ 出口 drain 非重入 ⇒ 工作项留在队列里
thread: W1 回来后本轮预算用尽 ⇒ 退出
thread: br_irq_disable(该线)                  ← 线程上下文, 合法
thread: drain(8)                              → 取出 bh_run ⇒ 该线 depth>0 ⇒ 抑制命中
```

整条链条没有一处"为了测试而造的状态": 预算耗尽与线程侧 disable 都是生产里会发生的事,
而 §11.4.1 要回答的恰恰就是"那一份已经排队的怎么办"。

### 5.4 门禁

`brickie check` = 0 错/0 警/0 提示(闭包 14 个插件, RAM 合计 35 KiB);
`brickie build -j16` 绿; `brickie test -j16`:宿主 6 条(sync/string/mem/sched/rr/work)+
QEMU 7 条(smoke/irq/dbg/sync/plugin/sched/fs)+ 脚本 4 条 —— 全绿。

## 6. 覆盖变化(诚实条款)

本原型一次只能构建**一个**产品镜像, 而 `br_sched_register()` 二次注册 = panic(ADR-0006
裁定 S-1)⇒ 镜像里只能装一个调度器。本刀的裁定是**镜像选 `sched/rr`**(因为"IRQ 出口换栈"
与"下半部"这两条新机制必须有 preempt 类调度器才用得上), 于是:

| 件 | 目标(QEMU) | 宿主 |
|---|---|---|
| `sched/rr` | ✅ `sched-test` 门禁(TC-TASK-102/103) | ✅ `rr-test`(AABB 序列 + 自动 tick) |
| `sched/coop` | ❌ **不在镜像里** | ✅ `sched-test`(含 `TC-TASK-101`) |
| `br_sched_selftest` 的分叉 | 按 `ops.kind` 跑**一边** | 两个宿主用例各跑自己那一边 |

⇒ **没有一条判据被删掉**, 但 `TC-TASK-101`(coop 的定义)从此只在宿主上跑, 而
`TC-TASK-102/103` 只在目标上跑。换回 coop 只需改 `product.toml [select].plugins` 一行。

## 7. 本刀改到的公共文件(逐处; 便于审阅与冲突排查)

| 文件 | 改动 |
|---|---|
| `core/include/br/core/br_sched.h` | **加**: `br_sched_request_resched()` / `br_sched_resched_pending()`; `br_sched_irq_epilogue` 与 `br_sched_block_current` 的注释更新(抢占接缝 / bh 禁令)。**未破任何签名** |
| `core/include/br/core/br_work.h` | **新**(`br_work_submit` / `br_work_drain` / `br_work_in_bh` / `br_work_stats_get` + 契约头注) |
| `core/include/br/core/br_irq.h` | `BR_IRQ_F_DISPATCH_BH` 语义从"忽略"改为"生效"; `br_irq_stat_t` **追加** bh 三项; 新增 `br_irq_in_isr` / `br_irq_in_atomic` / `br_irq_lock_depth`; 域与分期口径的注释更新 |
| `core/include/br/core/br_trace.h` | 追加 4 个事件 id(`BH_DEFER`/`BH_DROP`/`DISPATCH_SUPPRESSED`/`BH_CTX_VIOLATION`)+ `SCHED_PREEMPT` |
| `core/src/sched/sched_core.c` | `s_need_resched` + `resched_from_irq()` + `br_sched_request_resched/resched_pending`; `br_sched_irq_epilogue` 消费它; bh 禁令三处; `br_sched_thread_entry` 的 P-1 |
| `core/src/work/work_core.c` | **新**(有界静态环 / ISR-safe submit / 非重入 drain / bh 标志 / 统计) |
| `core/src/irq/irq_core.c` | BH 分发(`br_irq_dispatch_defer` + `br_irq_bh_run`)+ §11.4.1 抑制 + 出口 `br_work_drain` + `br_irq_in_atomic` / `br_irq_lock_depth` / bh 计数; `br_irq_register` 的 flags **并**而不是覆盖(绑定表可能已给 BH) |
| `core/src/irq/irq_domain.c` | SLOW 域**打开**; `br_irq_demux_fast` → `br_irq_demux`(一份循环两种上下文); 父线 ISR 的 SLOW 分支(单飞/mask/提交/回滚); `br_irq_domain_drop_of` 访问器; 子中断的 DISPATCH 位语义更新 |
| `core/src/irq/irq_pic.c` | 绑定表的 `BR_IRQ_DISPATCH_BH` 折进描述符 flags |
| `core/src/irq/irq_internal.h` | CPU-local 计数 + 域 drop 访问器 + bh 计数接口 |
| `core/src/trace.c` | 名字表补 5 个 id |
| `core/selftest/sched_selftest.c` | 按 `ops.kind` 分叉; 新 `TC-TASK-102/103/104`; 用例文本与注释 |
| `core/selftest/work_selftest.c` | **新**(TC-WQ-001..007) |
| `core/selftest/core_selftest.c` | 聚合 `br_work_selftest()` |
| `sched/rr/**` | **新插件**(manifest / 头 / 实现 / README / tests) |
| `sched/coop/{README.md,tests/smoke.toml}` | 退役说明与 `platform = ["host"]` |
| `platform/qemu-aarch64/include/br/board_irq.h` | 新增 `BR_IRQ_DOMAIN_SLOW` / `BR_IRQ_BH_LINE`(表长 5 → 7) |
| `platform/qemu-aarch64/src/board_irq.c` | 两条新绑定(SGI 4 / SGI 5, 后者 `dispatch = BH`) |
| `platform/qemu-aarch64/src/irq_conf.c` | `TC-IRQ-102` 翻面 + 新 `TC-IRQ-015/016/017`; 用例从 91 → 91 项(重排) |
| `tests/host/{host_sched.h,host_sched.c}` | **新**: 共享装置(假时钟 + 自动 tick 源 + L2 + ucontext + 日志/panic) |
| `tests/host/{rr_test.c,work_test.c}` | **新**; `tests/host/sched_test.c` 改用共享装置 |
| `tests/gates.toml` | 两条新 hosttest; 三条宿主/目标的 sources 与 tag 表; `[WQCONF]` 判据进 smoke/sched-test |
| `product.toml` | `[select].plugins`: `sched/coop` → `sched/rr` |
| `tools/brickie/rust/src/plan.rs` | `gen_total` 口径 = 闭包大小(§3.7) |

## 8. 结论

三件事都落到了"可运行 + 可机械判定"的程度:

* **RR(插件)**: core 只多了"一个位 + 出口的一次换栈", 插件里**没有一行切换代码**;
  `AABBAABB` 序列(宿主)与 `busy_us≈两个 tick 且 victim 已跑`(目标)是同一条性质的两条腿。
* **bh(core)**: ISR 与 handler 之间的接缝有了唯一实现(有界队列 + `eoi` 后 `ERET` 前),
  并且"bh 不是线程上下文"这条**被运行期执法**(阻塞 `-EPERM` / 让出拒绝 + 留痕 / 退出 panic)。
* **workqueue(机制)**: 三个消费者(按线的 BH、SLOW 域 demux、任意 ISR 的裸提交)全部
  走同一条 `br_work_submit`; `-EAGAIN` 的两处回滚都写了, 且 IR-10 那条"唯一的死锁入口"
  有留痕与计数。

设计里没写的 5 类接缝(出口换栈的现场归属、新线程首次进入的 L2 形态、bh 的上下文定义、
抑制/不重放的时序、SLOW 域打开的判据翻面)在 §3 逐条裁定并登记; 未做的部分(§4)按
"诚实条款"列清, 不写成"已实现"。
