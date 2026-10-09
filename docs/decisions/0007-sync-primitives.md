# 0007 — 同步原语(mutex / semaphore / cond / spinlock)在 core 落地

> 状态: **已落地**(宿主侧判据全绿; QEMU 侧端到端用例已接线, 依赖 F2 的调度框架)
> 影响面: `core/src/sync/sync.c`(整体替换占位体)、`core/include/br/core/br_sync.h`(冻结面, 本刀未破)、
> `core/src/time.c`(`br_deadline_from_now`)、`core/include/br/core/br_time.h`(`br_delay_*` 注释)、
> `tests/host/sync_test.c`(新)、`tests/gates.toml`(`[[hosttest]]` + `[[gate]]` 各一条)、
> `core/src/sched/sched_internal.h`(**只读引用**: TCB 布局 + 等待链四原语)
> 设计依据: `3-01` §7(同步原语, 117–139 行)/§14 CA-1(微秒)/CA-3(ISR 白名单)/CA-4(超时三态)/CA-5(面预算),
> `3-02` §6.4(`br_irq_lock/unlock` 嵌套计数)/§11.3(v1 COOP 的 ISR→调度语义),
> `3-03`(阻塞语义)、`6-01` §3.2(TC-SYNC-001..009)/§3.3(TC-TIME-001..003)/INV-1/INV-2
> 相关: ADR-0006(调度框架, F2 同期; 本 ADR 与它共享"阻塞接缝"的措辞)、ADR-0001..0004

## 1. 背景

v0.2 的分配是"调度框架(F2) + 同步原语(F3) + 插件管理器(F1) + 宿主/工具面"。设计的
`3-01` §7 只给了**符号面**(12 个函数 + 3 个 `BR_*_DEFINE` 宏, 且 `br_mutex_t` 是**不透明类型**、
宏体写的是 `...`), `3-03` 把"阻塞语义"留给调度器, 而**同步原语的实现归属、对象布局、
cond 的唤醒记账、spinlock 这四个字在设计里根本没有出现过**。本 ADR 把这几处"设计缺口"
逐条裁定并登记, 同时记录本刀的实测证据与留待项。

## 2. 决策

### 2.1 `spinlock` 是**新增面**(设计 0 命中)⇒ 走 CA-5 记录

`grep -rni spin ../Design/` **无结果**; 设计里最接近的东西是 `3-02` §6.4 的
`br_irq_lock/br_irq_unlock`(L2 临界区)。用户需求里明确要 spinlock, 所以本刀把它作为
**新增 API 面**落地, 并按 CA-5("面目标 ≤50 函数, 超出需决策记录")登记。

它与 `br_irq_lock/unlock` 的关系写成一句可执行的话:

> `br_spinlock_t` = **关中断**(复用 core 的嵌套计数) + **自旋位** + **可观测的争用计数**。

* `br_spinlock_lock(s)` = `br_irq_lock()`(保存进入前 DAIF 并关中断) + 抢自旋位, 返回
  **进入前的中断状态**; `br_spinlock_unlock(s, saved)` 先放自旋位再把状态原样交回
  `br_irq_unlock(saved)` —— 与 `br_irq_lock/unlock` **同构的配对纪律**(凭据必须原样交回)。
* `br_spinlock_trylock()` 忙时返回 `-EBUSY` 且**不持有**中断关闭(它短暂关中断只为做原子的
  test-and-set, 返回前恢复)。

**单核上自旋位为什么仍然要有**(这是最容易被质疑的一点):

1. **互斥的真身是关中断, 但"临界区"需要显式可见**。`br_irq_lock` 表达的是"我暂时不想被
   中断", 谁都可以嵌套、谁都可以在任何地方拿; spinlock 表达的是"这段代码在保护某个对象",
   两者语义不同, 合并会让审查者失去"临界区边界"这个信息。
2. **为多核(CA-9/v2b)留位**。关中断只挡住本核, 不挡住另一个核; 自旋位是 SMP 下唯一还成立的
   互斥。现在写进去, 将来开 SMP 时**语义面不变**。
3. **争用可观测**。`br_spinlock_contention()` 暴露"进过自旋循环的次数"; 单核上正常用法恒 0
   (只有"自己锁自己"才会进自旋 ⇒ 这是死锁而非争用), 所以它的价值是**反证**: 它证明这条
   计数真的维护在热路径上, 而不是没人读的死字段(TC-SYNC-009 断言它为 0)。

