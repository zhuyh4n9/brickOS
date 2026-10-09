# 0008: 调度框架与同步原语 —— `br_sched_ops` 冻结形态、同步归 core、`spinlock` 新面与时间粒度实况

> 状态: **已接受(原型已落地)** | 影响面: **调度框架契约 / 锁归属 / 时间语义 / native API 面**
> 格式依据: `1-02` §2.2(`docs/1-architecture/1-02-api-contract-governance.md:53`); 该节阈值表把 **"sched_class 契约"** 与"解冻/重新冻结"并列为**最高门槛(影响所有插件)**(`:81`)——本篇即这条门槛要求的那份决策记录
> 落点: 原型 `brickOS-prototype-v0.x.0` @ `1d60a15`(原型侧 = `prototype/docs/decisions/0006-scheduler-framework.md` + `0007-sync-primitives.md`; 落点路径均相对原型仓库根)
> 相关: `3-03`、`3-01` §2/§3/§4、`1-01` §4.1/§5.1/§5.2/§5.3、`6-01` §3.1–§3.4、`3-02` §11/§17.4、`1-03` §2/§3(M1)、`Design/comment/review2/02-os-core-review.md:48–52`

## 1. 动机

1. **`br_sched_ops` 在设计里只有省略号**。`3-03` 是骨架(§2 列 7 项"待成文", `3-03-sched.md:36–44`), 其 §2 第 1 项要求"全字段表", 而字段表只在 `3-01` §10(`:292`)留了一行 `br_sched_register`; `1-01` §5.1(`:274–296`)的草案带 `...` 与 `mutex_lock` 等槽位。0.x.0 必须把省略号变成可编译、可注册期校验的接口。
2. **同步原语的归属在设计原文里归插件**。`3-01` §3 标题写"实现在调度插件, 主文档 §5.1"(`:115`), `1-01` §4.1 的表也写"锁的 API 与语义契约 … 实现归调度插件"(`:195`)。这与"锁对象要被所有插件看见"直接冲突(§2 D5)。
3. **`spinlock` 在既有设计正文 0 命中, 而评审已预警**。口径: **本轮新增的两篇之外**(即 `1-01`/`1-02`/`3-01`/`3-03`/`6-01` 等现行正文)`spinlock`/`自旋` 0 命中 —— 本轮 4 篇新文档本身当然会命中, 引用本句时别把新文档算进去; 评审存档侧有 3 处 P1 预警(`comment/review2/02-os-core-review.md:48–52`, 见 D7)。`1-02` §2.2 要求新增面走决策记录。
4. **时间粒度必须写清**。`3-03` §1(`:27`/`:34`)、`3-01` §4(`:144`)、`1-01` §4.1(`:193`)、`1-03` §3 M1(`:233`)都写 **tickless**; 0.x.0 实际是**平台 100 ms 周期 tick**(D8)。不写清, INV-2"不早醒"(`6-01-test.md:41`)会被读成"精密 tickless 已实现"。
5. **面预算必须重算**。`br-sched.txt` 现登记 21 函数(`3-01:58`), 全清单 48 函数 + 3 宏、目标 ≤50(`3-01:6`、CA-5 `:424`)。本篇新增面对预算的影响见 D7。

## 2. 决策

### D1 `br_sched_ops` = **9 字段(3 元数据 + 6 回调槽位)**, 策略在插件、机制在 core

冻结形态(`prototype/core/include/br/core/br_sched.h:96–113`):

| 槽位 | 签名 | 语义 | 空值(裁定 S-1) |
|---|---|---|---|
| `name` | `const char *` | 呈现名(如 `"sched/coop"`) | **必填** |
| `kind` | `br_u32` | `BR_SCHED_KIND_{COOP,PREEMPT,TT}`(`:88–90`); `> TT` ⇒ 注册 panic | **必填** |
| `tcb_size` | `br_u32` | TCB 总字节(≥ 公共头, ≤ 池块 256 B) | **必填** |
| `thread_ready` | `int (*)(br_thread_t *)` | NEW/唤醒 → READY 时入就绪结构 | **必填** |
| `thread_block` | `void (*)(br_thread_t *)` | RUNNING → BLOCKED 时摘除 | **必填** |
| `pick_next` | `br_thread_t *(*)(void)` | 下一个该跑的; `BR_NULL` ⇒ core 进 idle | **必填** |
| `on_tick` | `void (*)(void)` | tick 驱动点(coop 不做时间片) | 可空 |
| `sleep_until` | `int (*)(br_thread_t *, br_time_t)` | 登记唤醒期限(core 的超时表负责遍历) | 可空 |
| `idle` | `void (*)(void)` | 自定义 idle; 缺省 = core 的 WFI 临界区 | 可空 |

