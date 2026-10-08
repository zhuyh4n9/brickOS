# 0002 — 中断管理(3-02 Stage 1)落地与 GICv3 驱动

> 状态: **已实施**(brickOS-prototype v0.1.0)
> 设计出处: `docs/3-os-core/3-02-int.md`(中断管理)、`3-01-core-api-list.md` §8/§8.1/§10/§11/§15、
> `1-01-architecture.md` §8(平台三层模式)、`6-01-test.md` §3.7(TC-IRQ 组)。
> 关系: 本 ADR 记录**原型如何落地**这些设计, 以及落地时必须自己拍板的几处
> (**实现裁定 P-IRQ-1…3**)—— 设计文档给定的是"面与语义", 而"哪段代码放哪个目录/
> 声明面怎么表达"是原型期的工程取舍。

## 1. 背景

v0.1.0 原来全程关中断(DAIF 全屏蔽)、没有中断控制器; `README` 的"下一步"里
中断框架排在 M1。但设计 `3-02 §1.1` 把中断子系统切成两段, **Stage 1(调度器交付前)
必须自成闭环**: 无 bh、无线程、ISR 内联执行。也就是说 Stage 1 不依赖调度器,
可以在**今天**实现并在 QEMU 上做 conformance —— 这一条是本 ADR 的正当性来源。

同时, 设计 `1-01 §8` 的"平台能力三层模式"要求中断控制器驱动**不在** platform 数据里:

| 层 | 拥有者 | 本原型落点 |
|---|---|---|
| 语义与机制 | **core** | `core/src/irq/**`(号空间/生命周期/三层屏蔽/优先级语义/域框架/fault) |
| 控制器方言 | **ISA 共享库**(可复用库, **非插件**) | `platform/qemu-aarch64/src/gicv3.c`(GICv3 寄存器序列)+ `src/vectors.S`(异常向量桩) |
| 数据与特化 | **Platform 插件** | `src/board_irq.c`(绑定表/PIC 实例与基址/静态 prio&trigger)+ `src/timer_arch.c` |

## 2. 决策

### 2.1 实现 3-02 的 Stage 1(范围即边界)

| Stage 1 议题(3-02 §1.1) | 状态 | 落点 |
|---|---|---|
| ① 中断号管理与抽象 | ✅ | `br_irq.h` 的 virq 空间 + `br_irq_binding_t` 绑定表 + `irq_pic.c` 的 **有序索引 + 二分**(§14.2 的压缩形态, 不用 `uint16_t virq_of[nr_irq]`) |
| ② PIC 抽象/方言/能力降级 | ✅ | `br_pic.h` 的 `br_pic_ops_t` 全表 + `br_pic_caps_t` 能力位 + 确定性降级 |
| ③ 生命周期(Pending/Active) | ✅ | `br_irq_enter` 的 ack → 分发 → ISR → **唯一 eoi 出口**(§5.2 的四分支表) |
| ④ fault 路径(分槽/分类/extable/panic) | ✅ | `vectors.S` 分槽 + `irq_fault.c` EC 分类 + extable fixup + `br_panic_bare`; **注册 API 属 v2, 未实现**(§10.5 已定稿) |
| ⑤ 优先级与中断抢占 | ✅ / 关 | 逻辑优先级 + 方言量化(向低优先级饱和 + 留痕); **ISR 嵌套按 IR-6 关闭**(`set_running_prio` 槽位在, 无人调) |
| ⑥ 屏蔽与使能三层 | ✅ | L1 `depth` 嵌套 / L2 `br_irq_lock/unlock`(含下溢护栏)/ L3 由 `pic_ops.init` 放行 PMR |
| ⑦ 触发方式 | ✅ | 编码透传 + `pic_ops.set_trigger`; 运行期改触发方式仍不做(§8.1) |
| ⑧ 级联中断域 | FAST ✅ / SLOW ✗ | `irq_domain.c` 的域池/窗口切片/FAST demux/逐子 ack/无属主 mask;**SLOW 域返回 NULL**(其前提是 bh, 属 Stage 2) |
| Stage 2(分发形态/亲和性/均衡/IPI) | ✗ | `BR_IRQ_F_DISPATCH_BH/_THREAD` 置位**接受但忽略 + 留痕**(§3.4); 描述符里不占字段 |