### 2.2 同步原语的实现归属: **core**, 不是"调度插件"(偏离 `3-01` 第 115 行)

设计 `3-01` 第 115 行把同步原语的实现放在调度插件里(§5.1 的 `br_sched_ops` 也把
`sem_take/mutex_lock/...` 列成 ops 槽位)。本原型改为 **core 实现**, 与 `br_sched.h`/
`br_sync.h` 头注和 ADR-0006 §1 的措辞**逐字一致**:

* **锁对象必须被所有插件看见**。放插件头会逼出"插件互依赖"(任何用锁的插件都要反向依赖
  调度插件的头); 放 core 则是"公共对象、公共语义"。
* **阻塞/唤醒机制对 coop 与 preempt 是同一套**, 只有"谁先跑"不同。机制在 core, 策略在插件 ——
  这正是 `br_sched_ops` 现在只留 `thread_ready/thread_block/pick_next/on_tick/sleep_until/idle`
  的原因(ADR-0006 §2)。
* **设计的 ops 字段表本身是带省略号的草案**(`3-03` §2 尚待成文), 不是已冻结的接口。
* **对外语义一字不改**: 签名、错误码、`br_sem_give` 的 ISR-safe 都照 `3-01` §7/§11。

### 2.3 对象布局由 core 定(设计只说"不透明 + CA-2", 宏体从未给出)

`3-01` §7 的 `BR_MUTEX_DEFINE` 宏体写的是 `...`, 并说布局 = "core 公共头 + 调度插件私有尾"
(CA-2)。本刀把布局**收敛在 core**(`br_sync.h`): 每个对象一个 `magic`(自检用) + 一个
等待链头 `head`(节点嵌在 TCB 里 ⇒ 同步路径**零分配**) + 少量计数。理由:

* 同步原语现在就在 core(§2.2), 布局跟着实现走; 放插件尾只会制造"同步对象的大小取决于
  选了哪个调度器"这种毫无必要的耦合。
* **零分配是硬要求**, 不是优化: 锁不能因为"内存不够"而失败。等待者是 TCB 的 `wait_next`
  节点(`sched_internal.h` 的冻结布局), 因此 `push/pop/remove` 都不碰堆。

### 2.4 cond = "等待链 + 无等待者 no-op"(裁定 G12)

`cond` 与 `sem` 的区别**只在这一点**: `br_cond_signal()` 遇到空等待链时是 **no-op, 不记账**。
若让它像 sem 一样把信号存进计数, cond 就退化成一个"只能存一个令牌的 semaphore", 而调用者
按标准 cond 语义写出的 `while (!pred) wait()` 会因为"a 的信号被 b 吃掉"而**永久挂起**。
本刀用一条机械判据把它钉住: TC-SYNC-007 先对空 cond `signal`, 再 `wait(ZERO)` —— 必须仍是
`-ETIMEDOUT`(若 signal 记了账就会立刻返回 0)。

`br_cond_wait` 的另一半是"**原子地放锁 + 等待 + 重新加锁**": 放锁走"有等待者则直接交接
所有权"的 mutex 内部路径(不是先置空再让别人抢), 重取一律用**无期限** `br_mutex_lock` ——
超时只覆盖"等待", 不覆盖"重新加锁"。**超时返回 `-ETIMEDOUT` 时持锁状态必须已恢复**
(TC-SYNC-008 的判据)。

### 2.5 `magic` 校验: 未初始化/被踩坏不许静默当"没锁"

`3-01` 的对象是**不透明类型**, 没有任何"是否已初始化"的运行期表达。本刀在每个对象里放
`magic`, 并在每个公开入口校验:

| 返回类型 | 坏魔数的行为 |
|---|---|
| `int` 族(mutex/sem/cond/trylock) | `-EINVAL` |
| 查询族(`is_locked`/`owner`/`count`/`waiters`) | `FALSE`/`NULL`/`0`(无错误通道) |
| `br_spinlock_lock()`(返回中断状态, 无错误通道) | `br_panic_bare()`(**不静默继续**) |

`br_spinlock_unlock()` 即使魔数坏了也**必须**把中断状态交回给 `br_irq_unlock` —— 一次脏对象
不该让本核永久关中断。

### 2.6 与调度框架的握手位: `wait_status`(ICB 协议)