- **必填/可空以逐字段注释为准**, 而非头注总述(`br_sched.h:94` "除 `idle` 外都不可为空")——原型裁定 S-1(`prototype/docs/decisions/0006-scheduler-framework.md:64–70`)。注册期对必填项、`kind`、`tcb_size` 范围**硬校验, 违反 ⇒ `br_panic`**; 二次注册同(`prototype/core/src/sched/sched_core.c:375–394`)。
- **与 `1-01` §5.1 草案的差异**(`:274–296`): 草案的 `mutex_lock`/`mutex_unlock`/`sem_take`(`:288–290`)、`work_submit`/`irq_epilogue` **均不在运行期 ops 内**(以代码为准)。`thread_sleep` 被替换为 `sleep_until(t, abs_us)`; 新增 `tcb_size`。`br_sched_irq_epilogue()` 是 **core 内部函数**(`br_sched.h:126`, 实现 `sched_core.c:428–441`, 调用点 `core/src/irq/irq_core.c:746` 未动)——与 `3-02` §17.4 要求"入 `br_sched_ops` 槽位"的字面口径不同(见 §4)。
- **核心裁定**: ops 只表达"**谁先跑**", 阻塞/唤醒机制留在 core(`sched_core.c:456–496`、`:262–285`)。这是 `3-03` §1"语义在 core, 策略在插件"(`:27`)的可编译落地。

### D2 TCB 与上下文: 公共头 + 插件私有尾; `ctx[13]`; trampoline; **`current` 是 core 静态量**

- **布局**: TCB = **core 公共头 + 调度插件私有尾**(CA-2 同型)。公共头 = `struct br_thread`(`prototype/core/src/sched/sched_internal.h:31–56`), 尾从 `ctx[]` 之后开始, 由插件经 `br_thread_priv()` 取用(`br_sched.h:149–155`); coop 的尾 = 8 B 的 `ready_next`(`prototype/sched/coop/src/coop.c:34–36`)。
- **实测尺寸**(本机亲自编译探针: `gcc -I core/include -I core/src/sched` 编一段 `printf`): `sizeof(struct br_thread) = 200`、`offsetof(ctx) = 96`、`ctx` = 104 B。⇒ coop 报 `tcb_size = 200 + 8 = 208`(`coop.c:151`), 与运行期日志 `spawn=208`(`prototype/build/logs/sched.log:568`)一致。
- **`ctx[13]` 保存的寄存器**(`sched_internal.h:22–29`、机器可见常量 `sched_ctx.h:23–26`、asm 真值 `core/src/sched/switch.S:38–45`): `[0..9]` = x19–x28, `[10]` = x29(fp), `[11]` = x30(lr), `[12]` = sp。只存 callee-saved —— 协作式切换只发生在函数调用边界(注释 `switch.S:12–15`)。
- **初始栈帧由 core 构造**(`sched_core.c:566–574`): `x19 = TCB`、`x30 = trampoline`、`sp = 栈顶 & ~15`(AAPCS64 16 字节对齐)、`x29 = 0`(backtrace 停点)。
- **trampoline**: `switch.S:66–73` → C 侧 `br_sched_thread_entry()`(`sched_core.c:525–531`)→ `entry(arg)`; entry 返回 ⇒ `br_task_exit(0)` —— **不返回野地址**(与 `3-01` §2.2 `:110` 同口径)。
- **`current` 指针寄存处 = core 静态量 `s_current`**(`sched_core.c:62`), **不是 `TPIDR_EL1`**; 代码注释明写 `TPIDR_EL1` 是 v2/SMP 的落点。与 `3-01` §2.2 `:111`("`current` 指针: `TPIDR_EL1`")口径不同(§4)。
- **静态 TCB 池**: 容量 `BR_TASK_MAX = 8`(`sched_internal.h:81–83`; `product.toml` 未覆盖, 全树无第二处定义), 单块 `BR_TCB_SLOT_BYTES = 256`(`sched_core.c:47`)⇒ 池共 2048 B。布局漂移由 `_Static_assert` 变成**编译红**(`sched_core.c:55–58`)。