### 2.2 ISA 层不建独立目录, 但保住文件边界(WORKAROUND `br-wa-isa-001`)

设计明确 ISA 共享库是"可复用库, **非插件**"。本原型**没有**独立的 `isa/` 树与
独立的构建单元, 所以把方言放在 platform 插件目录内 —— 但**文件边界就是层边界**:

- `src/gicv3.c` + `include/br/platform/br_gicv3.h`: 只有"换控制器型号就变"的东西
  (寄存器偏移/位域/序列/**EOImode 义务/1020–1023 折算/MSB 对齐的优先级量化**);
  **不含任何板级事实**(基址/引脚/绑定表在看 `board_irq.c` 与 `plat_qemu_virt.c`);
- `src/vectors.S`: 异常向量入口桩 —— 设计里也属 ISA 库(§1.3), 同理同放;
- 冻结的异常帧布局(`br_exc.h`)因为 core 的 extable fixup 必须**改写现场**而与 asm
  共享同一处真值, 也暂放 core(抽出 ISA 库时随之搬家)。

⇒ 欠债登记为 `br-wa-isa-001`(见 `WORKAROUNDS.md`), 退出条件 = 抽出独立 `isa/aarch64/`
**非插件**构建单元 + 把 `br_exc.h`/`vectors.S`/`gicv3.c` 迁走。

### 2.3 `board_irq.h` 走设计给定的**退路**(静态头), 不假装有生成头

设计 §3.1(IR-2)要求"逻辑线名 → virq 的解析发生在**构建期**", 并显式给出退路:
"若生成头机制未落地: platform 插件导出静态头, 内含同型宏。语义等价, 仅'谁生成'不同"。
`brickie` v0.1 **只**生成插件描述符(`build/gen/<plugin>/plugin_desc.c`), **不生成**
`board_irq.h`(该机制属 `2-01` 的开放项 O-4)。⇒ 本原型走退路:
`platform/qemu-aarch64/include/br/board_irq.h` 是**手写静态头**, 驱动源码里
只出现 `BR_IRQ_TIMER` 这样的**名字**, 零数字(绑定表在 `.rodata`, 是 platform 数据)。

### 2.4 GICv3 驱动的"开发"由 brickie 治理声明面

- platform 插件声明**两个接口单元**:
  - `platform/qemu-aarch64#plat`(平台契约, 已有; 本次扩入 `br_plat_irq_init` /
    `br_plat_timer_*` / `br_plat_irq_conformance` / `br_plat_irq_trigger` / `br_plat_irq_hwirq`);
  - `platform/qemu-aarch64#gicv3`(**本次新增**): GICv3 方言的面 —— `br_gicv3_register` /
    `br_gicv3_info` / `br_gicv3_sgi_trigger` / `br_gicv3_readback_*` + INTID 分界宏 +
    `br_gicv3_cfg_t` 类型。
- 流程仍是 `brickie check`(dev+release) → `brickie iface publish`(快照 + CHANGELOG + lock)
  → `brickie gen`(描述符生成物); `plugin.toml` 里的 `hash` 由人从快照**抄回**(§9.1.1:
  机器不改人写文件)。
- `[privileged]` 如实登记: `level = "P4"`(machine: PIC/早期 console/MMU 归 platform),
  `resources.irq` 列出本板用到的线(SGI 0–2 / PPI 30 / SPI 33)。

### 2.5 实现裁定(设计未写死的三处)

| # | 裁定 | 理由 |
|---|---|---|
| **P-IRQ-1** | 绑定表**只覆盖直连线**; 域子中断的描述符由 `br_irq_domain_create` 按窗口切片直接填(`dom_id`/`sub`/`pic_id`), 不需要绑定条目 | 设计的 §3.3/§14.6 在"绑定表是否含域成员"上两种读法都通。选"只覆盖直连线"使 `dom_id` 字段在绑定表里恒为 -1(不接受非 -1 ⇒ `-EINVAL`), 消除"域成员又被绑定表二次描述"的双真值面 |
| **P-IRQ-2** | extable 表由链接器按 `.br_extable` 段收集, **v0.1 线性扫描** | 设计 §10.4 期望"按 pc 有序 ⇒ 二分", 但段顺序 = 链接顺序, 而**没有无运行期初始化的排序点**(在 fault 上下文排序是被禁止的)。表项是个位数 ⇒ 线性扫描的代价可忽略; 有序表 + 二分留给 ISA 层在链接期生成 |
| **P-IRQ-3** | core 侧的优先级量化判据(`step = 1 << (8 - prio_bits)`、向数值大的方向取整)**假定 MSB 对齐的硬件编码**(GIC 系)。LSB 对齐的方言(NVIC)需在方言侧自行处理, 且 core 的量化 trace 对该类方言不保证准确 | 设计 §7.1 已声明"映射归方言"; core 只是为了"降级也要留痕"(IR-7)才需要知道"是否发生了量化"。v1 的首要目标就是 GICv3(QEMU virt), 所以裁定按首要目标写, 并把边界写在案 |
| **P-IRQ-4** | hwirq→virq 索引表项 **8 B**(`{virq, pic_id, pad, hwirq}`), 不是设计 §14.2 说的 4 B | 4 B 装不下"按 (pic_id, hwirq) 排序"的键: 只有一个 CPU 接口 PIC 时 4 B 够用, 但"第二个 PIC 经级联域接入"(IR-3 推论)会让键失去唯一性。代价 = 绑定数 × 8 B(5 条 = 40 B), 可忽略 |
| **P-IRQ-5** | `br_irq_cpu_init()` 除设 `TPIDR_EL1` 外, 还把描述符池的 `pic_id/dom_id` 初始化为 **-1**、`in_handler = 0xFFFF` | BSS 零**不是**这里的"无效": 0 是合法 PIC id、也是合法域 id。不显式初始化 ⇒ 未绑定的 virq 会命中 `dom_id >= 0` 的域成员检查(报 `-EINVAL`), 而设计 §14.3 要求的"无绑定 ⇒ `-ENODEV`"变成死代码。设计侧这份静态池初始化属 `core.init`, v0.1 并入本函数 |
| **P-IRQ-6** | 尚无 PIC 注册时, `br_irq_lock()` 合成一个"全部屏蔽"的凭据(不调方言、不 fault) | EARLY 相 §14.3 步 1 之前没有方言可调; 让"锁"在最早期的代码里也能用, 比"未初始化即崩"更符合"锁是基础原语" |
| **P-IRQ-7** | "控制器不支持优先级"这一降级**复用 `PRIO_QUANTIZED` 事件**(`encoded = 0`)上报 | 冻结的 `br_trace.h` 没有为它留独立 id; 复用 + 在 ADR 里写明, 好过悄悄丢掉这条"降级必须留痕"的义务(§4.2) |
| **P-IRQ-8/9** | `isr_seq` 的**唯一递增点在 `br_irq_enter` 顶部**(storm_check 只采样); 窗口比较回到 **16 位模运算**(`(u16)(isr_seq - epoch) > WINDOW`) | 两处都递增会把 256 的窗口实际压成 ~128; `epoch` 是 `u16`(冻结布局)而 `isr_seq` 是 `u32`, 按 32 位直比会在 `isr_seq > ~65792` 后恒真 ⇒ 检测器**永久失效**(正是 §8.3 警告的失效模式) |
| **P-IRQ-10/11** | INV-C 自愈路径按 `raw = 0`(放行 IRQ)恢复, 并把泄漏记进 `UNLOCK_UNDERFLOW` 事件; `br_sched_irq_epilogue()` 的局部原型暂放 `irq_core.c` | 帧未被 v0.1 使用(ERET 由 SPSR 恢复现场), 所以"按入口 DAIF 恢复"暂时只能取放行; 事件族没有独立 INV-C 项。接缝符号待 `br_sched_ops` 落地时落户到调度头 |
| **P-IRQ-12/13** | 域子中断**拒绝** `BR_IRQ_F_PERCPU`(PERCPU 的权威来源是绑定表, 子中断没有绑定条目); 但**接受** `trigger == DEFAULT`(表示"由域方言决定") | 前者是"绝不静默忽略一个请求的属性"(§3.4); 后者否则会让"域成员 + 未指定触发"永远注册不上(直连线的 DEFAULT 有绑定表可解析, 域成员没有) |
| **P-IRQ-14** | `saved_daif` 存**方言原值**(raw DAIF), 不是 `br_irq_state_t` | `pic_ops.cpu_unmask()` 的入参就是方言原值(见 `br_pic.h`); 存原值才能"精确重放"。`irq_internal.h` 里那句"以 br_irq_state_t 编码"的注释是错的(留待随下次头文件变更一起修) |
| **P-IRQ-15** | GICv3 的 `set_prio` 对"量化后落在**最低可实现档**"的请求返回 **`-ENOTSUP`**(不写寄存器、不提权) | 实测: 5 位优先级下 PMR 只实现高 5 位(写 0xFF 读回 0xF8), 投递判据是 `prio < PMR` ⇒ 0xF8 档**永远收不到**。此时"降到 0xF0"是**提权**(数值更小 = 更紧急), IR-7 明确禁止; §4.2/§7.1 给的正是 `-ENOTSUP` 这条路。副作用: 逻辑 prio ∈ [0xF1,0xFF] 被拒 —— 这是诚实的, 比"静默让该线永不投递"好 |
| **P-IRQ-16** | 一致性用例用**非对齐访存**构造"确定可恢复的 data abort" | MMU 未开 ⇒ 全部访存按 Device-nGnRnE ⇒ 非对齐访问必取 Alignment fault——这正是 `-mstrict-align` 存在的原因, 反过来也是一个不依赖内存映射的确定性 fault 源。**反例(实测)**: "读一个看起来没映射的地址"(0x1000/0x2000)在 QEMU virt 上**不 fault**(那些区间被实现为返回 0)。⚠ 开了 MMU 后本用例必须换构造 |
| **P-IRQ-17** | timer PPI 的 **ISR 注册 + 使能 + 全局开中断**放在 **platform**(`br_plat_irq_start()`), APP 只读心跳计数 | 设计 `3-01 §13.6` 把"中断控制"归 **P3**(仅 `ability.subkind=scheduler` 与 `platform`)、"IRQ 线"归 P1, 而 APP 是 **P0** ⇒ 让 `app/hello` 调 `br_irq_register` 是**声明面与实现不一致**, 且 `brickie check` 的 priv 域(看不到源码)不会报红 —— 这类"工具看不见"的越权只能靠**放置**避免。设计 `3-02 §11.1` 也把 timer 的 ISR 归 `core.init` |
| **P-IRQ-ASM-1** | 向量表每槽 0x80 B 内只放 4 B 跳板, 真正的保存/恢复桩体由宏**每槽各展开一份**放在表后 | 320 B 帧的存取 > 240 B, 物理上放不进一槽; 但"一槽一份实例、互不 fallthrough"的纪律不破 |
| **P-IRQ-ASM-2** | 恢复路径**不**写 `msr daif, x9` | 该编码不存在(只有 `daifset/daifclr` 写 DAIF); `SPSR_EL1` 被恢复, `eret` 原子地套用 DAIF。`frame.daif` 仍按 §6.4 保存 |
| **P-IRQ-PLAT-1** | `ICC_CTLR_EL1.PRIbits` 按 **`field + 1`** 解码并夹到 [5,8] | 实测 QEMU 10.2/cortex-a53: field=4 ⇒ 5 位(与回读断言 `0x01 → 0x08` 一致)。设计 §7.1 提到的 `5 + field` 在本平台会算出 9 位并使 `1 << (8 - prio_bits)` 行为异常; 无论取哪种口径, **回读断言才是执法者** |

## 3. 被否决的替代方案

| 方案 | 否决理由 |
|---|---|
| 把 GICv3 驱动做成一个插件(`isa/gicv3` 或 `ability/gicv3`) | 设计 `3-02 §1.3` 明确 ISA 共享库是"**非插件**"; 做成插件会污染插件分类学(四类 plugin_type)与依赖求解, 也会让"每 SoC 一个 platform 插件"的模型失真 |
| 现在就抽出 `isa/` 独立构建单元 | 需要动 Makefile 的源集合/包含路径/依赖声明(platform → isa 没有声明载体: ISA 库不是插件, 无 `[[dep]]` 可表达), 而收益是**分层整洁**而非本任务的功能。⇒ 登记为欠债 `br-wa-isa-001`, 不半途而废地做一半 |
| 用 `uint16_t virq_of[nr_irq]` 直接索引 hwirq→virq | GICv3 的 1024 INTID 要 2 KB, 与描述符池同量级(设计 §14.2 的诚实修正)。选"有序表 + 二分"(4 B/绑定) |
| 在 host 平台插件上跑中断用例(O-5) | Stage 1 的中断路径(向量桩 → ack → ISR → eoi → ERET)在 host 进程里不存在; `6-01 §3.7` 也已定"中断组 = target-only"。⇒ 用例编进镜像, 由 `make irq-test` 在 QEMU 上判红绿 |
| 让 SLOW 域"先返回 NULL 但假装支持" | 分期边界必须显式: 返回 NULL + 留痕(`TRACE_IRQ_DOMAIN_NOBH`), 而不是让它在 Stage 1 静默不可用 |

## 4. 后果

- **验证入口**: `make smoke`(启动+延时+心跳+一致性摘要)与 `make irq-test`(逐用例 PASS)。
  两次都是**真跑 QEMU**(`-M virt,gic-version=3`), 不是静态检查。
- **QEMU 机器型号必须钉住 `gic-version=3`**: QEMU virt 的缺省是 GICv2
  (`-M virt,dumpdtb` 的 compatible = `arm,cortex-a15-gic`)。镜像里是 GICv3 驱动,
  所以 `QEMUFLAGS` 显式带 `gic-version=3` —— 否则症状是"PIC 初始化完毕却收不到中断"。
- RAM: 描述符池 64×32 B + 运行期表 64×8 B + 域池 + trace 环 256×16 B ≈ **8.3 KiB**
  (core 静态; 与 `product.toml` 的 64 KiB 预算同侧)。
- **两条构建/运行纪律(都不是"可选优化", 见根 README §4)**: 镜像必须带
  `-mstrict-align`(MMU 未开 ⇒ Device 内存 ⇒ 非对齐访存必 fault; 实测编译器给 12 字节
  局部结构清零时生成了 `stur xzr,[sp,#36]`), QEMU 必须显式 `gic-version=3`
  (virt 的缺省是 GICv2, `mrs icc_sre_el1` 会变未定义指令)。
- 仍然欠着的(诚实清单): `br_fault_handler_register`(v2)、`BR_IRQ_DISPATCH_BH/THREAD`
  的实际分发(Stage 2)、SLOW 域(bh)、亲和性/均衡/IPI(v2b)、`CAP_NEST` 嵌套、
  `br_pic_save/restore`(PM, v2)、host 平台的中断抽象(O-5)。
- 设计侧行动项(`3-02 §17.4`: `3-01 §10` 补 `br_pic_register`/`br_irq_bindings_set`
  登记等)属 **brickOS-Design 仓库**的改动, 不在本次原型改动范围内。

## 5. 目标侧门禁抓到的两个真 bug(为什么"能编译"不算数)

两道门禁都是**真跑 QEMU** 的, 它们各抓到一个"静态检查永远看不见"的问题:

| # | 症状 | 根因 | 修法 |
|---|---|---|---|
| 1 | 开机即 `[PANIC] fatal fault: data abort ec=0x25 esr=0x96000061`(还没进 MainLoop) | MMU 未开 ⇒ 全部访存是 Device-nGnRnE ⇒ **非对齐访问取 Alignment fault**; 编译器按 Normal memory 假设, 为 `{u8;u32;u32}` 这样的 12 字节局部结构清零生成了 `stur xzr, [sp, #36]` | `ARCHFLAGS += -mstrict-align`(并把理由写进 Makefile: 开 MMU 后才可去掉) |
| 2 | 风暴用例判红: `storm` 计数已过阈值, 但 trace 环里**找不到** `IRQ_STORM` 事件 | `br_trace_drain()` 在 `avail` 被"单次最多取 N 条"截断时, 起点取的是 `head - avail`(**最新的 N 条**)并把 `tail` 直接推到 `head` ⇒ **静默丢掉中间那段积压**。事件少时(≤16)看不出来, 风暴期 160+ 条积压就暴露 | 起点改为 `s_tail`(**最旧的未消费**); 只有"生产者跑过不止一圈"时才从 `head - SIZE` 开始(那才是真的被覆盖) |

第 1 条值得单独记一笔: 它**不是**中断框架的 bug, 而是"MMU 关着"这个环境事实与
"编译器按 Normal memory 生成代码"之间的错配 —— 在**引入中断之前**的原型里恰好没有
触发它的代码形状。这类问题只有"真跑起来"才会说话。