同步原语的**唯一难点**是这个窗口: 关中断临界区里挂完等待链、开中断之后、
`br_sched_block_current()` 把线程置 `BLOCKED` 之前, ISR 可能 `give/signal` 并唤醒本线程。
若 `br_sched_wake()` 此时把它挂进就绪队列, 而 `block_current` 又照常切走 ⇒ **丢唤醒 + 幽灵节点**。

本刀用 TCB 里**既有**的 `wait_status` 消掉它(不新加字段):

1. 调用方(sync.c)在**关中断临界区内**: `self->wait_status = -ETIMEDOUT;` → `br_waitq_push(...)`;
2. `br_sched_wake(t)`: 先写 `t->wait_status = 0`; **仅当** `t->state == BLOCKED` 才置 READY +
   挂就绪队列 ⇒ 落在窗口里的唤醒**只置位、不入队**;
3. `br_sched_block_current(abs)`: 入口若见 `wait_status == 0`, 说明已被唤醒 ⇒ **不切走**,
   直接返回 0; 否则置 `BLOCKED` + 登记期限 + 切走, 回读 `wait_status`;
4. 超时扫描: **只对 `state == BLOCKED`** 的线程置 `-ETIMEDOUT`; 已被唤醒的线程不再超时。

关键性质: **无论"先超时后被 unlock 交接"还是"先交接后超时", 结果收敛到同一个赢家**。
配套纪律: 任何"会被别人唤醒的阻塞"(包括 `br_task_join`)在让出 CPU 前都要把 `wait_status`
立成 `-ETIMEDOUT`, 否则上一次唤醒留下的 0 会被误当"已经又被唤醒"(宿主装置第一次跑就踩到过
这个忙等死循环, 已修)。这条已同步给 F2, 并写入 ADR-0006 的 block/wake 语义。

### 2.7 超时语义与错误码(CA-4 / INV-1)

* 三态统一: `BR_TIMEOUT_ZERO` = 非阻塞 / `BR_TIMEOUT_INF` = 无期限 / 其余是**相对微秒**,
  内部经 `br_deadline_from_now()` 折算成**绝对期限**(tickless 只比较期限)。
* **mutex 的 `ZERO` ⇒ `-ETIMEDOUT`**(设计 `6-01` TC-SYNC-002 与 INV-1; 集成阶段按设计改回,
  冻结头最初的 `-EBUSY` 是本刀的偏离, 已修正);
  **sem 的 `ZERO` ⇒ `-ETIMEDOUT`**(它就是"零期限", 依据 `6-01` TC-SYNC-004)。
  两者**刻意不对称**: mutex 的 ZERO 问题是"现在是忙是闲", sem 的 take 问题是"令牌到没到"。
  这是本刀对设计表(TC-SYNC-002 写 `-ETIMEDOUT`)的**一处显式偏离**, 落点是被冻结的
  `br_sync.h` 头注与本 ADR。
* 超时路径**必须自己从等待链上摘掉自己**(唤醒者不知道超时者还在链上) —— 否则留下幽灵节点,
  下一次 `give/signal` 会唤醒一个早就返回的线程。TC-SYNC-004/008 用
  `br_waitq_len(head) == 0` 作机械判据。
* `br_deadline_from_now()` **饱和不回绕**: 溢出返回 `BR_TIMEOUT_INF`("没有期限"), 绝不
  回绕成过去的时间点(那会把"等到天荒地老"静默变成"立刻超时"; TC-TIME-003)。

### 2.8 `br_task_sleep` 的 `-EINVAL` 判据(裁定 G13)与 `br_delay_*` 降级

* `br_task_sleep(BR_TIMEOUT_INF) ⇒ -EINVAL`: `br_time_t` 是无符号, "负值"不可表达; 而
  "相对睡眠无限长"就是"不返回", 不该伪装成一个正常的睡眠请求。`sleep_until` 同理拒绝 INF。
* `sleep(0) ⇒ 0` 立即返回(不被超时框架打扰); `sleep_until(已过期限) ⇒ 0` 立即返回。
* `br_delay_us/br_delay_ms` **降级为纯忙等**: v0.2 起 `br_task_sleep()` 已落地, `br_delay_*`
  **不再是"睡眠的实现"**, 只是"调度器还不存在"的窗口(platform early_init)与 platform 内部件。
  `app/hello/src/main.c` 的忙等调用点已由 F1 改成 `br_task_sleep(BR_MAINLOOP_PERIOD_MS * BR_US_PER_MS)`
  (本刀不越界改 F1 的文件, 只把口径写进 `br_time.h` 的注释)。