### D3 上下文切换 asm 与 trampoline 归 core(既有口径, 不新增偏离)

与 `3-01` §2.2 的既有口径逐字一致(`:110`): "**上下文切换 asm = core**(主文档 §4.1): callee-saved 集; 初始栈帧由 create 构造, entry 经 **trampoline**"。落地 = `core/src/sched/switch.S`(aarch64 汇编)+ `sched_ctx.h`(asm↔C 的唯一偏移真值桥)+ `sched_core.c` 的构造与 `_Static_assert`。宿主侧由 `ucontext` 假切换层替代(`prototype/tests/host/sched_test.c:11`、`:164–182`), 属测试装置而非第二份目标实现。

### D4 超时框架 = 绝对期限表 + 平台节拍注入 + idle WFI; **ICB 唤醒握手**关闭丢唤醒窗口

- **期限**: 每个 BLOCKED 线程在 TCB 的 `wake_at` 上带**绝对期限**(`sched_internal.h:41`), 相对超时统一经 `br_deadline_from_now()` 折算(`br_sync.h:25–26`)。
- **平台节拍注入**: QEMU 平台 timer ISR 调 `br_sched_on_tick(br_clock_now())`(`prototype/platform/qemu-aarch64/src/board_irq.c:207–208`); core 的 `scan_timeouts()` 遍历全 TCB 链, 只碰 `state == BLOCKED && wake_at <= now`(`sched_core.c:295–310`), 并在**任意 IRQ 出口**再扫一次(`br_sched_irq_epilogue`, `:428–441`), 使超时不依赖"是哪个 IRQ 叫醒 CPU"。
- **idle/WFI 的临界区层级 = L2 关中断**: `关中断 → 检查无就绪 → WFI → 开中断`(`sched_core.c:316–337`), 逐字实现 `3-02` §11.1 的处方(`docs/3-os-core/3-02-int.md:1105–1107`)。插件可用 `idle` 覆盖; coop 留空(`coop.c:157`)。宿主判据 `HOST-SCHED-IDLE`。
- **唤醒握手(ICB)**: `wait_status` 是**唯一握手位**(`sched_internal.h:42–49`)。等待方在**关中断临界区内**先置 `-ETIMEDOUT` 再挂等待链(`sync.c:19–30`; 例 mutex `:163–167`); `br_sched_wake()` 先写 0、**只在 `BLOCKED` 时**入队(`sched_core.c:262–285`); `br_sched_block_current()` 入口读到 0 ⇒ **不置 BLOCKED、不登记期限、不切栈**, 直接返回 0(`:467–478`)。这一条正是"`push` 等待队列 → 真正 block"之间那个窗口的关闭装置——落在窗口里的 ISR 唤醒**只置位不入队**, 不丢唤醒、不留幽灵节点。

### D5 同步原语实现在 **core**(偏离设计原文)

- **设计原文**: `3-01` §3 标题"同步原语(br-sched 组; **实现在调度插件**, 主文档 §5.1)"(`:115`); `1-01` §4.1 `:195`; ops 草案把 mutex/sem 列为槽位(`1-01:288–290`)。
- **0.x.0 裁定**: 原语是**策略无关的 core 设施**(`br_sync.h:7–14`、`sync.c:1–37` 头注; 原型 ADR-0006 §3.5 `:161–166`、原型 ADR-0007 §2.2 `:50–62`)。调度插件只提供 `thread_ready`/`thread_block`/`pick_next`(必填)与必要时 `sleep_until`/`on_tick`/`idle`(D1)。
- **理由三条**: ① **三调度器下语义一致** —— 阻塞/唤醒机制对 coop/preempt/tt 是同一套, 只有"谁先跑"不同(coop 已落地; 另两者未做, 见 D10); ② **可在宿主侧真跑判据** —— `tests/host/sync_test.c:207–368` 用 `ucontext` 假调度器真跑阻塞/唤醒/超时, 36 条宿主用例(原型 ADR-0007 §5 `:208`); ③ **零分配等待链** —— 等待者节点复用 TCB 的 `wait_next`(`sched_internal.h:50`); **等待链的四个操作**(`br_waitq_push`/`pop`/`remove`/`len`)在 `sched_core.c:98–160`, 而**四类同步原语本身**(mutex/sem/cond/spinlock)在 `sync.c`(本句 2026-10-09 修正: 原文误把 `sched_core.c` 的区间当成原语实现); 同步路径不 `malloc`(锁不因分配失败而失败, `sync.c:19–22`)。
- **CA-2 口径的现状(必须分开说)**: **TCB 仍成立**(core 公共头 + 插件私有尾, D2); **锁对象布局的 CA-2 不再成立** —— 布局收敛在 core 的具体结构体里(`br_sync.h:48–54`/`:74–79`/`:94–98`/`:123–126`; 原型 ADR-0007 §2.3 `:64–73`), 不再是"公共头 + 调度插件私有尾"。理由: 实现在 core, 布局跟实现走; 否则"同步对象的大小取决于选了哪个调度器"。`3-01` §3 `:141` 与 CA-2 `:421` 的锁对象部分需回灌(§4)。

