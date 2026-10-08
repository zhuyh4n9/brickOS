# 0006 — 调度框架(core)+ 协作式调度器 `sched/coop` 落地

> 状态: **已落地**(宿主侧判据全绿; QEMU 侧端到端用例全绿 —— `brickie test sched-test`)。
> 影响面: `core/src/sched/sched_core.c`(**整体替换占位体**)、`core/src/sched/switch.S`(新)、
> `core/src/sched/sched_ctx.h`(新)、`core/src/sched/sched_internal.h`(**只加不改**: ICB 注 + 切换层原型)、
> `core/include/br/core/br_sched.h`(**只加不破**: `BR_STACK_MIN` / `br_sched_tcb_size` / `br_thread_priv` / ICB 注)、
> `sched/coop/**`(新插件)、`core/src/irq/irq_core.c`(把 `br_sched_irq_epilogue` 的实现搬走, 调用点不动)、
> `platform/qemu-aarch64/src/board_irq.c`(timer ISR 加一行 tick 驱动)、
> `product.toml`(`[select].plugins` 追加 + `[build].core_sources` 纳入 `.S`)、
> `tests/host/sched_test.c`(新)、`tests/gates.toml`(`[[hosttest]]` + `[[gate]]` 各一条)、
> `app/hello/src/main.c`(**接线**: APP 线程化 + `br_task_sleep`; 主控在 2026-10-09 把本文件划归本刀)
> 设计依据: `3-03-sched.md`(总纲; §2 的 7 项待成文清单)、`3-01` §2(任务/线程)/§5.1(`br_sched_ops` 注册点)/
> §14 CA-1(微秒)/CA-4(超时三态)/CA-5(面预算)、`3-02` §11.1(四条交互路径, 含 idle 的 L2 临界区处方)/
> §11.2(`br_sched_irq_epilogue`)/§11.3(v1 coop 的中断-调度语义)、`1-01` §9(启动序列)/§13、
> `6-01` §3.1(TC-TASK-*)/§3.3(TC-TIME-*)/INV-1/INV-2、`WORKAROUNDS.md` 的 `br-wa-boot-001`
> 相关: ADR-0005(插件管理器, F1; `br_sched_run()` 的调用者)/ADR-0007(同步原语, F3; ICB 的**共用措辞**以
> `br_sched.h`/`br_sync.h` 的头注为准)

## 1. 背景与本刀的分工

设计把调度拆成两半: **语义在 core, 策略在插件**(`3-03` §1)。但 `3-03` 是**骨架**
(§2 列了 7 项"待成文"), `br_sched_ops` 的字段表在 `3-03` 里只有一行指向 `3-01` §5.1, 而
`3-01` §5.1 的 ops 表带省略号。本刀要同时交付: core 的切换/超时/idle/状态机 + 一个真的
调度器插件 + 能证明"真的在切"的用例。于是"设计缺口"必须在实现里逐条拍定并登记(§2)。

分工落点(与 `br_sched.h` 文件头的表一致):

| 面 | 归属 | 落点 |
|---|---|---|
| 上下文切换 asm / TCB 公共头 / 栈与 trampoline / 超时唤醒 / idle / 状态机 / 首次调度 | **core** | `core/src/sched/{sched_core.c,switch.S,sched_internal.h}` |
| 就绪队列 / `pick_next` / tick 策略 / 抢占决策 | **调度器插件** | `sched/coop/src/coop.c` |
| 阻塞与唤醒的**机制**(block/wake/等待链) | **core** | `sched_core.c` + `sched_internal.h` |
| 锁与信号量的对象与语义 | **core** | `core/src/sync/sync.c`(F3; ADR-0007) |

**关键不变量**(实现与用例都建在它们上; 与 `br_sched.h`/`sched_internal.h` 的头注同源):

* **I1** `br_sched_register()` 把**调用者所在的启动上下文**物质化为 main 线程(RUNNING, 名字 `"main"`)。
  register 之前 `br_task_self() == BR_NULL`, 之后恒 == main。⇒ `br_task_create/join` 在
  `br_sched_run()` **之前**(APP 的 `start()` 相)就能工作; `br_sched_run()` 只是"进入 idle / 首次调度"的入口。