### 2.9 宿主侧判据为什么用 `ucontext`, 以及它不是第二份实现

`tests/host/sync_test.c` 把 sync.c/time.c/string.c 编成宿主可执行, 用
`getcontext/makecontext/swapcontext` 实现一个**假调度器**: 就绪队列 + `br_sched_block_current`/
`br_sched_wake` + "没有就绪线程就把虚拟时钟推到最近期限"的超时扫描。于是
**阻塞、唤醒、超时三态在宿主上真的跑了一遍**(而不是只测无锁快路径)。

**它是测试装置, 不是第二份目标实现**:

* 目标侧的上下文切换是 `core/src/sched/switch.S` 的 aarch64 汇编; 宿主是 x86-64, 手写一遍
  目标切换汇编既无意义也不可执行。宿主只需要"能阻塞/能被唤醒"这一件事, `ucontext` 正好是
  这条最小能力, 且是 libc 标准设施(不引第三方)。
* 等待链四原语与 TCB 布局在宿主侧按 `sched_internal.h` 的**冻结契约原样重实现**(30 行单链表,
  不是算法); `core/src/sched/sched_core.c` **不参与**宿主编译(它含 `wfi` 内联汇编)。
* 因此宿主侧证明的是**同步原语自己的正确性**(挂/摘链、所有权交接、超时三态), 不证明调度器;
  调度器由 F2 自己的用例与 QEMU 门禁负责。

## 3. 被否决的替代方案

| 方案 | 评价 |
|---|---|
| ❌ 同步实现留在调度插件(照 `3-01` §115 字面) | 锁对象被所有插件看见 ⇒ 每人都反向依赖调度插件头; 且 coop/preempt 的锁语义完全相同, 分两份实现必漂移 |
| ❌ 给 `br_thread_t` 加一个"唤醒已到"专用字段 | `wait_status` 已经是这个字段(TCB 注释写明"被唤醒时带出的状态: 0 / -ETIMEDOUT"); 新字段会让 `sched_internal.h` 的冻结布局分叉 |
| ❌ cond 的 signal 记一次"欠账"(sem 化) | 见 §2.4: 会破坏标准 cond 的 `while(!pred) wait()` 用法, 让信号被无关等待者吃掉 |
| ❌ sem_give 先 `count++` 再唤醒等待者 | 被唤醒者在下一次调度点才跑, 期间第三个线程会**抢走**那个令牌, 被唤醒者回来再减就成负数。本刀用**直接交接**(有等待者则不加计数, 令牌直接归属被唤醒者) |
| ◐ mutex_lock_to(ZERO) 返回 `-EBUSY`(冻结头的初版口径) | **被否决**: 设计表 `6-01` TC-SYNC-002 与 INV-1 都把"等不到"统一到 `-ETIMEDOUT`, 否决理由不能是"头文件这么写的"; 集成阶段已按设计改回(`-ETIMEDOUT`), spinlock 的 trylock 不在设计面内, 保留 `-EBUSY` |
| ❌ 宿主侧再写一份 x86-64 切换汇编 | 见 §2.9: 测试装置不该长成第二份目标实现 |
| ❌ 宿主用例只查数据结构(不真阻塞) | 同步原语的 bug 几乎全在阻塞路径; 不真跑等于没测。这也是本刀把主判据放宿主的原因 |
| ◐ `br_spinlock_lock()` 坏魔数只 trace 不 panic | 无错误通道时的"静默继续"正是 `br_sync.h` 头注反对的东西; 选择 `br_panic_bare`(ISR 上下文也安全) |

## 4. 后果与留待项

* **同步面第一次真的能阻塞**: `br_sched_block_current()` 是唯一基石, 四类对象都建在它 + 等待链
  四原语上; TCB 布局与等待链的冻结契约(`sched_internal.h`)第一次有了消费者。