### D6 `br_mutex_lock_to(m, 0)`(ZERO): 未锁 ⇒ `0`; 已锁 ⇒ `-ETIMEDOUT`

- **裁定与出处**: `br_sync.h:63–65`、实现与注释 `sync.c:148–155`。对齐 `6-01` §3.2 的 **`TC-SYNC-002`**(`6-01-test.md:82`: 未锁/已锁 ⇒ `0 / -ETIMEDOUT`)与 **INV-1**(`:40`: 任何超时到期 → `-ETIMEDOUT`)。`sem` 的 `take(ZERO)` 同码(`sync.c:247–249`), 二者刻意不对称的依据见 原型 ADR-0007 §2.7(`:126–129`)。
- **如实记录一处偏离与纠正**: 冻结头**初版写的是 `-EBUSY`**, 属偏离; 集成阶段按 `6-01` 改回 —— 代码注释留痕 `sync.c:149–153`("冻结头最初写成 `-EBUSY`…`-ETIMEDOUT` …"); 原型 ADR-0007 §3 把 `-EBUSY` 方案列为**被否决**(`:173`)。
- **证据**: 宿主自检 `sync.c:682–693`(断言 `r1==0`、`r2==-ETIMEDOUT`)+ 目标侧 `build/logs/sched.log:594` `PASS TC-SYNC-002 mutex lock_to(ZERO): 空闲 0 / 忙 -ETIMEDOUT`。
- **唯一保留 `-EBUSY` 的是 `br_spinlock_trylock()`**(`br_sync.h:133–134`、`sync.c:485`)——它**不在设计面内**(D7), 故不受 INV-1 的统一口径约束。

### D7 `spinlock` = **新增面**(CA-5 登记), 不是 `br_irq_lock` 改名

- **先自证"既有设计正文 0 命中"**: `grep -rn "spinlock\|自旋" /home/zhuyh/workspace/unikernel-dev/Design/docs`(在**本轮 4 篇新文档之前**跑)⇒ **无输出(grep 退出码 1)**; 命中只落在 `Design/comment/`(评审存档, 3 处: `comment/review2/02-os-core-review.md:48`/`:50`/`:52`)。⚠ 现在再跑同一条命令会有命中 —— 命中源就是本轮新增的这 4 篇; 复核时应限定在既有正文上。其中 `:50` 正是那条 P1: "自旋锁(ISR 上下文的跨核保护)**没有任何文档认领归属**" —— `:52` 的建议就是"为 v2b 的自旋锁/抢占关闭原语**预先立项决策记录(新面规划)**"。
- **原型实际新增**(`prototype/core/include/br/core/br_sync.h:128–138`): `br_spinlock_init` / `_lock` / `_trylock` / `_unlock` / `_is_locked` / `_contention`(6 函数)+ `BR_SPINLOCK_INIT_VALUE` / `BR_SPINLOCK_DEFINE`(2 宏); 实现 `sync.c:438–512`。语义 = **关中断**(复用 core 的嵌套计数)+ **自旋位** + **可观测争用计数**(`br_sync.h:116–121`)。
- **裁定: 新增面, 而非改名。理由**: `br_irq_lock` 在 SMP 下**只关本核中断**, 失去跨核互斥效力(`3-01` §8 定义 `:229`; 评审 P1 `:50`), 而 v2b 需要 ISR 上下文的跨核保护; 且 `br_irq_lock/unlock` 已在 `br-irq.txt` 冻结批次里(`3-01:61`), 给它改名或扩义属"改/删已冻结条目 = 解冻 → 重新冻结", 是 `1-02` §2.2 的**最高门槛**(`:81`)。现在 `br-sched` 组尚未升格 frozen(第二批, `3-01:436`), **在 EXPERIMENTAL 区新增并登记, 比留到冻结后再临时新增便宜**。
- **判据**: `TC-SYNC-009`(`build/logs/sched.log:602`)覆盖"lock/unlock 中断状态一致 + trylock `-EBUSY` + 争用计数 0 + 坏 magic `-EINVAL`"。
- **CA-5 面预算的现状(引原文)**: `3-01` §1 `:6` "规模: **48 函数 + 3 静态定义宏**(CA-5: 面目标 ≤50, 余量 2)"; CA-5 `:424` 同义。仅 spinlock 一项 ⇒ `br-sched` 组 21 → **27 函数**(`:58`), 全清单 48 → **54 函数**, **超预算 4**, 且静态定义宏 3 → 4。若把 `br_sync.h` 另有的 4 个查询访问器(`br_mutex_is_locked`/`br_mutex_owner`/`br_sem_count`/`br_cond_waiters`, 均不在 `3-01` §3 清单内)一并计入, 则为 **58 函数**。⇒ 必须重算面预算并单独升格记录(§5)。