* **I2** 只有 core 改 `state`; 插件只动"就绪结构的成员关系"。READY ⇄ 队列; RUNNING ⇒ 不在队列;
  `pick_next` **只出队**, 置 RUNNING 是 core 的事(状态机一处真值)。
* **I3** `br_sched_wake(t)` **先**写 `t->wait_status = 0`, 只有 `state == BLOCKED` 才挂回就绪队列;
  超时扫描只碰 `state == BLOCKED` 且 `wake_at` 到期的线程。
* **I4** 无抢占(coop): 只在显式点(yield / block / exit / 首次调度)换栈; IRQ 出口不切栈。
* **I5** **ICB 握手**: `wait_status` 是唤醒与超时之间的**唯一赢家位**(见 §3)。

## 2. `br_sched_ops` 字段表成文(补齐设计省略的 5 类槽位)

`3-03` §2 第 1 项要求"全字段表 + 三形态差异矩阵"。本刀的成文表 = `br_sched.h` 里的
`br_sched_ops_t`; 逐槽位的三形态语义:

| 槽位 | coop(本刀) | preempt(v2) | tt(v3) | 空值合法性 |
|---|---|---|---|---|
| `name` | `"sched/coop"` | — | — | **必填** |
| `kind` | `BR_SCHED_KIND_COOP` | `_PREEMPT` | `_TT` | **必填**(> TT ⇒ panic) |
| `tcb_size` | `br_sched_tcb_size() + sizeof(struct coop_priv)` = 208 B | +就绪节点/时间片/PI 字段 | +表项 | **必填**, 且 `[sizeof(公共头), BR_TCB_SLOT_BYTES]` |
| `thread_ready` | FIFO 尾插 | 按 prio 插 | 按表插 | **必填** |
| `thread_block` | 防御性摘除(通常空操作) | 摘除 | 摘除 | **必填** |
| `pick_next` | 出队; 空 ⇒ `BR_NULL` ⇒ core idle | 最高 prio | 表驱动 | **必填** |
| `on_tick` | `BR_NULL`(不做时间片) | 时间片/need_resched | 表推进 | 可空 |
| `sleep_until` | 返回 0(core 的 `wake_at` 表负责) | 时间轮 | 时间轮 | 可空 |
| `idle` | `BR_NULL` ⇒ core 的 L2+WFI | WFI/PM | WFI | 可空 |

**裁定 S-1(两种读法的取舍)**: `br_sched.h` 的总述写"除 `idle` 外都不可为空", 但逐字段注释
又写 `on_tick`/`sleep_until` 可为 `BR_NULL`。本刀取**逐字段注释优先**: 必填 = {`name`, `kind`,
`tcb_size`, `thread_ready`, `thread_block`, `pick_next`}, 可空 = {`on_tick`, `sleep_until`,
`idle`}。理由: 总述是"防漏实现"的提醒, 而逐字段注释是**语义**(coop 没有时间片, 强迫它实现
`on_tick` 只会逼出空函数, 把"没有"写成"有但空"—— 那是第二种谎)。注册时对必填项与
`tcb_size` 范围做**硬校验**, 违反 ⇒ `br_panic`(二次注册同): 一个坏掉的调度器注册 = 系统不可启动,
诚实失败优于静默降级。

**裁定 S-2**: 设计草案里的 `sem_take/sem_give/mutex_lock/mutex_unlock/work_submit/irq_epilogue`
**不再进 ops**(与 `br_sched.h` 的"偏离 1/2"一致)。同步原语归 core(理由见 §3.5);
`work_submit`/bh 列为遗留项(§6); IRQ 出口是 **core 的内部函数** `br_sched_irq_epilogue()`。
注意 `3-02` §17.4 要求把这个接缝"落为 `br_sched_ops` 的一个槽位(不能 hidden)"——本刀的
读法是: 接缝**必须存在且不是插件私有隐藏符号**, 但它的调用点是 core 的 IRQ 退出路径
(只有一个), 归 core 比归插件更贴合"core 拥有机制"; 这一读法与 `3-02` §17.4 的**字面**
相左, 登记为回灌项(§7)。