* **`br_deadline_from_now` 落地**: 相对超时统一折算成绝对期限, 为将来到期比较器(tickless)留好接口。
* **留待项**(全部如实登记, 不当成"已实现"):
  1. **时间分辨率 = 100 ms**(周期 tick 扫描)。平台 timer 的周期是
     `BR_BOARD_TIMER_PERIOD_US = 100000`(`platform/qemu-aarch64/src/board_irq.c`),
     超时/睡眠由每个 tick 的到期扫描唤醒。**QEMU 实测**(`build/logs/sync.log` 的
     `[SYNCCONF] MEASURE` 行): 请求 `lock_to(20 ms)` 实测 **96.8 ms**, 请求
     `sleep(1000 us)` 实测 **99.5 ms** —— 都约等于一个 tick。这符合 `3-01` INV-2
     ("不早醒, 晚到无上界"), 但**"按最近期限装弹比较器"的 tickless 机制没有做** ——
     本原型不是 tickless, 是周期 tick 扫描。
      > ★ **后续(ADR-0017)**: tick 周期不再是写死的 100 ms —— 它 = `1/HZ`,
      > `HZ` 由 `product.toml [kernel].hz` 配置(缺省 200 ⇒ 5 ms)。上面那组 QEMU 实测
      > 是**100 ms tick 时代**的记录(≈ 一个 tick); 缺省配置下同型实测 ≈ 一个 5 ms
      > tick + 请求值。性质(分辨率 = 一个 tick)不变。
  2. **优先级继承 / PI 字段未做**。设计 `3-03` 把 PI 归 preempt/v2; `br_mutex_t.owner` 只是
     为诊断(与将来的 PI)保留的位置, v0.2 的 coop 下没有优先级反转的调度后果。
  3. **`br_work_submit` / bottom-half 未做**。它属工作队列/Stage 2, 与 `br_sem_give` 的
     ISR-safe 是两条不同的延迟路径; CA-3 白名单里 `work_submit` 一格本刀未触及。
  4. **`BR_*_DEFINE` 的跨 TU 静态初始化只验证了单 TU**(宿主用例)。真镜像里目前只有一个
     conformance 翻译单元用它们。
  5. **`br_sync.h` 头注里未写"sem ZERO ⇒ -ETIMEDOUT"的显式脚注**(头注说"三态同上", 容易被
     读成与 mutex 同码); 本 ADR §2.7 是真值来源, 头注可在下一次冻结窗口补一句。
  6. QEMU 侧端到端用例已就位并**全绿**(见 §5/§6); 唯一未做的仍是上面第 1 条的
     "真 tickless 比较器"。

## 5. 实测证据(全部来自本刀真跑)

宿主门禁(`brickie test sync-test --no-build`, 用真工具链编 + 真跑):

| 判据 | 命令 | 结果 |
|---|---|---|
| 宿主用例(tests/host/sync_test.c) | `brickie test sync-test --no-build` | `ok hostcc sync-test` + `ok run sync-test`; `[HOSTTEST] SUMMARY pass=36 fail=0 total=36` |
| 同一份 sync.c 的一致性用例(宿主) | 同上日志 | `[SYNCCONF] SUMMARY pass=12 fail=0 total=12`(TC-SYNC-001..009 + TC-TIME-001..003) |
| 无线程挂起/无幽灵节点 | 同上日志 | 每个用例后 `br_waitq_len(head)==0`; 压测 3x50k 次后 mutex/sem 等待链均为 0 |
| 随机化压测 | 同上日志 | `joined=3 incs=74991 expect=74991 overlap=0 errs=0 sem_count=3` |
| 目标侧编译 + 链接 | `brickie test sync-test` | `ok cc core/src/sync/sync.c` / `ok cc core/src/time.c` / `ok ld build/brick.elf`(`br_waitq_*` 由 F2 的 `sched_core.c` 提供) |
| **QEMU 端到端(sync-test 门禁)** | `brickie test sync-test` | `ok qemu sync-test`; `[SYNCCONF] SUMMARY pass=12 fail=0 total=12`(12 个 tag 全 PASS) |
| 全门禁回归 | `brickie test -j16` | **全部全绿**: 宿主 `sync-test`/`string-test`/`mem-test`/`sched-test` + QEMU `smoke`/`irq-test`/`dbg-test`/`sync-test`/`plugin-test`/`sched-test` + 4 条脚本门禁 |
| 目标实测 sleep 分辨率 | `build/logs/sync.log` | `sleep(1000us) -> measured=99473 us`; `lock_to(20000us) -> measured=96676 us`(均 ~1 个 100 ms tick) |
| 宿主 sleep 分辨率(虚拟钟 1 us/tick) | `build/logs/sync-test.log` | `sleep(1000us) -> measured=1000 us`, `sleep(0) -> measured=0 us` |
| UBSan 干净 | 手工附加 `-fsanitize=undefined -fno-sanitize-recover=all` 编宿主用例 | 无 `runtime error`, 36/36 通过 |

## 6. QEMU 侧结论(最终)