### D8 时间粒度: 设计写 **tickless**, 0.x.0 实况 = **平台 100 ms 周期 tick + 绝对期限队列**

- **设计表述**: `3-03` §1 `:27`"core 拥有上下文切换 asm、**tickless 超时框架**、锁 API 契约"; `:34`"超时框架: tickless, 绝对期限"; §2 第 3 项 `:40`"**tickless 超时队列**"; `3-01` §4 `:144`"时间(br-sched 组; **tickless 框架**, 平台 timer 供节拍)"; `1-01` §4.1 `:193`; `1-03` §3 **M1** `:233`"sched-coop + native task API + **tickless timer** + bottom half"。
- **① tick 来源与周期(代码指针)**: `#define BR_BOARD_TIMER_PERIOD_US 100000u`(`prototype/platform/qemu-aarch64/src/board_irq.c:29`); timer ISR 每拍 `br_sched_on_tick(br_clock_now())` + `br_timer_rearm(BR_BOARD_TIMER_PERIOD_US)`(`:207–208`, 另见 `:237`)。
- **② 由它推出的语义边界**: 任何超时/睡眠的**实际分辨率 ≈ 一个 tick(100 ms)**; 保证**不早醒**、**晚到无上界**(INV-2, `6-01:41`), 因此 `TC-TIME-002` 的容差是 1.0–2.0 s(`6-01:96`)而非精密值。期限**已经过去**时 `br_sched_block_current()` 直接按 `-ETIMEDOUT` 收场、**不切栈**(`sched_core.c:480–485`)⇒ `ZERO`/已过期期限的语义是"立即", 不必等下一个 tick。
- **③ 原型实测数据(真实数字, 取自本机原型构建日志)**:
  - 目标(QEMU): `[SYNCCONF] MEASURE TC-SYNC-004 lock_to request=20000us measured=96755 us (tick granularity)`(`build/logs/sched.log:596`); `[SYNCCONF] MEASURE TC-TIME-001 request=1000us measured=99056 us`(`:603`)——请求 20 ms 与 1 ms, 实测各约一个 tick。另有 `sync.log:596/603` 的同型数字(96973 / 98755 us)与 原型 ADR-0007 §4 引用的 96.8 / 99.5 ms(`prototype/docs/decisions/0007-sync-primitives.md:186–190`)。
  - 宿主(虚拟钟, 1 us/tick): `sync-test.log:6/13` 的 `measured=20000 us` / `measured=1000 us`(精确)——宿主装置与目标粒度**不同**, 引用数字时不可混用。
- **④ 裁定**: **tickless("按最近期限装弹比较器")未落地**, 登记为差距(L1, 原型 ADR-0006 §6 `:295`); `3-03` §1/§2 第 3 项与 `1-03` §3 M1 的 tickless 表态需要回灌(写进 §4, 本 ADR **不改**那两份文档)。

### D9 work queue / bottom half 未落地 ⇒ 归 M1 差距