**裁定 S-3**: 栈与 TCB **全部静态**(原型口径: `br_task_attr.stack` 由调用方给, TCB 池 =
`BR_TASK_MAX` 个静态槽)。`3-01` §2.2 允许 `stack == NULL ⇒ br_malloc`, 本刀**不支持**
该路径(⇒ `-EINVAL`): 无动态分配使"池上限/`-ENOMEM`"成为可机械判定的性质(TC-TASK-006)。
池槽 = `BR_TCB_SLOT_BYTES`(256 B; 公共头 200 B + 私有尾余量), `tcb_size` 超上界 = 注册期报错。

**裁定 S-4(`br_sched_run` 的归属与形态)**: 设计只给了它一个名字(`3-01` §5.1 的注册点旁),
没有签名/没有调用者。本刀定为: **core 内部 `BR_NORETURN`**(不返回 ⇒ 不进 native API 的
"可调用面", 沿用 `1-01` §9 的"入口不返回"口径), 由**插件管理器**在全部 `start()` 之后
`if (br_sched_registered()) br_sched_run();`(ADR-0005 §2.2 的启动链)。为什么不是插件导出:
调度器是**恰一个**且要在 EARLY 就注册(早于任何线程), 若由插件导出"进入调度循环"的符号,
插件管理器就得知道具体是哪个插件 —— 那正是 `subkind=scheduler` 想避免的耦合。

**裁定 S-5(ICB 的前置义务)**: `br_sched_block_current(abs)` 要求调用方在**开窗之前**把
`wait_status` 置成 `-ETIMEDOUT`(见 §3)。core 自己的两条调用路径(`br_task_join` /
`br_task_sleep*`)照此办理; 该义务写在 `br_sched.h` 的注释里。

## 3. 逐条"设计没写清"的裁定

### 3.1 启动序列: register 建 main, run 只进循环(I1)

`1-01` §9: `app.start() 创建 APP 线程 → br_sched_run()`。若 main 线程要等 `br_sched_run()`
才存在, 则 `start()` 里的 `br_task_create/join` 全部不可用 —— 而 `br_sched_conformance()` /
`br_sync_conformance()` 的调用点恰恰在 `start()` 里(ADR-0005/0007 的接线)。**读法**:
`br_sched_register()`(EARLY 相, 恰一次)同时完成"锁存 ops + 把当前上下文物质化为 main"。

实测(QEMU, 本刀): `[TASKCONF] ---- scheduler=sched/coop kind=0 self=main spawn=208` ——
用例确实跑在 main 线程里, 且 `create/join/yield/sleep` 全部可用; 随后
`app: APP 线程已创建(name=app state=1)` → `[PLUGIN] manager: 调度器已注册 ⇒ br_sched_run()`
→ `tick=1 ...` ⇒ 首次调度真的把 APP 线程换上去了。

### 3.2 idle/WFI 的 L2 临界区(`3-02` §11.1 处方, `3-03` 漏写)

`3-02` 点名"检查就绪队列 → WFI"的竞态要 **L2 临界区**保护, 并说这条由 `3-03` 写 —— 而
`3-03` 没写。本刀按处方逐字实现(idle 在 **core**, 不在插件):

```c
br_irq_state_t st = br_irq_lock();     /* 关本地 IRQ(L2; 真身 = PSTATE.I) */
if (ready_count_now() != 0u) { br_irq_unlock(st); return; }   /* 有活 ⇒ 不睡 */
__asm__ volatile("wfi");               /* 原子: 检查与睡之间不被打断 */
br_irq_unlock(st);                     /* 醒来后重新 pick */
```

`WFI` 在**有 pending 中断时立即返回**, 即使 `PSTATE.I == 1`(Arm ARM) —— 所以关中断不
妨碍它被 timer 叫醒。插件可提供 `idle` 覆盖(coop 不需要 ⇒ `BR_NULL`), 缺省就是上面这段。

### 3.3 超时框架: `wake_at` 表 + 周期 tick 扫描(诚实条款)