`tests/gates.toml` 的 `[[gate]] name = "sync-test"` 判据:
`require = ["\\[SYNCCONF\\] SUMMARY pass=[0-9]* fail=0 ", "irq_ticks=[1-9]"]`,
`forbid = ["\\[SYNCCONF\\] FAIL", "\\[PANIC\\]"]`, `require_tags` = 12 个用例 tag。

最终状态: **绿**。`brickie test sync-test` 的 QEMU 步骤 `ok`, 日志里
`[SYNCCONF] SUMMARY pass=12 fail=0 total=12`, TC-SYNC-001..009 与 TC-TIME-001..003
十二条 tag 全部 `PASS`(逐条见 §5)。

过程中出现过两个**上游依赖**缺口, 都如实登记(与同步原语本身无关):

1. **F1 的描述符段**: `product.toml [build].gen_sources` 一度是 `[]` ⇒ `.br_plugins` 段为空
   ⇒ 插件管理器按设计 panic("段里没有 platform 插件"), 启动链走不到 APP 的 `start()`。
   F1 翻成 `["build/gen/**/plugin_desc.c"]` 后解决。
2. **F2 的等待链原语**: `br_waitq_push/pop/remove/len` 在 F2 重写 `sched_core.c` 时一度丢失
   ⇒ 链接期 `undefined reference to br_waitq_*`。F2 补回后解决(本刀坚持**不在 sync.c 里
   自带一份**: `sched_internal.h` 是冻结契约, 两份列表实现会漂成"随机丢唤醒")。

降级行为仍然保留且经宿主验证: 若 `br_sched_registered() == BR_FALSE`,
`br_sync_conformance()` 打 `[SYNCCONF] SKIP ...` 并只跑不依赖线程的子集
(002/003/005 计数/007 无线程子集/009/TC-TIME-002/003) —— 此时 SUMMARY 的 fail 仍为 0,
但**线程用例的 PASS tag 不会出现**, `require_tags` 会**如实判红**("未验证"不等于"通过")。
这条降级路径用宿主装置实测过: `pass=7 fail=0`, 缺 5 条线程 tag。

## 7. 本刀改到的文件(逐处)

| 文件 | 改动 |
|---|---|
| `core/src/sync/sync.c` | **整体替换占位体**: 四类对象 + `br_sync_conformance()`(1070 行) |
| `core/src/time.c` | 新增 `br_deadline_from_now()`(文件后半, `br_clock_now()` 之后; 饱和不回绕 + INF 原样) |
| `core/include/br/core/br_time.h` | 只改注释: 头注与 `br_delay_*` 的降级说明; 冻结面(类型/常量/声明)未动 |
| `tests/host/sync_test.c` | **新增**: ucontext 假调度器 + 宿主用例(36 条) |
| `tests/gates.toml` | 追加 `[[hosttest]] name = "sync-test"` 与 `[[gate]] name = "sync-test"`(判据/正则见 §6) |
| `docs/decisions/0007-sync-primitives.md` | 本文件 |
| `core/include/br/core/br_sync.h` | **未改动**(冻结面: 布局/常量/签名/错误码口径照实现) |
| `core/src/sched/**` | **未改动**(只读引用 `sched_internal.h`) |
| `app/hello/src/main.c` | **未改动**(F1 的领地)。F1 已自行把主循环的忙等 `br_delay_ms(BR_MAINLOOP_PERIOD_MS)` 换成 `br_task_sleep(BR_MAINLOOP_PERIOD_MS * BR_US_PER_MS)`(真阻塞切换) —— 与本 ADR §2.8 的建议一致, 本刀无需再动 |

## 8. 结论

同步原语(mutex / semaphore / cond / spinlock)已在 core 落地, 建在"`br_sched_block_current`
唯一基石 + TCB 内嵌等待链 + 关中断临界区"三件事上; 宿主侧与 QEMU 侧两套判据全绿;
设计的三处空白(spinlock 无出处、实现归属、对象布局/cond 记账/`-EINVAL` 口径)与一处
(集成阶段修正: mutex ZERO 也返回 `-ETIMEDOUT`, 与 sem 一致 —— 设计表 TC-SYNC-002 与 INV-1 是唯一权威。)
另: 本刀的目标侧一致性套件里的 `TC-*` tag 是**自编号**, 与 `6-01` 的表尚未逐条对齐 —— 见 `WORKAROUNDS.md` 的 `br-wa-test-001`。