- `3-01` §5 定义 `br_work_submit`(`:159–166`, "ISR-safe; 队列满 → `-EAGAIN`"), §11 把它列为 ISR 白名单第一格(`:304`); `1-01` §4.1 `:196`、§5.2 的 coop 形态写"**事件入队、主循环分派**(v1 形态)"(`:310`)。
- **原型 grep 证据**: `grep -rn br_work_submit prototype/{core,sched,platform,app,product.toml}` ⇒ **唯一命中** `prototype/sched/coop/README.md:55`("`br_work_submit` / bh ❌ 未做")——**无实现、无声明、无调用点**。
- **依赖它的 SLOW 级联域被明确拒绝**: `prototype/platform/qemu-aarch64/src/irq_conf.c:524–533` 的 `TC-IRQ-102` 断言 `br_irq_domain_create(..., F_SLOW, ...) == BR_NULL` + `TRACE_IRQ_DOMAIN_NOBH`; 原型把这条记作**分期边界而非缺陷**(`prototype/WORKAROUNDS.md:41`:"SLOW 域的存在前提就是 bh")。设计侧对应 `3-01` §8.1 `:272` 的 SLOW = 线程上下文(bh)契约、`6-01` §3.7 的 `TC-IRQ-102`(`:138`)。
- **裁定**: 归 M1 差距; 与 `3-03` §2 第 5 项"work queue(bh)实现归属: 调度插件"(`:42`)一并回灌。

### D10 只交付 `sched/coop`; `sched_class` 三态校验**已真执法**, 运行时单调度器锁定在 core

- **未做**: `sched-preempt`(v2.0)/`sched-tt`(v3.0)——与 `1-01` §5.2 的版本列(`:304–306`)和 `1-03` §2 的顺序理由(`:221–226`)一致(原型 ADR-0006 §6 L2/L3 `:296–297`)。
- **已落地 ① 组合期校验**(核过 `brickie` 侧, **确实真的校验 `sched_class`**): 枚举常量 `SCHED_CLASSES = [SAFE_PREEMPT, COOP_ONLY, TT_SAFE]` / `SCHED_KINDS = [coop, preempt, tt]`(`prototype/tools/brickie/rust/src/rules.rs:18–19`); `sched_class` 落枚举 + "`TT_SAFE` 必须同时给 `[sched.tt]`"(`rust/src/model.rs:934–936`、`:993–999`); **`COOP_ONLY` × 非 coop 调度器 = `BRV-MF-0001`**(`rust/src/check.rs:408–457`); `sched_kind` 落枚举且"只对 `subkind = "scheduler"` 有意义"(`model.rs:938–946`)。插件侧自述 `sched_kind = "coop"` / `sched_class = "COOP_ONLY"`(`prototype/sched/coop/plugin.toml:18`/`:24`)。
- **已落地 ② 运行时单调度器锁定**: `br_sched_register()` 二次注册 ⇒ panic, 必填项/`kind`/`tcb_size` 越界 ⇒ panic(`sched_core.c:375–394`)。
- **如实**: 我在 `brickie` 侧**未见**"闭包内恰一个 scheduler"的组合期检查(`check.rs` 里唯一的唯一性检查是导出单元名, `:294–307`)⇒ 记入 §5 待核实。

## 3. 替代方案与否决