`3-01` §4 / `3-03` §2 第 3 项要"tickless 超时队列"。本刀做到的是**周期 tick 上的到期扫描**:

* 每个 BLOCKED 线程在 `wake_at` 里带一个**绝对期限**(`BR_TIMEOUT_INF` = 无期限);
* `br_sched_on_tick(now)`(平台 timer ISR 调)与 `br_sched_irq_epilogue()`(任意 IRQ 出口)
  扫全表; `state == BLOCKED && wake_at <= now` ⇒ `wait_status = -ETIMEDOUT` + READY + **停止跟踪**;
* `br_sched_block_current` 自己再挡一层: 期限**已经过去** ⇒ 直接 `-ETIMEDOUT`, 不切栈
  (相对 0 的 trylock 语义因此是"立即", 而不是"等到下一个 tick")。

**没做**: "比较器按最近期限装弹"(把平台 timer 的 TVAL 设成 `min(期限)`)。平台的 tick 周期
实测 = **100 ms**(`BR_BOARD_TIMER_PERIOD_US`), 所以本刀的**超时分辨率就是 100 ms**:
`br_task_sleep(2000us)` 实测 ≈ 100 ms(`[TASKCONF] PASS TC-TIME-001`, 日志时间戳
0.0882 s → 0.1820 s)。这符合 INV-2(不早醒, 晚到无上界), 但**不要把它读成"tickless 已实现"**。
⇒ 遗留项 §6。

### 3.4 阻塞接缝 = ICB 握手(I3/I5; 与 F3 的 ADR-0007 §2.6 共用一套措辞)

问题: 同步原语要"关中断挂等待链 → 开中断 → 阻塞", 这中间有一个窗口 —— 如果唤醒
(ISR 里的 `sem_give`)落在"已挂链、还没真正 BLOCKED"之间, 朴素实现会把唤醒丢掉
(醒来时 `state == READY` 但没人知道), 或者留下"已唤醒却还在等待链上"的幽灵节点。

**ICB(interrupt-control-block)握手**: `wait_status` 是**唯一握手位**。

1. 调用方在**关中断临界区内**先写 `self->wait_status = -ETIMEDOUT`(开窗), 再 `br_waitq_push`,
   然后开中断、调 `br_sched_block_current(abs)`;
2. `br_sched_block_current` 入口先看 `wait_status == 0` ⇒ "窗口里已被唤醒" ⇒ **直接 return 0**
   (不置 BLOCKED、不切栈、不登记期限); 否则置 BLOCKED + `wake_at` + 切走; 回来返回 `wait_status`;
3. `br_sched_wake(t)` **先**写 `t->wait_status = 0`; 只有 `state == BLOCKED` 才置 READY + 入队
   (否则只写握手位 = 窗口里的早唤醒); 幂等;
4. 超时路径只对 `state == BLOCKED` 且到期的线程写 `-ETIMEDOUT` + READY + 清 `wake_at`。

顺序无关性: 无论"先超时后被 unlock 交接"还是"先交接后超时", 都收敛到同一个赢家
(`wait_status`)。`br_sched_block_current` 入口的读 + 置 BLOCKED 在**同一个** L2 临界区里完成,
所以 ISR 要么在开窗前看到 `-ETIMEDOUT`(继续阻塞), 要么在关中断后落在窗口外(看到 BLOCKED 并挂链)。

### 3.5 同步原语归 core(与 `3-01` 第 115 行的偏离)

`3-01` §3 标题写"实现在调度插件"。本刀与 F3(ADR-0007 §2.2)取同一读法: **core 实现**。
理由: (a) 锁对象要被**所有**插件看见, 放进调度插件会让每个使用者反向依赖某个插件头;
(b) 阻塞/唤醒机制对 coop 与 preempt 是同一套, 只有"谁先跑"不同; (c) 设计的 ops 表本身是
带省略号的草案(`3-03` §2 待成文)。于是 ops 只留"谁先跑", 同步语义留在 core, **对外语义一字不改**。

### 3.6 coop 与 preempt 的分界: "忙循环不被切"是设计内行为

`3-02` §11.3 的诚实条款: coop 下 IRQ 返回**永远回到被打断的线程**; ISR 唤醒的线程要等到
下一次显式调度点。推论: **一个不 yield 的忙循环线程不会被切走**(没有时间片)。这不是缺陷,
是 coop 的定义 —— 用 `TC-TASK-101` 把它变成机械判据:

> 忙线程在 200000 次循环里采样 victim 的计数器, 必须**恒为 0**; `br_task_yield()` 之后
> 采样到 1。换成 preempt 调度器, 这条用例**应当变红**。

本刀实测(宿主与 QEMU 都 PASS): `TC-TASK-101 coop: 不 yield 的忙线程不被抢占, yield 后 victim 才跑`。

### 3.7 `br_task_join` 用侧表而不是等待链

`sched_internal.h` 的头注把 `wait_next` 描述为"mutex/sem/cond/**join** 共用"。本刀的 join
改用 core 私有的 `s_joiner[BR_TASK_MAX]` 侧表(target → joiner, 一次一个, 与"二次 join ⇒
`-EINVAL`"正好对应)。理由: `wait_next` 是**单节点**(一个线程同时只能挂一条链), 而"join 的
等待者"是**挂在目标 TCB 上**的语义 —— 若借用 `t->wait_next`, 当目标自己正阻塞在某个 mutex 上
时, 它的 `wait_next` 已被 mutex 的链占用, 两者会互相覆盖。侧表零分配(静态数组)、不改冻结
布局, 且"二次 join"判据天然落在 `s_joiner[i] != BR_NULL`。等待链四原语仍按原契约提供
(`br_waitq_push/pop/remove/len`), 供同步原语使用(它们是 F3 的 `sync.c` 的实际依赖)。

### 3.8 `br_task_sleep` 的返回口径(裁定 G13b)

`br_sched_block_current` 超时返回 `-ETIMEDOUT`(INV-1)。但**睡眠的到期是成功路径**:
`br_task_sleep*` 把 `-ETIMEDOUT` 映射成 0 返回。`-ETIMEDOUT` 保留给"等某个事件而超时"的 API
(mutex/sem/cond/join)。理由: INV-1 的措辞针对"阻塞 API 等待失败", 而 sleep 的期限就是它要的
结果; 让 `br_task_sleep(1000)` 永远返回错误码只会逼每个调用点写 `if (r != -ETIMEDOUT) ...`。
`rel_us == 0 ⇒ 立即返回 0`, `rel_us == BR_TIMEOUT_INF ⇒ -EINVAL`(裁定 G13, 与 F3 一致)。

### 3.9 `br_sched_irq_epilogue()` 搬进 sched_core.c

`3-02` §11.2/`irq_core.c` 的 Stage-1 占位体搬到 `sched_core.c`(调用点 `br_irq_handle_end()`
里那一行**不动**), `irq_core.c` 只留 `#include <br/core/br_sched.h>`。coop 下它的动作 =
"扫一遍到期表"; 不切栈(切栈要等 v2 preempt, 且必须在 eoi 之后、ERET 之前 —— 占位体的注释
保留了这条约束)。

### 3.10 整体替换占位实现时的对账纪律(教训)

第一版 `sched_core.c` 漏掉了 `br_waitq_push/pop/remove/len` —— 它们是**冻结内部头里声明、
F3 在用**的符号, 于是全树链接失败(四条 QEMU 门禁同时红)。**纪律**: 整体替换一个占位实现
时, 必须对"冻结头里声明的每一个符号"逐条对账; 最便宜的机械判据是

```sh
nm -u build/obj/**/*.o | grep ' U br' | sort -u
```

(本刀补上四件后, 全树未定义符号只剩链接脚本提供的边界符号。) 同一次事故的另一个面:
`br_thread_pool_used()` 也是内部头声明的符号, 本刀一并补了实现。

### 3.11 `br-wa-boot-001` 的哪一半由此还清

`WORKAROUNDS.md` 的 `br-wa-boot-001` ① = "MainLoop 的睡眠是忙等(`br_delay_ms`), 调度器
(M1)落地后换 `br_task_sleep`"。本刀:
* `hello_start()` 从"自己 `for(;;)` 占住 CPU"改成"**创建 APP 线程 + return 0**"(设计 `1-01` §9
  的启动序列), 由管理器的 `br_sched_run()` 首次调度;
* MainLoop 的 `br_delay_ms(1000)` 换成 `br_task_sleep(1000 * 1000)`(真阻塞切换)。
⇒ **① 还清**。仍欠 = ② 日志/trace 直写 console/RAM 环, 未经服务注册表(归后续刀)。

实测: `tick=1 ... delay=1000886 us` / `tick=2 ... delay=1001979 us`(smoke 门禁 3 s 内跑到
`tick=2`, 判据未破)。

## 4. 被否决的替代方案

| 方案 | 否决理由 |
|---|---|
| **切换层也放进 `.c`**(顶层 `__asm__`) | `.S` 是工具链既有约定(platform 已用), 且 `core_sources` 里补 `*.S` 一处即可; 把 aarch64 指令塞进 C 会让"宿主编译同一份 .c"必须加 `#if` 长城 |
| **`br_sched_ops` 里放同步原语**(照 `3-01` 第 115 行) | 见 §3.5; 且会让"锁对象布局"随调度器变(CA-2 的公共头就白定了) |
| **`br_sched_run()` 由调度器插件导出** | 见 §2 裁定 S-4(插件管理器要知道具体插件名 = 耦合) |
| **TCB 池按 `tcb_size` 运行期分配** | 原型"无动态分配"; 静态上界让池满成为可判定性质(§2 裁定 S-3) |
| **idle 放在插件**(coop 实现 `idle`) | `3-02` §11.1 的临界区是**语义**(不是策略); 放 core 保证 coop/preempt/tt 都不会写错这段竞态。coop 的 `idle` 因此留空 |
| **`wait_status` 之外的握手方案**(如"等待链表头 + 双检") | ICB 只用**一个已有字段**, 不动冻结布局, 与 F3 对齐(ADR-0007 §2.6); 换方案要加字段或加 API |
| **宿主用一个"假切换"驱动策略层** | 见 ADR-0007 §2.9 的同一读法: `ucontext` 是真换栈, 与目标 `switch.S` 的语义一一对应; "假切换"会让 block/join 的判据变成查表 |
| **`br_sched_conformance` 只查数据结构** | 任务书要求它"真的验证切换/阻塞/唤醒/超时": 它自身跑在线程里, 真的 `create + join + yield + sleep`(§5 证据) |

## 5. 实测证据(全部来自本刀真跑)

宿主(`tests/host/sched_test.c`, `ucontext` 版切换层 + 假时钟 + idle 钩子):

```
[HOSTTEST] TRACE fifo seq=ABCABCABC (n=9)
[HOSTTEST] PASS HOST-SCHED-FIFO        coop 就绪队列 FIFO: 三线程严格轮转 ABCABCABC
[HOSTTEST] PASS HOST-SCHED-BLOCKWAKE   block_current/wake: RUNNING→BLOCKED→(wake)READY→RUNNING→ZOMBIE
[HOSTTEST] PASS HOST-SCHED-TIMEOUT     有限期限到期 ⇒ -ETIMEDOUT(wake_at 表在 tick 扫描里唤醒)
[HOSTTEST] PASS HOST-SCHED-SLEEPUNTIL  br_task_sleep_until 登记绝对期限, 不早醒
[HOST INFO ] [TASKCONF] PASS TC-TASK-003 yield 让出后另一个线程真的跑过
[HOST INFO ] [TASKCONF] RUN worker A i=0 / RUN worker B i=0 / A i=1 / B i=1 / A i=2 / B i=2 / A i=3 / B i=3
[HOST INFO ] [TASKCONF] TRACE coop interleave seq=ABABABAB
[HOST INFO ] [TASKCONF] SUMMARY pass=12 fail=0 total=12
[HOSTTEST] PASS HOST-SCHED-IDLE        pick_next 返回 NULL ⇒ core 进 idle(宿主由钩子代 WFI)并被 tick 唤醒
[HOSTTEST] PASS HOST-SCHED-POOL        测试线程全部被 join 回收(只剩 main + supervisor)
[HOSTTEST] SUMMARY pass=8 fail=0 total=8 (idle_calls=100)
```

QEMU(`brickie test sched-test`, `build/logs/sched.log`):

```
[0.085400] INFO  [TASKCONF] ---- scheduler=sched/coop kind=0 self=main spawn=208
[0.086344] PASS TC-TASK-001 create 后 state=READY(且 br_task_self() 非空)
[0.086126] PASS TC-TASK-002 join 拿到退出码 42
[0.087254] TRACE coop interleave seq=ABABABAB        ← "真的在交替执行"的机械证据
[0.087392] PASS TC-TASK-003 两线程严格交替(ABABABAB): 强制切换点真的在切
[0.087576] PASS TC-TASK-004 exit 后 state=ZOMBIE 且 join 回收
[0.087804] PASS TC-TASK-005 非法入参 -EINVAL(create NULL/attr/栈/entry + join NULL/自身)
[0.087997] PASS TC-TASK-006 TCB 池满 ⇒ -ENOMEM(池上限是硬上界)
[0.088226] PASS TC-TASK-007 局部变量地址落在该线程自己的栈区间内(栈真的用上了)
[0.181721] PASS TC-TIME-001 sleep(2000us) 不早醒(实测 >= 请求值)     ← 实测 ~93 ms = 一个 tick
[0.181886] TRACE sleep0 rc=0 elapsed_us=2
[0.181991] PASS TC-TIME-002 sleep(0) 立即返回 0(实测 < 1ms, 未进超时表)
[0.182126] PASS TC-TIME-003 sleep(INF) ⇒ -EINVAL(裁定 G13)
[0.182607] PASS TC-TASK-101 coop: 不 yield 的忙线程不被抢占, yield 后 victim 才跑
[0.182757] [TASKCONF] SUMMARY pass=12 fail=0 total=12
[0.484196] INFO  app: APP 线程已创建(name=app state=1); 交给 br_sched_run() 首次调度
[0.484472] INFO  [PLUGIN] manager: 调度器已注册 ⇒ br_sched_run()
[1.485507] INFO  tick=1 uptime=1486600 us delay=1000886 us (>=1000000 us: ok) irq_ticks=14
[2.487642] INFO  tick=2 uptime=2488764 us delay=1001979 us (>=1000000 us: ok) irq_ticks=24
```

门禁: `brickie check` = 0 错/0 警; `brickie build -j16` 绿; `brickie test -j16` **全部全绿**
(sync/string/mem/sched 四条宿主 + smoke/irq/dbg/sync/plugin/sched 六条 QEMU + 四条脚本)。
`TC-TIME-002` 的判据是"实测 < 1 ms"而不是"`d == 0`": 目标上 ISR 可能在两次读数之间落下
(首版 `d == 0` 因此偶发红, 日志见 `TRACE sleep0 ... elapsed_us=2`)—— 反例是"睡到下一个
tick(100 ms)", 1 ms 阈值把两者分得很开。

## 6. 遗留项(诚实清单; 与 `sched/coop/README.md` 的表一致)

| # | 项 | 现状 |
|---|---|---|
| L1 | **tickless 的"按最近期限装弹"** | ❌ 只做周期 tick(100 ms)上的到期扫描 ⇒ 超时分辨率 = 100 ms |
| L2 | `sched-preempt`(v2.0)+ PI 互斥 | ❌ 未做; `br_sched_irq_epilogue` 已留好切栈位置与顺序约束 |
| L3 | `sched-tt`(v3.0, 调度表) | ❌ 未做; `ops.sleep_until`/`on_tick` 槽位是它的落点 |
| L4 | `br_work_submit` / bh(`3-01` §5) | ❌ 未做(ops 里刻意没有 `work_submit`, 见裁定 S-2) |
| L5 | SMP(per-CPU 就绪队列 / IPI / `current` 用 `TPIDR_EL1`) | ❌ 未做; 本刀 `current` 是 core 静态量(单核) |
| L6 | 优先级/PI | ❌ `prio` 字段被 coop 忽略(`3-01` §2.1: 属性是建议) |
| L7 | 栈保护区(guard)/ 栈溢出检测 | ❌ 未做 |
| L8 | 线程名指针的生命周期(调用方必须保证常量字符串) | ⚠ 约定, 未强制 |
| L9 | `3-02` §17.4 的"`irq_epilogue` 入 ops 槽位" | ⚠ 本刀按 §2 裁定 S-2 放在 core; 若设计坚持入 ops, 需要一次契约决策 |
| L10 | `br_sched_register` 的失败路径是 `br_panic`(无错误码返回) | ⚠ 因签名是 `void`; 设计 `3-01` §5.1 也没给失败语义 |
| L11 | 宿主侧切换层是 `ucontext`(不是目标 asm) | ⚠ 语义等价但**不是同一份代码**; 目标路径由 QEMU 门禁覆盖 |
| L12 | `app/hello` 的 APP 线程栈 8 KiB 是拍的值 | ⚠ `-g3` + 深调用下的余量未实测(backtrace 服务可用时复测) |
| L13 | `br_delay_*`(忙等)在新代码里已无调用点 | ⚠ 是否删除/降级为 platform 内部件留待主控(设计里没有它) |

## 7. 本刀改到的公共文件(逐处; 便于审阅与冲突排查)

| 文件 | 改动 |
|---|---|
| `core/include/br/core/br_sched.h` | **加**: `BR_STACK_MIN`; `br_sched_tcb_size()` / `br_thread_priv()` 声明; `br_sched_block_current` 的 ICB 前置义务注; `br_task_attr.stack` 的"栈顶(高地址)"澄清。**未破任何签名/语义** |
| `core/src/sched/sched_internal.h` | **加**: `wait_status` 的 ICB 注; 切换层三原型(`br_sched_switch_to` / `br_sched_thread_trampoline` / `br_sched_thread_entry`); 私有尾注释指向 `br_sched.h`。**布局未动** |
| `core/src/sched/sched_core.c` | **整体替换占位体**(含搬进来的 `br_sched_irq_epilogue` 与等待链四原语) |
| `core/src/sched/{switch.S,sched_ctx.h}` | 新 |
| `core/src/irq/irq_core.c` | 把 epilogue 的**实现**搬走; 加 `#include <br/core/br_sched.h>`; **IRQ 退出路径的调用点一字未动** |
| `platform/qemu-aarch64/src/board_irq.c` | timer ISR 加 `br_sched_on_tick(br_clock_now());` + 两个 include。**其余未动** |
| `product.toml` | `[select].plugins` 追加 `"sched/coop"`; `[build].core_sources` 追加 `"core/src/*/*.S"`(否则 `switch.S` 不进镜像) |
| `app/hello/src/main.c` | **接线**(主控划归本刀): `hello_start` 创建 APP 线程 + return 0; MainLoop 移入线程体 `hello_mainloop`; `br_delay_ms` → `br_task_sleep`; 钩子名/自检调用序列/管理器契约不变 |
| `sched/coop/**` | 新插件(`plugin.toml` / `src/coop.c` / `include/br/sched/coop.h` / `README.md` / `tests/smoke.toml`) |
| `tests/host/sched_test.c` | 新(宿主切换层 + 桩 + 五条宿主判据) |
| `tests/gates.toml` | 追加 `[[hosttest]] sched-test` 与 `[[gate]] sched-test` |

## 8. 结论

core 的调度框架与 `sched/coop` 在本刀落地到"可运行 + 可机械判定"的程度:
`br_sched_ops` 字段表成文并带三形态语义; 启动序列按 `1-01` §9 走通(APP 线程化);
"真的在切"有交替序列证据; "无抢占"有分界用例; 超时/阻塞的语义与 F3 的 ICB 握手对齐。
设计里没写的 5 类槽位、`br_sched_run` 的归属、idle 的 L2 临界区、同步归属的偏离、
coop/preempt 分界都在本文逐条裁定并登记; 未做的部分(§6)按"诚实条款"列清, 不写成"已实现"。