| 方案 | 否决理由 |
|---|---|
| ❌ **同步原语留给调度插件**(照 `3-01:115` 字面) | 锁对象被所有插件看见 ⇒ 每个使用者反向依赖某个调度插件头; 且 coop/preempt 的锁语义完全相同, 分两份实现必漂移(原型 ADR-0007 §3 `:169`) |
| ❌ **`br_sched_ops` 里放同步原语** | 同上; 且会让"锁对象布局随调度器变", CA-2 的公共头白定(原型 ADR-0006 §4 `:234`) |
| ❌ **`br_irq_lock` 改名/扩义为 spinlock** | 越过 `1-02` §2.2 最高门槛(`:81`)去改 `br-irq.txt` 已冻结条目; 且二者语义不同(见 D7) |
| ❌ **idle 放调度插件**(coop 自己实现 idle) | `3-02` §11.1 的临界区是**语义**不是策略; 放 core 保证三形态都不会写错这段竞态(原型 ADR-0006 §4 `:237`) |
| ❌ **tickless 比较器现在就做**(把平台 timer TVAL 设成 `min(期限)`) | coop 的价值是"最小可用"; 周期 tick 已满足 INV-2(不早醒), 比较器归 v2/v3 的时间结构(`ops.sleep_until`/`on_tick` 槽位已留)(原型 ADR-0006 §3.3 `:135–139`) |
| ❌ **`br_sched_run()` 由调度器插件导出** | 插件管理器就要知道具体是哪个插件 ⇒ 正是 `subkind=scheduler` 想避免的耦合(原型 ADR-0006 §2 S-4 `:85–90`) |
| ❌ **TCB 按 `tcb_size` 运行期分配** | 原型"无动态分配"; 静态上界让"池满 ⇒ `-ENOMEM`"成为可机械判定的性质(原型 ADR-0006 §2 S-3 `:80–83`) |
| ◐ **`br_task_join` 借用 `wait_next` 等待链**(`sched_internal.h` 头注原写"mutex/sem/cond/**join** 共用") | 采纳**侧表** `s_joiner[BR_TASK_MAX]`(零分配、不改冻结布局): `wait_next` 是单节点, 目标自己阻塞在 mutex 上时两者会互相覆盖(原型 ADR-0006 §3.7 `:179–187`) |

## 4. 兼容性影响(含"待回灌")

| 面 | 影响 |
|---|---|
| `br-sched` golden(`second batch`) | `br_sched_ops` 字段表**从省略号变为 9 字段**; 新增 `br_spinlock_*` 6 函数 + 2 宏; 冻结前必须重算 CA-5 面预算(D7) |
| golden 单元粒度 | `br-sched.txt` 组内函数数 21 → 27(仅 spinlock); 具体计数与"D15 冻结批次"条文需一并修订 |
| 结构布局 | TCB 私有尾机制(CA-2)仍成立; **锁对象的不透明性丢了**——`br_mutex_t`/`br_sem_t`/`br_cond_t`/`br_spinlock_t` 在 `br_sync.h` 里是具体结构体(`:48–54`/`:74–79`/`:94–98`/`:123–126`)。v2 二进制分发时, 这些字段进 core golden 而非插件 golden |
| 兼容性承诺 | 对外签名/错误码/ISR-safe 面"一字不改"(除 D6 的 `-EBUSY` → `-ETIMEDOUT` 纠正); 新增面全部落在 EXPERIMENTAL 区(尚未升格 frozen) |
| 现有产物 | 无破坏: 原型 `brickie check` = 0 错/0 警, `test -j16` 全绿(原型 ADR-0006 §5 `:285–286`) |

**待回灌清单(逐条: 文档位置 / 文档现写什么 / 实况是什么)**

| # | 文档位置 | 文档现写什么 | 实况(0.x.0) |
|---|---|---|---|
| T1 | `3-01` §3 标题 `:115`; `1-01` §4.1 `:195`; `3-01` §3 `:141` | 同步原语"**实现在调度插件**"; 锁对象布局 = "core 公共头 + 调度插件私有尾" | 实现在 **core**(`sync.c`); **锁对象布局收敛在 core**, CA-2 只对 TCB 仍成立(D5) |
| T2 | `1-01` §5.1 ops 草案 `:274–296` | `thread_sleep`、`mutex_lock/unlock`、`sem_take`、`...` | 冻结为 9 字段(3 元数据 + 6 回调); 同步/work 不进 ops; `thread_sleep` → `sleep_until`; 新增 `tcb_size`(D1) |
| T3 | `3-03` §1 `:27`/`:34`、§2 第 3 项 `:40`; `3-01` §4 `:144`; `1-01` §4.1 `:193`; `1-03` §3 M1 `:233` | **tickless** 超时框架/队列/timer | 平台 **100 ms 周期 tick** + 绝对期限扫描; 分辨率 ≈ 1 tick(D8) |
| T4 | `3-03` §2 第 5 项 `:42`; `1-01` §4.1 `:196`; §5.2 `:310`; `3-01` §5 `:159–166` | work queue(bh)归属调度插件; coop = "事件入队、主循环分派" | **未落地**(无声明/实现/调用点); SLOW 级联域因此被明确拒绝(D9) |
| T5 | `3-01` §2.2 `:111` | `current` 指针 = `TPIDR_EL1` | 单核用 core 静态量 `s_current`; `TPIDR_EL1` 是 v2/SMP 落点(D2) |
| T6 | `3-02` §17.4 `:1702` | 要求 `br_sched_ops` **增加 `irq_epilogue` 槽位**(不能是 hidden 函数) | 落为 core 内部函数 `br_sched_irq_epilogue()`(非 ops 槽位), 调用点在 IRQ 退出路径未动(D1) |
| T7 | `3-03` §2 第 1 项 `:38` | "`br_sched_ops` 完整规格 + 三形态差异矩阵(coop/preempt/tt × ops 槽位语义)" | 字段表成文; **三形态矩阵只有 coop 列是实测**, preempt/tt 两列是**设计预测**(D1 注、D10) |

## 5. 待办与遗留

1. **回灌 T1–T7**(上表)。其中 T1/T2 属"契约归属"变更, 按 `1-02` §2.2 需在 `3-01`/`1-01` 成文时一并落; T3/T4 改变的是**分期承诺**(M1 的 DoD 措辞)。
2. **面预算重算与登记**: 按 D7 的数字(21 → 27; 48 → 54; 宏 3 → 4)重算 `3-01` §1 组表(`:58`)与 CA-5(`:424`); 并决定 `br_sync.h` 的 4 个查询访问器与 `br_sched.h` 的观测访问器(`br_task_name`/`br_task_state`/`br_sched_task_count`/`br_sched_ready_count` 等)是入 native 面还是归"面向特定作者"(`3-01` §10 的定位)。
3. **tickless 差距**: `3-03` §2 第 3 项的"按最近期限装弹比较器"是**未实现**项, 归属 v2/v3 的时间结构(槽位已留 `sleep_until`/`on_tick`)。
4. **`br-wa-test-001` 的 id 漂移(如实转述, 不夸大也不淡化)**: `prototype/WORKAROUNDS.md:32` 登记"三套目标侧一致性用例的 `TC-*` id 是**自编号**, 与 `6-01` 的用例表尚未逐条对齐", 并举反例"本刀的 `TC-TASK-004` 是'exit 后 ZOMBIE', 而 `6-01` §3.1 的 `TC-TASK-004` 是'sleep(1ms) 实测 ≥1ms'"; 还债动作 = 对齐 id + 补齐设计有而原型无的 `TC-TASK-002/003/004/005`、`TC-SYNC-003`、`TC-TIME-002/003`。**核对结果: 该文字与代码/日志一致** —— `build/logs/sched.log:570–590`(TC-TASK)里 `TC-TASK-002` = "join 拿到退出码 42"、`TC-TASK-003` = "yield 让出后另一个线程真的跑过"、`TC-TASK-004` = "exit 后 state=ZOMBIE"、`TC-TASK-005` = "非法入参 `-EINVAL`"; `:595` 的 `TC-SYNC-003` = "非持有者 unlock ⇒ `-EPERM`"; `:605–606` 的 `TC-TIME-002/003` = "sleep(0) 立即返回"/"sleep(INF) ⇒ `-EINVAL`" —— 均与 `6-01` §3.1/§3.2/§3.3 的同号用例**含义不同**。`TC-SYNC-002` 与设计**对齐**(`:594` ↔ `6-01:82`), 可作"对齐已可做到"的先例。日志里 id 后的自述文字准确, 故"按 id 检索"会对不上、"按文字读"不会误导。
5. **待核实(不确定项, 不写成结论)**:
   - `brickie` 侧是否有"闭包内恰一个 scheduler"的组合期检查 —— 我未找到(只找到导出单元唯一性, `check.rs:294–307`); 需向工具侧确认这是"刻意不做"还是缺口(D10)。
   - `BR_TASK_MAX = 8` 是否应由 `product.toml` 的预算裁剪(设计 `3-02` §14.6 允许 manifest 裁剪 `BR_IRQ_MAX` 等); 当前 `product.toml` 未声明, 用的是 `sched_internal.h` 默认值。
   - `3-01` §2.2 `:112` 的 `BR_STACK_MIN` 标注为 `2K [?]`: 原型取 2048(`br_sched.h:50`)—— 是设计值还是原型自定, 待设计侧确认。
6. **不在本篇范围**: `sched-preempt`/`sched-tt`(D10)、SMP/`TPIDR_EL1`(D2)、优先级/PI(v2, `3-03` §3 `:51`)、栈保护区(原型 ADR-0006 §6 L7)。
