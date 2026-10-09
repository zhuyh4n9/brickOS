# 1-04 — 原型 0.x.0 实现现状(实况对账)与 v1.0 差距

> 章节: **1-architecture**。状态: **成文(实况记账, 非设计提案)**。
> 定位: 本篇是**设计 ↔ 原型**的对账表, 回答两个问题 ——
> ① **0.x.0 到底交付了什么**(逐域, 每条带可复核证据); ② **它离 `1-03` §1 的 v1.0 还差什么**(逐项, 带归属与关闭条件)。
> 与 `1-03` 的分工: `1-03` 写"**应该**长成什么样 + 什么时候长"(路线图 / M0–M5 / 设计 DoD); 本篇写"**现在**长成了什么样 + 差多少"。
> 依赖: `1-03`(版本与里程碑)/ `3-01`(native API 清单)/ `3-02`(中断)/ `3-03`(调度)/ `3-04`(内存)/ `3-05`(插件管理)/ `3-06`(服务)/ `5-01`(debug)/ `6-01`(用例目录)/ `2-01`+`2-02`+`brickie-v0.1.md`(工具链)。
> **下游裁定(本篇 §2 的偏离逐条落在三篇决策记录上)**: [`0006-build-declaration-surface.md`](../decisions/0006-build-declaration-surface.md)(构建声明面 + `Makefile` 退役的落点)、[`0007-plugin-manager-runtime.md`](../decisions/0007-plugin-manager-runtime.md)(插件管理器运行期)、[`0008-sched-framework-and-sync-in-core.md`](../decisions/0008-sched-framework-and-sync-in-core.md)(调度框架 + 同步归 core)。
> **写法纪律**: 每条断言要么给 `文件:行`, 要么给**可重跑的命令**(§6)。凡"未验证"就写"未验证", 不写成"应该没问题"。

## 缩略词(abbreviations)

| 缩略词 | 英文全称 | 说明 |
|---|---|---|
| **ADR** | Architecture Decision Record | 决策记录(`docs/decisions/NNNN-*.md`, 格式见 `1-02` §2.2) |
| **APP** | Application | 应用(插件类别: 唯一业务逻辑, 恰一个, 不被任何插件依赖) |
| **bh** | bottom half | 中断下半部(work queue 形态; `3-01` §5) |
| **CA-1–CA-10** | — | core native API 契约决策编号(`3-01` §14) |
| **DoD** | Definition of Done | 完成定义(`1-03` §5 = v1.0 **设计**阶段完成清单) |
| **DMA** | Direct Memory Access | 直接内存访问(外设不经 CPU 读写内存) |
| **EARLY / CORE / LATE / APP** | — | 插件生命周期四相(`1-01` §6.2): 关中断无线程 → 堆可用 → 服务/接口 → 进 APP main |
| **golden** | — | 冻结面的符号/布局快照(`1-02` §2.6.4 生成 → §2.6.6 消费) |
| **GIC / GICv3** | Generic Interrupt Controller | ARM 通用中断控制器(实现归 Platform 插件) |
| **INV-1…INV-6** | — | `6-01` 的不变量编号(超时语义/不早醒/ISR 白名单/… ) |
| **ISA** | Instruction Set Architecture | 指令集架构(设计里 `isa/aarch64/` 是**共享库**, 非插件) |
| **M0–M5** | — | v1.0/v1.x 内部里程碑(`1-03` §3) |
| **MPSC** | Multi-Producer Single-Consumer | 多生产者单消费者(无锁写形态; `5-01` §1 trace 环) |
| **O-x** | — | **本篇**的偏离/裁定编号(§2; 区别于 `O-S*`/`O-H*`) |
| **PLGCONF / SVCCONF / TASKCONF / SYNCCONF / IRQCONF / MEMCONF / DBGCONF** | — | 原型目标侧一致性套件的摘要标签(`[标签] SUMMARY pass=N fail=M`) |
| **PI** | Priority Inheritance | 优先级继承(抢占调度下的互斥反转对策) |
| **TCB** | Task Control Block | 任务控制块(任务的全部元数据) |
| **TC-*-NNN** | — | `6-01` §3 的用例 id(`6-01` 定义, 原型侧执行) |
| **TLSF** | Two-Level Segregated Fit | O(1) 动态内存分配算法(此仓库的堆池实现) |
| **v1.0 / v1.x** | — | **设计**版本代际(`1-03` §1); 与原型版本 `0.1.0`/`0.x.0` 不是同一套编号(见 §0.3) |
| **WORKAROUND** | — | 原型台账条目(`prototype/WORKAROUNDS.md`, id 形如 `br-wa-*`) |

> **编号约定**: `D*` = 全局决策(`1-01` §1); `CA-*` = native API 契约决策(`3-01` §14); `TC-*` = 测试用例(`6-01` §3); `O-x` = **本篇**偏离编号; `br-wa-*` = 原型欠债。

---

## 0. 取证口径与坐标

### 0.1 三个坐标(唯一身份 = 提交)

| 坐标 | 内容 | 备注 |
|---|---|---|
| 设计仓库 | 分支 `brickOS-Design`(即 `Design/` 树) | 本篇所在侧 |
| 原型基线 | 分支 `brickOS-prototype-v0.1.0` @ **`9471fc6`** | 中断框架 Stage 1 + GICv3(`0ab1ecd`)、内存映射/堆/调试插件(**同一个提交** `9471fc6`)、`brickie` v0.1 组合期工具 + platform 插件化(`b55e1cf`) |
| **本篇对账对象** | 分支 `brickOS-prototype-v0.x.0` @ **`1d60a15`** | **相对基线只多 1 个提交**: `brickie` 构建族 + 插件管理器 + 调度框架/`sched/coop` + 同步原语(112 files, +13787/−761) |

原型自己按域留了 7 篇决策记录, 与本篇的对应关系见 **附录 A**。

### 0.2 实况记录的纪律

- 每条"已交付"都配一条**可重跑**的判据(§6 的清单), 不靠"我记得跑过"。
- 每条"未交付"都注明**归属**(哪个里程碑 / 哪篇设计文档的哪一节), 不用"将来会支持"填空。
- 数值一律给**测点**(`build/logs/*.log`、镜像体积、符号计数), 并注明是**干净树**还是增量树上的测量。
- 原型的目标侧一致性套件是 `[XXXCONF]` 摘要格式; 本篇引用摘要时照抄其 `pass/fail/total`, 不做二次加工。

### 0.3 ⚠ 版本串口径**未统一**(原型侧待修, 本篇不依赖它)

原型树里同时存在**两种**版本串, 且互相不一致:

| 位置 | 现值 | 问题 |
|---|---|---|
| `prototype/product.toml` `[product].version` | `0.1.0.0` | 未随 0.x.0 前进 |
| `prototype/README.md`(**整篇**)/ `prototype/WORKAROUNDS.md` **标题** | `v0.1.0` | **README 通篇是 v0.1.0 快照**(全文件无 `0.2.0` 字样); 因此它记的 §5 欠债清单与 `br-wa-entry-001` 状态都已过时(§5.3 #3) |
| `prototype/WORKAROUNDS.md` **正文**、`prototype/docs/decisions/0005-plugin-manager.md` | `v0.2.0` | 与同文件**标题**冲突 |

**裁定**: 这是原型侧的**记账瑕疵**(不是设计问题), 已在本篇登记为待修项(§5.3)。本篇与三篇新 ADR 一律用 **`分支名 @ 提交`**(`brickOS-prototype-v0.x.0` @ `1d60a15`)作坐标, 不引用版本串 —— 提交号不会撒谎, 版本串会。

---

## 1. 0.x.0 交付清单(逐域)

### 1.0 一图看全

| # | 域 | 0.x.0 交付物 | 归属(设计) | 主要证据(§6) |
|---|---|---|---|---|
| 1 | Platform | `platform/qemu-aarch64` 插件: reset/EL 降级/向量、PL011 早期 console、GICv3、arch timer、4 KiB 恒等映射 + region 表、`.br_plugins` 段 | `1-01` §8/§9、`3-02` §4.1、`3-04` §2 | E-2/E-5 |
| 2 | 中断 | 框架 **Stage 1 全量**: 号空间/描述符池/生命周期/三层屏蔽/优先级语义/FAST 级联域/fault+extable/panic | `3-02`(Stage 1 范围) | `[IRQCONF] 67/0` |
| 3 | 内存 | 三池(TLSF 堆 / contig / 页位图)+ DMA + region 表 + `br_mm_ops` + cache 维护; TLSF 红区/毒化 | `3-04`、`3-01` §6/§7、`5-01` §4 | `[MEMCONF] 35/0` + 宿主 `mem-test 81/0` |
| 4 | Debug | 5 个服务插件 `service/{trace,backtrace,hexdump,memleak,dump}`; core 侧 16 B 事件环 + ISR 发射点 | `5-01` §1–§4、`1-03` §1(仅 `service/trace`) | `[DBGCONF] 9/0` + memleak `12/0` |
| 5 | 插件管理 | **描述符段 + 相位驱动 + Kahn 拓扑 + 首败即停**: `br_plugin_manager_run()`; `br_core_main` 删除 | `1-01` §6.1/§6.2/§6.5/§9、`3-05` | `[PLGCONF] 6/0`; 段内 8 描述符 / 5 init 边 |
| 6 | 服务注册表 | `br_service_publish`/`_lookup`(+3 个观测入口), 语义照 `3-01` §9 | `3-06`、`3-01` §9 | `[SVCCONF] 3/0` |
| 7 | 调度 | **core 框架 + `sched/coop`**: TCB 池/上下文切换/trampoline/超时表/idle/首次调度; `br_sched_ops` 9 槽成文 | `1-01` §4.1/§5.1、`3-03`、`3-01` §2 | `[TASKCONF] 12/0` + 宿主 `sched-test 8/0` |
| 8 | 同步 | `mutex`/`semaphore`/`cond`/**`spinlock`(新面)** + 零分配等待链 + `br_deadline_from_now` | `3-01` §3/§4、`6-01` §3.2 | `[SYNCCONF] 12/0` + 宿主 `sync-test 36/0` |
| 9 | 工具链 | **`brickie` 构建族 6 命令 + 声明面四处 + 三后端 + 门禁判定**, 顶层 `Makefile` 降级为薄委派 | `2-01`/`2-02` §1+§3 BR-D4、`brickie-v0.1.md`、**设计 ADR-0003** | E-1/E-3/E-4 |
| 10 | 验证体系 | 14 条声明门禁(4 宿主 + 6 QEMU + 4 脚本)+ `[build].post` 2 条 + 8 套一致性摘要 | `1-02` §2.3(三层门禁)、`6-01` | E-2(全绿) |

### 1.1 Platform 域(`platform/qemu-aarch64`)

| 交付物 | 落点 | 与设计的口径 |
|---|---|---|
| reset / EL2→EL1 / 栈 / 异常向量 / BSS 清零 | `platform/qemu-aarch64/src/start.S`、`vectors.S` | 形态一致; 槽内跳板 + 槽外桩体(一槽 0x80 B 放不下完整帧, 裁定 `P-IRQ-ASM-1`) |
| 早期 console(PL011 **轮询**) + arch timer 读数 | `console_pl011.c`、`timer_arch.c` | `1-01` §8 的 console 双形态第一形态; **设计内行为, 不登记 WORKAROUND** |
| GICv3 驱动(`br_pic_ops` 方言)+ 板级绑定表 | `gicv3.c`、`board_irq.c` | 方言代码暂居 platform 目录(`br-wa-isa-001`) |
| **4 KiB 恒等映射**: 4 级页表 / 仅 TTBR0 / T0SZ=25 / IPS=40 位 / **7 张静态页表(28 KiB)** | `mmu.c` | 逐页 4 KiB 是 `set_attrs`(RO/NX)与 v2 重定位按页生效的前提(原型 `P-MM-1`; 页表张数与体积取自该登记, 本篇未逐字复核 `mmu.c` 的数据结构) |
| 低 1 GiB = 512 条 2 MiB **Device 块**; RAM 开 **8 MiB** 的 4 KiB 页窗; 窗口外 invalid ⇒ 可恢复翻译 fault | `mmu.c` | `P-MM-2`/`P-MM-3`(原型登记的理由: GICR 第 4–8 帧排到 `0x081A0000`, 只映射 GICD 附近会漏 —— 该地址表本篇未逐字复核) |
| **region 表** + 三池认领(heap 1 MiB / contig 256 KiB / page 1 MiB / DMA 256 KiB / 保留 16 KiB) | `memmap.c` | **池比例写死**在平台数据里, 未经 `[budget]`/`[[res]]` 生成 ⇒ `br-wa-mem-001` |
| 声明面: `[build]`(源/包含)+ `[build.target]`(arch/交叉前缀/arch 标志/链接脚本 + `[build.target.qemu]`) | `plugin.toml`、`src/link.ld` | `1-03` M0 的"platform 早期 console, 不注册设备"与之一致 |

**镜像形态**: `build/brick.elf` = text **117472** / data **101** / bss **121632**(§6 E-5)。

> **未落地的平台件**: `platform/host`(v1.0 清单第 2 件, M3)完全未做 —— 现阶段"宿主能跑"指的是**把 core 的 mem/sched/sync 编成宿主可执行跑算法判据**(4 条宿主门禁), **不是** host 平台插件(`1-01` §8 的三层模式没有第二个实现)。这是 §3.2 的一整行差距。

### 1.2 中断域(`3-02` Stage 1)

- **Stage 1 全量**: 号空间 / 描述符池 / 生命周期(ack → ISR → eoi **单出口**)/ 三层屏蔽 / 优先级语义(含 GICv3 的 `PRIbits` 解码与回读断言)/ 触发语义 / **FAST 级联域** / fault 分类 + extable + panic(bare 路径 + double-fault 兜底)/ 最小 trace 环。
- **逐用例**: `[IRQCONF] SUMMARY pass=67 fail=0 total=67`; 其中 `TC-IRQ-001…004` 与 `TC-IRQ-101`(FAST 域: 分发/逐子 ack/无属主子中断被 mask)是**真跑**, 不是静态检查。
- **两处明确的分期边界**(设计明示, 不算缺陷):
  - **SLOW 级联域不可用**: `br_irq_domain_create(SLOW)` 返回 NULL 并留 trace(`3-02` §1.1.1: SLOW 域的存在前提就是 bh); 原型把它断言成"拒绝", **不是** `6-01` `TC-IRQ-102` 期望的"双上下文契约成立"。
  - **`br_fault_handler_register` 未实现**(仅签名): `3-02` §10.5 把注册面归 v2(ramdump 的挂钩点)。
- 新增的核心符号(`br_pic_register` / `br_irq_bindings_set` / `br_pic_ops_t` / `br_mm_ops_t` 等平台侧契约)走"分类登记", 不动 CA-5 的面计数。

### 1.3 内存域(`3-04` + `3-01` §6/§7)

| 面 | 实况 | 口径 |
|---|---|---|
| 签名面 | `3-01` §6 的 **11 函数 + §7 的 5 函数与设计逐字一致** | `P-MEM-1` / `P-MM-1`…: **native 面零增量** |
| TLSF | 二级位图 32×32; `BR_MALLOC_ALIGN = 16` | AAPCS64 的 SP 16 对齐 ⇒ 结构体自然对齐上界 16 |
| 用户块布局 | `[头 32B][用户区][红区 0xA5][尾 8B]`; **双 free 检出后直接返回** | `5-01` §4 的"TLSF 红区 + freelist 毒化"落地; 二次入链会毁堆, 拦截比报错重要(`P-MEM-3`) |
| region 种类 | 位域 `attrs[11:8]`(`BR_MM_KIND_*`) | `3-01` §13.6 只给了 `regions` 轴取值域、没给**载体**; 原型用 kind 位承载(`P-MEM-4`) |
| 页池 | 位图按 `BR_PAGE_MAX_PAGES = 4096` 静态预置(16 MiB 上界, 位图 512 B); 超界在 `br_mem_init` 就 `-ENOMEM` | 不静默截断 |
| `map`/`unmap` | 返回 `-ENOTSUP` | **正是 `6-01` `TC-MM-003` 的期望**(签名先行, 实现归 v2 重定位) |
| 属性改写 | `br_mm_set_attrs`(真 RO 保护验证) | `P-MM-4`: 是 v2 重定位通路在 v1 的演练 |
| `br_mm_query` | **只报页表真实属性**("这块是什么"归 region 表) | `P-MM-5`: 两处不互相冒充, 否则"属性"有两个真值 |
| NX | **只做描述符读回验证**, 不做真取指 | `P-MM-6`: executing-NX 走 instruction abort, extable 只覆盖 data abort ⇒ 会 panic, **不假装测过** |
| 记账 | `br_heap_owner_*` / `br_heap_walk` / `br_heap_stats_get`(归属标签) | `br-wa-debug-001`: 是 v2 per-plugin arena 的**近似**, 无预算上限/无强制归属/无 OOM 策略 |

### 1.4 Debug 域(`5-01`)

| 插件 | 职责 | 设计出处 |
|---|---|---|
| `service/trace` | 16 B 定长事件环(`BR_TRACE_RING_SIZE = 256` 条 ⇒ 4 KiB)的**唯一消费方**: drain/解码/计数/呈现 | `5-01` §1 |
| `service/backtrace` | x29 帧链捕获(**只捕获不符号化**, host 离线解码; 编译前提 = `-fno-omit-frame-pointer`) | `5-01` §3(离线分析面) |
| `service/hexdump` | **87 B/行的格式契约**(自己渲染, 不依赖 `br_log`; 用例逐字节断言) | `5-01` §2/§3 的呈现面 |
| `service/memleak` | 按归属标签出账(红区/毒化/双 free 已由 core 落地) | `5-01` §4(v1.x 便宜层 + v2 arena 的近似) |
| `service/dump` | 现场编排 + `[DBGCONF]` 入口; 边界判定 = **region 表** | `5-01` §2/§3 |

**三处必须说清的形态差异**:

1. **trace 环留在 core**(`core/src/trace.c`), 插件是消费方 —— 与 `5-01` §1"Service 插件拥有静态环"**不同**。裁定 `P-DBG-1`: CA-3 白名单要求中断/fault 路径**不依赖服务 init**, 故环与 ISR 发射点在 core; "可整层移除"由消费侧满足。
2. **`dump`/`trace` 直写早期 console**, 未经 `5-01` §2 的 debug bridge(COBS + CRC16 成帧 + `MEMRD`/`TRACE_READ` 命令面, M3)⇒ `br-wa-debug-002`。
3. **`memleak` 是归属标签, 不是 v2 的 per-plugin arena 记账**(无预算上限 / 无强制归属 / 无 OOM 策略)⇒ `br-wa-debug-001`; 这一件在 `1-03` §1 的 v1.0 清单里**本不存在**, 属**超出清单**的交付(见 §3.2)。

### 1.5 插件管理器 + 启动链(0.x.0 新增, `3-05`)

| 面 | 实况 | 证据 |
|---|---|---|
| 段 | `.br_plugins` = **独立输出段**(不是折进 `.rodata`), `KEEP` + `ALIGN(8)`; 边界符号 `__br_plugins_start/__br_plugins_stop` 用**普通赋值**(非 `PROVIDE`) | `link.ld`; 日志 `manager: .br_plugins 段 8 条描述符` |
| 描述符 | 8 条 × **96 B** = `0x300 B`; `br_plugin_t` **14 字段**(`name`/`plugin_type`/`subkind`/`phase_self`/`ver[4]`/`api_rev`/`sched_class`/`deps`/`abi_id`/`api_syms`/`res`/`early_init`/`init`/`start`), `aligned(8)`, 由**生成物**具名初始化(不走 `BR_PLUGIN` 宏) | `[PLGCONF] descriptors=8 gen_total=8`; `br_plugin.h` |
| 描述符内的形状 | `br_dep_t = {name, kind, phase, compat_gen}`(**区间不烧进镜像**); `br_res_t = {ram_kib, stack_kib}` | 与 `1-01` §6.1 的 `{name, range, phase, compat_gen}` 与"RAM/栈/IRQ/DMA"**不同** ⇒ O-8 |
| 运行期未使用的字段 | `abi_id`/`api_rev`/`api_syms` 无读取点(`ver` 亦仅呈现); `sched_class` **只记录不执法**(执法在组合期) | 设计 D14 的"Day1 存在、v2 生效"口径; 已登记 |
| 边与拓扑 | **5 条 init 边**(只取 `kind=init`); 运行期 Kahn 拓扑; 环 ⇒ 打印**完整环路径**后 panic | 日志 `init_edges=5`; `[PLGCONF] PASS TC-PLUG-002 拓扑序满足全部 init 边` |
| 相位 | EARLY(第一步**显式取 platform**)→ CORE → LATE → **开中断** → START(**两趟: 非 APP 全跑完 → APP 最后**) | 日志 `phase=early/core/late/start` 轨迹; `TC-PLUG-003` 两条断言 |
| 失败策略 | 首个 `init` 失败 ⇒ **启动失败**, 不继续后续插件 | `plugin_mgr.c` |
| 启动链 | `start.S` → `br_plugin_manager_run()`; **`br_core_main` 与 `br_main.h` 已删除** | `start.S` 无 `br_plat_early_init`/`br_core_main` 直调 |
| 注销 | `br-wa-entry-001` 四条退出条件**全部满足** ⇒ **台账注销** | `WORKAROUNDS.md` "已注销" 表 |

**仍欠的三件**(`br-wa-boot-001`, 台账原文): ① **没有独立的 `core.init` 入口** —— 堆/中断框架/注册表/调度对象的初始化仍借 `platform.early_init` 与管理器头部; ② APP 仍**直读平台身份**(`br_plat_name/isa/timer_ticks`, 只为启动日志与 `irq_ticks` 判据)⇒ `product.toml` 还剩**一条** `allow_edges` 豁免; ③ **日志/trace 未经服务注册表**(与 `br-wa-debug-002` 同源)。

### 1.6 服务注册表(0.x.0 新增, `3-06`)

- 语义逐字照 `3-01` §9: 只管**名字 → 指针**, 不解释 ops 类型; 重名 ⇒ `-EEXIST`(不覆盖 —— 覆盖会让"谁是真身"取决于启动顺序); 表满 ⇒ `-ENOSPC`; 空 name/空 ops ⇒ `-EINVAL`; 查不到 ⇒ `BR_NULL`。
- 新增 3 个**观测**入口(`br_service_count`/`_name_at`/`_lookup_hits`), 登记在原型 ADR-0005 §2.7。
- `[SVCCONF] SUMMARY pass=3/0`。
- **诚实结论: 机制已落地, 真实用户为零** —— 全树唯一的发布者是 core 自己的自检(`core/src/svc/svc.c`, 条目名 `selftest.*` 并在收尾白盒移除)。`3-06` §2 大纲第 3 项的"fd 表 / socket 表 / trace 环"三类共享状态主人**一个都还没服务化**。

### 1.7 调度框架 + `sched/coop`(0.x.0 新增, `3-03`)

| 面 | 归属 | 实况 |
|---|---|---|
| 上下文切换 asm / TCB 公共头 / 栈与 **trampoline** / 超时唤醒框架 / idle / 首次调度 | **core** | 与 `3-01` §2.2 的既有口径一致 |
| 就绪队列 / `pick_next` / tick 策略 / 抢占决策 / 调度表 | **调度插件** | `sched/coop`(`sched_kind = "coop"`, `subkind = scheduler`, 恰 1) |
| **阻塞与唤醒机制 / 锁对象与语义** | **core**(0.x.0 的裁定, 见 §2 O-14) | `br_sync.h`; 插件只回答"下一个人是谁" |
| `br_sched_ops` 槽位 | — | **9 槽成文**: `name`/`kind`/`tcb_size`/`thread_ready`/`thread_block`/`pick_next`/`on_tick`/`sleep_until`/`idle`(草案里的 `mutex_*`/`sem_*`/`work_submit` **不进 ops**) |
| 唤醒握手 | core | 等待方**先登记 `wait_status` 再阻塞**(ICB 握手)⇒ 关闭"push 等待队列 → 真正 block"之间的丢唤醒窗口 |
| 宿主侧判据 | — | 宿主 `sched-test` **8/0**, 内含 `[TASKCONF] 12/0`、idle 路径(100 次)与"测试线程全部被 join 回收" |
| 目标侧判据 | — | `[TASKCONF] 12/0`; 两线程**严格交替 `ABABABAB`**; `TC-TASK-101` 证明**不 yield 的忙线程不被抢占**(coop 的语义锚点) |
| `sched_class` 执法 | 组合期(`brickie-core`) | ✅ 已落地: `COOP_ONLY` 与闭包内调度器的 `sched_kind` 不符 ⇒ 红; `TT_SAFE` 缺 `[sched.tt]` 周期元数据 ⇒ 红(`check.rs`) |
| **"闭包内 `subkind = scheduler` 恰 1"** | — | ❌ **无组合期检查** —— 现在只靠运行期 `br_sched_register()` 二次注册 panic; 而 `1-01` §6.3 表 A 明写"`subkind = scheduler` 恰 1" ⇒ **D10 双保险缺了组合期那一半**(小差距, 可门禁化) |

### 1.8 同步原语(0.x.0 新增, `3-01` §3)

- 四类对象: `mutex`(magic/`locked`/`owner`/`waiters`/`head`)、`semaphore`、`cond`、**`spinlock`(设计 0 命中的新面)**; 静态宏 `BR_MUTEX_DEFINE`/`BR_SEM_DEFINE`/`BR_COND_DEFINE` 齐备。
- **零分配等待链**: 等待者挂在 TCB 的 `wait_next` 上, 同步路径不 `malloc`。
- 超时三态(CA-4): `INF`/`ZERO`/相对值; `br_deadline_from_now` 含**饱和不回绕**。
- `[SYNCCONF] SUMMARY pass=12/0`; 宿主 `sync-test` **36/0**(含压测收尾: 等待链为 0 ⇒ 无幽灵节点)。
- **`br_mutex_lock_to(m, ZERO)`**: 未锁 ⇒ `0`; 已锁 ⇒ **`-ETIMEDOUT`**(`6-01` `TC-SYNC-002` 的 INV-1 期望; 冻结头初版写 `-EBUSY`, 属偏离, **已纠正**; 面外的 `br_spinlock_trylock` 仍用 `-EBUSY`)。
- **实测超时行为**(`1d60a15` 干净树日志, 供 O-18 佐证): `lock_to request=20000us measured=96939us`、`sleep(1000us) measured=99520us`(`build/logs/plugin.log` 的 `[SYNCCONF] MEASURE` 行)—— 均**不早醒**(INV-2), 且量级由 **100 ms 周期 tick** 决定。**同一构建的不同 QEMU 运行会有出入**(`build/logs/sched.log` 的同型行为 96755 / 99056 us; 原型 ADR-0007 §4 另记 96.8 / 99.5 ms) —— 差异来自每次开机的 tick 对齐相位, 不是行为变化; 引用数字时**别跨平台混用**: 宿主侧是 1 µs/虚拟拍的精确值(`measured=20000us`)。

### 1.9 构建工具链 `brickie`(0.x.0 新增, `2-01`/`2-02`/`brickie-v0.1.md`)

| 面 | 实况 |
|---|---|
| 分层 | L5 Python(参数/编排/落盘/呈现, **零判定**)+ L0/L1 Rust `brickie-core`(全部判定与错误码)+ L2 C++ `brickie-gen`(只做模板渲染); JSON over stdio, `protocol_version = 1` 启动握手 |
| 入口形态 | 单文件自包含 ELF(`brickie` 启动器 + 嵌入载荷); 种子进库 `prebuilts/seed/brickie/<host-arch>/<host-os>/bin/{brickie,brickie-core,brickie-gen}` |
| 命令面 | **29 个叶子命令 = 23 组合期 + 6 构建族**(`build`/`clean`/`run`/`size`/`disasm`/`test`)+ 2 全局开关(`--version`/`--json`) |
| 声明面(唯一真值) | 插件自身源/包含/宏 = 各插件 `plugin.toml [build]`; **目标事实** = platform 的 `[build.target]`(+`.qemu`); **产品策略** = `product.toml [build]`/`[build.release]`; **门禁判据** = `tests/gates.toml` |
| 判定归属 | 工具候选序与**解析结果**、门禁红绿、生成物正文全在 core; L5 只执行 argv/并发/日志/短路 |
| 增量 | argv 指纹 + 输入 mtime + 上游 `dirty` 传播; 稳态复跑 `steps_todo = 0` |
| 后端 | `direct`/`make`/`ninja` 三后端;**同一干净树上 ELF + BIN 逐字节一致**(设计 ADR-0003 的 S2 出口) |
| 门禁正则 | 子集 `. * + ? [...] \x`; **拒绝** `( ) \| ^ $ { }` ⇒ `BRV-BLD-0012`(**绝不静默不匹配**) |
| 诊断码族 | `BRV-BLD-0001..0013`(新码族) |
| 写路径纪律 | 机器只写 `api/iface/**`、`brickie.lock`、`build/**`; `plugin.toml`/`product.toml` 是**人的财产** |
| `Makefile` 降级 | 缺省目标 = **编工具**; 镜像侧 12 条目标是**薄委派别名**; 无 `core/src`/`platform/**` 字面路径(由 `tools/check-build.sh` 的**六条不变量**执法) |
| 接口面产物 | `api/iface/` 下 **7 个 TOML**(platform `plat`/`gicv3` 2 个 + service `trace`/`backtrace`/`hexdump`/`memleak`/`dump` 5 个)+ `CHANGELOG.md` |

> **与设计演进序的关系**: `brickie-v0.1.md` §14 把"接口依赖扫描检查"排在 **v0.2**、"编译"排在 **v0.3**、"test"排在 **v0.4**、"run"排在 **v0.5**。0.x.0 交付了**后三阶**(提前), **v0.2 未做** ⇒ 见 §2 的 O-4 与 §3.7。

### 1.10 门禁与验证体系

`brickie test --list` 实测(**声明面** `tests/gates.toml`):

| 档 | 条数 | 名单 |
|---|---|---|
| 宿主用例 | **4** | `sync-test`、`string-test`、`mem-test`、`sched-test` |
| QEMU 门禁 | **6** | `smoke`、`irq-test`、`dbg-test`、`sync-test`、`plugin-test`、`sched-test` |
| 脚本门禁 | **4** | `check-string`、`check-headers`、`check-workarounds`、`check-build` |
| 构建后自动跑 | 2 | `check-string`、`check-headers`(`[build].post`) |

一致性摘要(**实测于 `1d60a15` 干净树, 取自 `build/logs/plugin.log`**):

| 摘要 | pass/fail/total | 备注 |
|---|---|---|
| `[IRQCONF]` | 67 / 0 / 67 | 含 FAST 级联域真跑 |
| `[MEMCONF]` | 35 / 0 / 35 | 含 `TC-MM-001…003` |
| `[DBGCONF]` | 9 / 0 / 9 | 调试域主套件 |
| `[DBGCONF] memleak` | 12 / 0 / 12 | 独立小计 |
| `[PLGCONF]` | 6 / 0 / 6 | 0.x.0 新增 |
| `[SVCCONF]` | 3 / 0 / 3 | 0.x.0 新增 |
| `[TASKCONF]` | 12 / 0 / 12 | 宿主侧同为 12/0 |
| `[SYNCCONF]` | 12 / 0 / 12 | 宿主侧另有 36 项压测 |

宿主门禁计数: `sched-test` 8/0(内含 `[TASKCONF] 12/0`)、`sync-test` 36/0、`mem-test` 81/0、`string-test` 6/0。

> **⚠ 用例 id 的账**: 三套目标侧套件(`[TASKCONF]`/`[SYNCCONF]`/`[PLGCONF]` 等)的 `TC-*` id 是**自编号**, 与 `6-01` §3 的用例表**尚未逐条对齐**; 日志里 id 后面的**自述文字是准确的**。原型已把这笔债登记为 `br-wa-test-001`, 逐条差异见 §3.5。

### 1.11 装配形态(数字)

| 量 | 值 | 来源 |
|---|---|---|
| 插件(闭包内) | **8** | `brickie check`: `闭包: 8 个插件, RAM 合计 16 KiB, 栈合计 4 KiB` |
| init 边 | **5**(只取 `kind=init`) | `[PLGCONF] init_edges=5` |
| 描述符 | 8 × 96 B = `0x300 B` | `[PLGCONF] descriptors=8 gen_total=8` |
| 生成物 | 8 个文件(重算逐字节一致) | `brickie gen --check` |
| core 公共头 | 17 个(`core/include/br/core/*.h`) | E-7 |
| core 公共符号 | `br_*` 函数去重 **140**(裸 grep 为 141, 多出的一个是 `br_pic.h` 的函数指针 typedef 误匹配; 见 E-7) | E-7 |
| 源文件 | 36(`core`/`platform`/`sched`/`service`/`app` 下的 `.c`/`.S`) | E-7 |
| 镜像体积 | text **117472** / data **101** / bss **121632**(`build/brick.elf`) | E-5 |

---

## 2. 偏离与裁定台账(设计原文 → 实况 → 裁定)

> **读法**: 每条 = 一处"设计这么写、实现那么做"的落点。**不是所有偏离都是缺陷** —— 有些是设计留给实现的空位(草案里的省略号), 有些是分期边界, 有些是**必须回灌**的口径修正。逐条的三篇专题决策记录: [ADR-0006](../decisions/0006-build-declaration-surface.md)(O-1…O-6)、[ADR-0007](../decisions/0007-plugin-manager-runtime.md)(O-7…O-13, 含 O-8b/O-13b)、[ADR-0008](../decisions/0008-sched-framework-and-sync-in-core.md)(O-14…O-20, 含 O-15b/O-15c; 该篇 §4 的 T1–T7 是逐条口径差异清单); O-21 起是原型既有 ADR 已登记的, 本篇只引用不重复。

### 2.1 构建族(§1.9)

| # | 设计原文 | 0.x.0 实况 | 裁定 |
|---|---|---|---|
| **O-1** | `brickie-v0.1.md` §0: "v0.1 的任何命令**不得**要求 `cc`/`cargo`/`nm` 在场" | 构建族**必须**有交叉工具链在场 | 该纪律约束的是**组合期命令**(`check`/`dep`/`iface`/`ver`/`gen`/`new`/`init`); 构建族的职责**就是**驱动工具链("要求编译器在场"是它的定义, 不是纪律的例外条款)—— 裁定 R-13 |
| **O-2** | `2-02` 未规定工具解析归属 | 候选序(`<cross>gcc` → `<cross>gcc-16…` → `cc`)与**解析结果**在 core | 生成物(`build/gen/build.mk`/`build.ninja`)**必须把解析结果烧进去** —— make/ninja 不会去猜 `aarch64-linux-gnu-gcc-16`; L5 只执行 argv |
| **O-3** | `1-02` §2.3 三层门禁"由 `brickie` 编排"(未说谁判) | 门禁**红绿判定在 core**(judge 按 `gates.toml` 的正则判日志) | "是否通过"只有**一处真值** ⇒ `--json`/文本/CI 退出码同源 |
| **O-4** | `brickie-v0.1.md` §14: v0.2 = 接口依赖扫描检查, **v0.3 = 编译**, **v0.4 = test**, **v0.5 = run** | 原型在 **v0.2.0** 就交付了"编译 + test + run"三阶(命令族 6 条); v0.2 的**符号级接口依赖扫描未做**(`truth`/`hash_scope` 仍只允许 `decl`); `run` 只有 QEMU 后端 | 演进序在原型侧**被重排**(提前 v0.3–v0.5, 跳过 v0.2); 设计 §14 需要回灌或显式标注偏离(对齐设计 ADR-0003 §5-1 的"版本号待拍") |
| **O-5** | `2-02` §1 把"构建编排"划给 `brickie`(未说声明面放哪) | **四处声明面**: 插件 `[build]` / platform `[build.target]` / 产品 `[build]` / `tests/gates.toml` | "一处事实一个可写处" —— 门禁判据以前散在 `Makefile` recipe 里, 那是第二处真值 |
| **O-6** | 设计 ADR-0003 §3 的 **S4**: "`make` 的唯一产物是宿主工具"; §6: "过渡期两条路径必须并存" | 缺省目标已 = 编工具, 无字面路径; 但 **`Makefile` 未删除**, 12 条薄委派别名保留 | S4 达成的是"**产物归属 + 零字面路径**", **不是**"删除 Makefile" —— 与 §6 的并存约定一致 |

### 2.2 插件管理器族(§1.5/§1.6)

| # | 设计原文 | 0.x.0 实况 | 裁定 |
|---|---|---|---|
| **O-7** | `1-01` §6.2/§9 只写"EARLY 第一、LATE 后开中断、`start` 全部插件 → APP" | 运行期补两条硬规则: ① EARLY **第一步按 `plugin_type == PLATFORM` 显式取**; ② START **两趟, APP 最后** | 不靠拓扑序巧合 —— 实测零 init 边时 `app/hello` 按段序会排**第一**, 若靠巧合会把 APP 的 MainLoop 放到 platform 的 timer bring-up 之前 |
| **O-8** | `1-01` §6.1 描述符 11 字段(`name`/`ver[4]`/`api_rev`/`sched_class`/`deps`/`abi_id`/`api_syms`/`res`/三个回调)+ 宏 `BR_PLUGIN(...)` + `aligned(4)` | 实况 **14 字段**(增 `plugin_type`/`subkind`/`phase_self`)、**`aligned(8)`**、宏名 `BR_PLUGIN_DEFINE(...)` 且生成物用**具名初始化**(宏形参装不下新字段) | 描述符是**最高门槛**面(`1-02` §2.2); 新增三字段是**必要补充** —— 管理器不能靠命名约定猜类别("EARLY 第一步必须是 platform"要知道谁是 platform) |
| **O-8b** | 同节 `br_dep_t = {name, range, phase, compat_gen}`、`br_res_t` = "RAM/栈/IRQ/DMA" | 实况 `br_dep_t = {name, **kind**, phase, compat_gen}`(**版本区间不烧进镜像**)、`br_res_t = {ram_kib, stack_kib}` | 运行期不需要区间(闭包与区间在组合期已解), 但**形状差异必须写进设计**, 否则"布局契约"(D14)有两处真值 |
| **O-9** | `3-01` §13.3 / `3-05` §2.2 提"链接脚本 `PROVIDE` 边界符号" | 用**普通赋值** + **独立输出段** + `KEEP` | `PROVIDE` 的"已有定义就不覆盖"会**掩盖接线错误**; 独立输出段是为了 `objdump -h` **看得见**段是否存在/多大 |
| **O-10** | `1-01` §9 有 `core.init` 一格; `3-04` §3 开放问题: "堆初始化点 = `core.init`(早于 EARLY 相)" | 无独立 `core.init`; 初始化借 `platform.early_init` 与管理器头部 | `br-wa-boot-001` 还债项 ① (M2/M3 前置) |
| **O-11** | `3-06` 注册表语义(发布 = 服务方 `init`, 查找 = 依赖方 `init`) | 机制照做; 但**唯一发布者是 core 自检**, 日志/trace 未服务化 | `br-wa-boot-001` 还债项 ③; `3-06` §2 大纲第 3 项(fd/socket/trace 三主人)**全部未落地** |
| **O-12** | `3-06` §3 **D7**: Service 插件边界判据(与 `11-01` 共同落定) | **仍未收敛** | **设计侧欠债**, 不是原型欠债; 是 `1-03` §5 DoD 第 1 项的一半 |
| **O-13** | `6-01` §3 有 9 个用例组(TC-TASK/SYNC/TIME/WORK/MEM/MM/IRQ/SVC/HSM), **没有 TC-PLUG / TC-DBG 组** | 原型自编号并打印 `[PLGCONF]`/`[DBGCONF]` | `br-wa-test-001`: 要么 `6-01` 补组, 要么把"自述 id"写进口径(§3.5) |
| **O-13b** | `1-01` §6.5 末句(第 453 行): "**运行期不做任何检测**(纯静态, **构建期全解**)" | 运行期**重算** Kahn 拓扑序, 并在**环**时报完整路径 panic; 还有拓扑序自证(`TC-PLUG-002`)与相位单调自证(`TC-PLUG-003`) | 排序必须在运行期做(镜像里没有生成物给的顺序), 环检测是它的副产品; **但"构建期全解"的字面读法指向另一条路**: 生成物可以把**已排序的描述符序列**烧进镜像, 运行期只做走查。两条都成立 —— 设计侧需要**选一条并写明**(选前者 ⇒ 运行期排序是契约的一部分; 选后者 ⇒ `1-01` §6.5 不用改)。**权威判定仍在组合期**(`BRV-DEP-0009/0010` 在 `brickie-core`) |

### 2.3 调度与同步族(§1.7/§1.8)

| # | 设计原文 | 0.x.0 实况 | 裁定 |
|---|---|---|---|
| **O-14** | `3-01` §3 标题/正文(第 115 行): 同步原语"**实现在调度插件**"(把 `sem_take`/`mutex_lock` 放进 `br_sched_ops`); 锁对象布局 = core 公共头 + **调度插件私有尾**(CA-2) | 同步原语**实现在 core**; 插件只提供 `thread_ready`/`thread_block`/`pick_next`。**CA-2 只对 TCB 仍成立, 对锁对象不再成立**(锁对象全收敛在 core) | 三条理由: ① 锁对象要被**所有**插件看见, 放插件头会逼出"插件互依赖"; ② 阻塞/唤醒机制对 coop/preempt **是同一套**, 只有"谁先跑"不同; ③ 设计的 ops 表本身就是**带省略号的草案**。**对外语义一字不改**(签名/错误码/ISR-safe 的 `br_sem_give`) |
| **O-15** | `1-01` §5.1 的 `br_sched_ops` 草案(含 `thread_sleep`/`mutex_lock`/`sem_take`/`work_submit`/…) | **9 槽成文**: 增 `tcb_size`/`sleep_until`/`idle`; `thread_sleep(t, rel)` → **`sleep_until(t, abs_us)`**; 锁面与 work 面移出 | `3-03` §2 大纲第 1 项(DoD 第 3 项)的**表已成文, 但只有 coop 一列**(T7) |
| **O-15b** | `3-01` §2.2(第 111 行): `current` 指针放 **`TPIDR_EL1`**(core 独占; v2 per-CPU) | 实况 = core **静态量 `s_current`**; `TPIDR_EL1` 是 v2/SMP 的落点 | 单核下静态量足够且省一次系统寄存器访存; **"放哪个寄存器"是实现细节, 但设计原文写了具体寄存器** ⇒ 需回灌为"实现可选" |
| **O-15c** | `3-02` §17.4(第 1702 行): `br_sched_ops` 须增加 **`irq_epilogue` 槽位**(不能是 hidden 函数) | 实况 = core 内部函数 **`br_sched_irq_epilogue()`**(非 ops 槽位); `core/src/irq/irq_core.c` 的退出调用点未动 | 与 O-14 同源(机制在 core ⇒ 不需要跨插件槽位); 需回灌 `3-02` §17.4 |
| **O-16** | `6-01` `TC-SYNC-002`: `lock_to(ZERO)` 已锁 ⇒ `-ETIMEDOUT` | 冻结头初版写 `-EBUSY` ⇒ **已按 `6-01` 纠正** | 设计是验收契约; 实现与它冲突时改实现 |
| **O-17** | **本轮新增文档之外**的既有设计正文(`1-01`/`1-02`/`3-01`/`3-03`/`6-01` 等)`spinlock`/`自旋` **0 命中**; 命中只落在 `Design/comment/review2/02-os-core-review.md:48/50/52`(评审存档, 其中 `:50` 正是 P1 预警"跨核自旋锁没有任何文档认领归属", `:52` 建议"预先立项决策记录") | 新增 `br_spinlock_t` 与 **6 个函数**(`init`/`lock`/`trylock`/`unlock`/`is_locked`/`contention`)+ 2 宏 | 走 **CA-5 面预算登记**: 计入 `br-sched` 组后 **21 → 27**, 总面 **48 → 54** ⇒ **超出 CA-5"面目标 ≤50"上限 4**。设计侧必须做一次面预算裁定(扩上限 / 把 `spinlock` 归入别处), 否则"面预算"就成了糊涂账。它不是 `br_irq_lock` 改名 —— 后者在 SMP 下只关本核中断、失去跨核互斥效力, 而 v2b 需要 ISR 上下文的跨核保护 |
| **O-18** | `3-03` §1 与 `1-03` §3 M1 写 **tickless** | **平台 100 ms 周期 tick** + 绝对期限队列; 保证**不早醒**, **晚到无上界**。实测: `lock_to(20000us)` → **96939 us**、`sleep(1000us)` → **99520 us**(日志 `[SYNCCONF] MEASURE` 行; 时钟 = 62.5 MHz arch timer) | **tickless 未落地**; 由它推出: 任何睡眠/超时的**实际分辨率 ≈ 一个 tick**(上例即 ~100 ms)。`3-03` §1/§2 与 `1-03` §3 需回灌 |
| **O-19** | `3-01` §5 的 `br_work_submit`; `1-01` §5.2 的 coop 形态"事件入队、主循环分派"; `3-02` 的 SLOW 级联域 | `br_work_submit` **不存在**; SLOW 域**明确拒绝**(`domain_create(SLOW)` = NULL + trace) | `1-03` §3 M1 的"bottom half"未交付; SLOW 拒绝是 `3-02` §1.1.1 的**分期边界**(设计内行为) |
| **O-20** | `6-01` §3 的"适用调度 **ALL** = 三调度器矩阵全跑"; `1-01` §6.3 表 A 的"`subkind = scheduler` **恰 1**" | 只交付 `sched/coop`; preempt(v2)/tt(v3) 未做。调度类别执法只落了**两半**: `COOP_ONLY` 与 `sched_kind` 不符 ⇒ 红、`TT_SAFE` 缺周期元数据 ⇒ 红(`brickie-core`); **"恰 1 个 scheduler"没有组合期检查**, 只靠运行期 `br_sched_register()` 二次注册 panic | 矩阵完成度 **1/3**; D10 的"双保险"**缺了组合期那条数量约束**(可门禁化) |

### 2.4 中断 / 内存 / 调试族(原型既有 ADR 已登记, 此处只做索引)

| # | 主题 | 一句话 | 原型登记处 |
|---|---|---|---|
| **O-21** | native 面零增量 | `br-mem` 11 + `br-mm` 5 与 `3-01` §6/§7 **逐字一致**; 新增的初始化/观测/记账/平台面 16 符号**分类登记**, 不动 CA-5 的面预算 | `P-MEM-1` / ADR-0003 §2.1 |
| **O-22** | SLOW 级联域拒绝 | Stage 1 无 bh ⇒ 拒绝并留 trace, 而非假装支持 | `3-02` §1.1.1(两段计) |
| **O-23** | `br_fault_handler_register` 未实现 | 仅有签名; `3-02` §10.5 把注册面归 **v2** | ADR-0002 §4 |
| **O-24** | trace 环留 core | 与 `5-01` §1"插件拥有静态环"不同; 理由是 CA-3 白名单(中断路径不依赖服务 init) | `P-DBG-1` |
| **O-25** | region **种类载体** | `3-01` §13.6 只给 `regions` 取值域, 没给载体 ⇒ 原型用 `attrs[11:8]` kind 位 | `P-MEM-4` |
| **O-26** | NX 只做描述符读回 | 不做真取指验证(**不假装测过**) | `P-MM-6` |
| **O-27** | 池比例写死 | heap 1 MiB / contig 256 KiB / page 1 MiB / DMA 256 KiB 在平台数据里, 未经 `[budget]`/`[[res]]` 生成 | `br-wa-mem-001` |
| **O-28** | ISA 共享库未独立 | GICv3 方言 / 向量桩 / 页表构造暂居 platform 目录(靠文件边界分层) | `br-wa-isa-001` |

---

## 3. 与 v1.0 的差距

> **对标对象**: `1-03` §1 的 v1.0 "walk" 定义(含 17 件插件清单)+ `1-03` §3 的 M0–M5 + `1-03` §5 的设计 DoD + `6-01` §3 的 49 条用例 + `3-01` 的 48 函数面。
> **判定图例**: ✅ 已达成(同形态) ｜ ◐ 部分(能力在但形态/范围不同) ｜ ❌ 未开始。

### 3.0 差距总览

| 对标面 | 分母 | ✅ | ◐ | ❌ | 一句话 |
|---|---|---|---|---|---|
| `1-03` §1 的**域**行(ISA/内存/int/sched/插件管理/接口/框架件/三方移植/存储/debug/工具/并发 = 12 行) | 12 | 3 | 4 | 5 | 从"能启动的机制内核"到"有文件、有设备、有 POSIX"的整段缺席 |
| `1-03` §1 的 **v1.0 插件清单** | 17 | 2 | 3 | 12 | 缺的 12 件里, 6 件是存储/设备栈, 2 件是接口皮肤 |
| `1-03` §3 的 **M0–M5** | 6 | 1 | 2 | 3 | M0 达成; M1 差 tickless + bh; M2 未开始 |
| `1-03` §5 的**设计 DoD** | 8 | 1 | 3 | 4 | 工具与骨架里只有"构建系统"一项达成, 其余是设计侧收敛工作 |
| `6-01` §3 用例(**按设计语义**有断言) | 49 | 33 | — | 16 | 缺口逐条见 §3.5 |
| `3-01` 的 native API 函数 | 48 | 47 | — | 1 | 只缺 `br_work_submit` |
| `brickie` 演进序(`brickie-v0.1.md` §14) | 6 阶 | 2 | 2 | 2 | v0.3/v0.4/v0.5 的"编译 / test / run"被**提前**; v0.2 的符号级扫描与 v0.6 的兼容性检查**缺席** |

### 3.1 逐域差距(对照 `1-03` §1)

| 域 | v1.0 要求 | 0.x.0 | 具体差什么 |
|---|---|---|---|
| **ISA** | aarch64 / QEMU virt 先行 / EL1 恒特权 | ✅ | 达成; ISA 层未独立成库(`br-wa-isa-001`) |
| **内存** | 恒等映射(region 属性)+ TLSF 堆 + per-plugin arena + DMA 友好分配 | ◐ | 恒等映射 ✅ / 三池 ✅ / DMA ✅; **per-plugin arena 预算与归属分配 ❌**(只有归属标签 `br-wa-debug-001`); **池比例由 manifest 生成 ❌**(`br-wa-mem-001`) |
| **int** | IRQ 框架 + **bottom half(work queue)** | ◐ | Stage 1 ✅(`[IRQCONF] 67/0`); **bh ❌**(`br_work_submit` 缺席; SLOW 级联域连带不可用) |
| **sched** | `sched-coop` | ◐ | coop ✅(含 coop 语义锚点 `TC-TASK-101`、`sched_class` 的组合期执法); **tickless ❌ → 100 ms 周期 tick**; **"恰 1 个 scheduler" 无组合期门禁**(O-20) |
| **插件管理** | 依赖版本区间 + 拓扑排序 + **环检测硬错误** + 描述符 v2 | ✅(组合期 + 运行期) | 组合期(闭包/区间/相位/分类学)在 `brickie-core`; 运行期(段枚举/Kahn/相位机/首败即停)在 core。**细节待回灌**(O-7/O-8) |
| **接口** | native API + **svc-posix(D18)** + `iface-posix` 薄皮肤 + `iface-min` | ❌ | native API 面 47/48 已落; **但三件接口/运行时件(`svc-posix`/`iface-posix`/`iface-min`)全无**, 且"皮肤再导出"机制(`1-01` §7.3)在原型里**没有运行实例** |
| **框架件** | `dev-core` + `cdev-core` + `bdev-core` + `vfs-core`(D19/D20) | ❌ | 四件全无; `8-01` 的三种组合形态与 `7-01` 的四层 ops **没有一行实现** |
| **三方移植** | sqlite 双模式移植(移植性验证) | ❌ | 未开始; **这条是"战略语境"里最重的一件**(架构的生死线), 却排在 M2 之后 |
| **存储** | VFS + **tmpfs rootfs + devfs(/dev)**(D21)+ block 层 + littlefs | ❌ | 全部未开始; `/dev` 不存在 ⇒ 设备与文件两条链都没有落点 |
| **debug** | `service/trace` + 最小 debug bridge | ◐ | trace ✅(形态偏离 O-24)+ 4 件**超出清单**的调试服务; **bridge ❌**(COBS/CRC16/命令面, 现在直写 console `br-wa-debug-002`) |
| **工具** | CLI + manifest 校验 + **host 平台插件**(CI + ASan 白捡) | ◐ | CLI + manifest 校验 ✅(且**超出设计排期**交付了构建族); **host 平台插件 ❌** —— 现在只有"把 core 的 mem/sched/sync/string 编成宿主可执行"的 4 条宿主门禁, 没有 `1-01` §8 三层模式的第二个实现, 因此 **ASan 白捡这条红利尚未兑现** |
| **并发** | 单核 | ✅ | 单核; 无 SMP 需求(v2) |

### 3.2 插件清单差距(`1-03` §1 的 17 件 + APP 行)

| 插件 | 归属里程碑 | 0.x.0 判定 | 说明 |
|---|---|---|---|
| `platform/qemu-aarch64` | M0 | ✅ | 形态一致(声明面由 `brickie` 治理) |
| `platform/host` | M3 | ❌ | 未做; 宿主侧只有"算法判据编成宿主可执行", 不是平台插件 |
| `sched-coop` | M1 | ✅ | 实况名 `sched/coop`(`sched_kind = "coop"`) |
| `dev-core` | M2 | ❌ | 未做 ⇒ 设备体系 0 |
| `cdev-core` | M2 | ❌ | 未做 ⇒ `/dev` 无字符设备 |
| `vfs-core` | M2 | ❌ | 未做 ⇒ `br_file`/`br_open` 不存在 |
| `bdev-core` | M2 | ❌ | 未做 ⇒ 块存储 0 |
| `fs/tmpfs` | M2 | ❌ | 未做 ⇒ **没有 rootfs** |
| `fs/devfs` | M2 | ❌ | 未做 |
| `io/uart-pl011` | M0(轮询)/M2(tty) | ◐ | **能力在, 插件不在**: PL011 早期轮询 console 在 platform 插件目录内; 中断 tty 未做 |
| `io/virtio-blk` | M2 | ❌ | 未做 ⇒ 无设备驱动样例 |
| `fs/littlefs` | M2 | ❌ | 未做 ⇒ 无"落盘"证据 |
| `svc-posix` | M2 | ❌ | 未做 ⇒ `fd`/`stdio`/`pthread` 全无 |
| `service/trace` | M2 | ◐ | 同名插件在, 但**环在 core**(O-24): 无 "manifest 定尺寸"、无 MPSC 无锁写的公开口径 |
| `service/dbg-bridge` | M3 | ◐ | 无 COBS/CRC16 成帧、无命令集、无 panic 独立命令通道; `dump`/`hexdump` 是"直写 console"的近似 |
| `iface-posix` | M2 | ❌ | 未做 |
| `iface-min` | M2 | ❌ | 未做 ⇒ M0 的 `allow_edges` 例外(`app → platform`)因此还留着 |
| `app/hello` + conformance | M0/M3 | ✅ / ◐ | APP ✅ 已生命周期化(init/start); conformance **形态不同** —— 用例编在各 platform/service 插件内, 不是 `1-03`/`6-01` 描述的 `app/conformance` 插件 |

**0.x.0 交付但 v1.0 清单里没有的 4 件**(超出清单, 需回灌):`service/backtrace`、`service/hexdump`、`service/dump`(**`6-01` 无 `TC-DBG-*` 组**, O-13)、`service/memleak`(`5-01` §4 把 per-plugin arena 记账排在 **v2.0**; 现在交付的是"归属标签"近似)。

### 3.3 里程碑差距(`1-03` §3)

| 里程碑 | 内容 | 0.x.0 | 差距 |
|---|---|---|---|
| **M0** | 启动链 + 插件管理; 验证 = hello + init 链打印 + **环检测用例** | ✅ | 达成: 相位轨迹逐插件打印; 组合期环检测(`tools/brickie/tests/fx/cycle/`)与运行期环路径 panic 都在 |
| **M1** | sched-coop + native task API + **tickless timer** + **bottom half** | ◐ | task API ✅(7/7 能力); **tickless ❌**(O-18); **bh ❌**(O-19) |
| **M2** | 框架件四件 + tmpfs/devfs + virtio-blk + littlefs + trace ring + svc-posix + sqlite 模式 A | ❌ | 只落了 trace ring 的 core 半边; **这是最大的一段空白**, 也是 §4 的咽喉 |
| **M3** | bridge 最小集 + host 平台插件 + CLI + conformance 首版 | ◐ | CLI ✅(**超排期**, 含构建族)+ conformance 首版 ◐(8 套摘要但 id 漂移); **bridge ❌**; **host 平台插件 ❌** |
| **M4**(v1.x 选配) | 首个真实 SoC + PMIC/GPIO 级联域驱动 | ❌ | 未做(QEMU 之外无平台) |
| **M5**(v1.x) | HSM 完整样例(6 件插件 + `app/hsm`) | ❌ | `6-01` `TC-HSM-101…106` **一条未跑**; `9-02` 的 A1–A6 无证据 |

### 3.4 设计 DoD 差距(`1-03` §5 的 8 项)

| # | 待收敛项 | 0.x.0 贡献 | 现状 |
|---|---|---|---|
| 1 | D7(Service 插件边界)/ D8(中断线程化)收口 | 无(设计侧工作) | ❌ **仍开放**: D7 判据未成文(O-12); D8 的 bh 未实现(O-19) |
| 2 | native API **头文件级规格** | **47/48 函数落了实际头文件**(17 个头, 140 个 `br_*` 函数符号的签名与注释) | ◐ 素材齐了, **设计侧头文件草案仍未成文**; 且 `1-02` §2.6.4 的 golden(IFACE-IR → `br-*.txt`)在原型侧是 `api/iface/**/*.toml` 形态, **golden 管线与 CI 层未落地** |
| 3 | `br_sched_ops` 完整规格 + coop v1 语义 | 槽位表**已成文**(9 槽)+ coop 语义有一手判据(`ABABABAB`、不抢占) | ◐ **三形态差异矩阵只有 1 列**(preempt/tt 未实现) |
| 4 | 插件描述符 + manifest 格式定稿 | manifest(TOML)+ 描述符(96 B)+ 生成物**已跑通** | ◐ **原型侧定稿了, 设计侧 `4-03`/`4-04` 仍是骨架** |
| 5 | 构建系统与仓库骨架 | ✅ **主要成果**: 声明面 + 三后端 + 门禁 + 骨架生成 | ✅ 达成(`1-03` §5 第 5 项原文就是"设计文档", 设计侧 `2-01`/`2-02` 仍待成文) |
| 6 | host 平台插件架构(core+插件 → Linux 进程映射规则) | 仅"编成宿主可执行"的经验 | ❌ **未成文也未落地**; `1-01` §8 三层模式没有第二个实现 |
| 7 | M0–M5 每个里程碑的验收标准细化 | 提供了**可复制的判据形态**(`tests/gates.toml` + `[XXXCONF]` 摘要) | ❌ 设计侧仍是"一句验证目标"(M5 除外) |
| 8 | trace 事件 id 分配方案 + bridge 最小命令集 | 实现侧有 **17 个编号事件 id**(`BR_TRACE_IRQ_ENTER`=1 … `BR_TRACE_FAULT_DOUBLE`=17, 另有 `BR_TRACE_NONE` 哨兵与 `BR_TRACE__COUNT`; 它们是 **enum 常量不是宏**, 头文件共 21 个 `BR_TRACE_*` 标识符)+ 256 条环 | ❌ "**分配方案**"(谁按什么规则分配 id、manifest 如何声明)未成文; bridge 命令集未定(未实现) |

### 3.5 验证体系差距(`6-01` §3 的 49 条用例)

> 判据口径: **该条设计用例的"语义"在原型侧有没有断言** —— 不看 id 是否同名。于是分两类:
> **(a) 语义缺** = 真的没测(计入缺口); **(b) id 错位** = 语义测了但在另一个 id 名下(`br-wa-test-001` 的"含义漂移", **不计入缺口**, 但会让"按 id 检索"失效)。
> 逐条区分见下表"缺口"列。

| 组 | 设计条数 | 按设计语义有断言 | 缺口(逐条) |
|---|---|---|---|
| TC-TASK | 7 | 5 | 两条**真的没断言**: ① **`TC-TASK-003` 的"二次 join"** —— 原型只覆盖了 `join NULL/自身`(`TC-TASK-005`), 二次 join 的 `-EINVAL` 分支**无断言**(实现里有该分支, `br_task_join` 对"已被别人 join"返回 `-EINVAL`); ② **`TC-TASK-005`**(`sleep_until(已过期期限)` ⇒ 立即返回 0)—— 实现里有该分支("期限已过 ⇒ 立即返回 0"), 但宿主侧只断言了**未来**期限。另 4 条属"**能力在但 id 错位**": 设计 `002`(attr 校验)→ 原型 `005`; `004`(sleep 实测 ≥1ms)→ 原型 `TC-TIME-001`; `006`(yield 交错)→ 原型 `003`; `007`(entry 返回走 trampoline)→ 原型 `002`(与设计 `001` 同一个原型 id) |
| TC-SYNC | 9 | 8 | 一条**真的没断言**: **`TC-SYNC-003`** 的"**耗时 ≥ 1 ms**" —— 原型 `TC-SYNC-004` 断言了 `-ETIMEDOUT` + 锁随后可再取, 并**记录**了耗时(`MEASURE ... request=20000us measured=96939us`), 但没有"请求 1 ms ⇒ 耗时 ≥ 1 ms"这条断言。`TC-SYNC-009`(静态 `BR_MUTEX_DEFINE` vs 动态 `init` 等价)**已覆盖**, 但走的是**另一个 id**: 宿主侧 `TC-SYNC-STATIC`("静态/动态对象行为等价")。另: 原型 `TC-SYNC-003` = "非持有者 unlock ⇒ `-EPERM`"(设计未列的**新增**用例) |
| TC-TIME | 3 | 2 | **`TC-TIME-002`(单位校准: `sleep(1s)` 对 host 参考钟 ≈1.0–2.0 s)未做**(原型该 id 被 `sleep(0)` 占用); `TC-TIME-003` 的"**deadline 饱和不回绕**"**有**断言(与 `sleep(INF)` 同一条 PASS 打印) |
| TC-WORK | 4 | 0 | **整组未做**(无 bh / 无 `br_work_submit`)⇒ O-19 |
| TC-MEM | 7 | 7 | —(宿主 `mem-test` 81/0 + 目标 `[MEMCONF]`) |
| TC-MM | 3 | 3 | —(含 `TC-MM-003` 的 `-ENOTSUP` 期望) |
| TC-IRQ | 7 | 5 | **`TC-IRQ-005`**(ISR 内 `malloc` 的运行时断言)**未做**; **`TC-IRQ-102`**(SLOW 域双上下文)**不可用** —— 原型断言的是"明确拒绝", 与设计期望相反。`TC-IRQ-101`(FAST 域)✅ 真跑; 另有约 10 条原型自编号用例(`TC-IRQ-008/010/012/…`) |
| TC-SVC | 3 | 3 | —(但**零真实用户**, §1.6; 另 `6-01` §3.8 把这三条的平台列标 **host**, 而原型的 `br_service_conformance()` 跑在 **target** ⇒ **平台口径需一并定**) |
| TC-HSM | 6 | 0 | 整组未做(M5) |
| **合计** | **49** | **33**(67%) | 16 条无按设计语义的断言; 另有一批"能力在但 id 错位"的条目 |

### 3.6 native API 面差距(`3-01`)

| 面 | 设计 | 原型 |
|---|---|---|
| `br-sched` 组(21) | 任务 7 + 同步 11 + 时间 2 + 工作 1 | **20/21**: 只缺 **`br_work_submit`** |
| `br-mem` 组(11) | 堆/contig/页/DMA | **11/11** |
| `br-mm` 组(5) | map/unmap/region_add/cache×2 | **5/5**(`map`/`unmap` 按设计返回 `-ENOTSUP`) |
| `br-irq` 组(9) | 中断 + 级联域 | **9/9** |
| `br-svc` 组(2) | publish/lookup | **2/2** |
| **合计** | **48** | **47/48**(97.9%) |
| 3 个静态宏 | `BR_MUTEX_DEFINE`/`BR_SEM_DEFINE`/`BR_COND_DEFINE` | **3/3** 齐备(等价性由宿主 `TC-SYNC-STATIC` 覆盖, 只是没走 `TC-SYNC-009` 这个 id, §3.5) |
| 面外新增 | — | core 公共头共 **140** 个 `br_*` 函数(已剔除 `br_pic.h` 的函数指针 typedef 误匹配; 见 E-7)⇒ **约 93 个设计清单外的符号**(平台面/观测面/记账面/一致性套件入口 + `spinlock` 一族)。分类登记口径: 原型 ADR-0002 §2.1、ADR-0003 §2.1、ADR-0005 §2.7 |

- **`br_fault_handler_register` 不在这 48 内**(`3-02` 把它归 v2), 故不计入缺口。
- **`spinlock` 一族是净新增面**(O-17): 计入 `br-sched` 组后 **21 → 27**、总面 **48 → 54** ⇒ **超出 `3-01` CA-5 的"面目标 ≤50, 余量 2"上限 4**。**设计侧必须收编**(登记到分组表 + 裁定面预算), 否则 native 面的"预算"就成了一本糊涂账。

### 3.7 工具链差距

| 阶 | 设计(`brickie-v0.1.md` §14) | 0.x.0 | 差距 |
|---|---|---|---|
| **v0.1** | 骨架生成 + 依赖管理与分析 + 版本管理 + 接口发布 | ✅ **全交付** | —(23 条叶子 + 2 开关) |
| **v0.2** | **接口依赖扫描检查**(符号级; `truth = "header"`, `hash_scope = "sym"`) | ❌ **未做** | 设计 §6.6 的"v0.1 → v0.x 真值迁移"**没有开始**; `api/iface/**` 仍是 `decl` 真值 |
| **v0.3** | **编译**(构建编排 + 描述符/头文件/链接脚本生成物) | ✅ **提前交付** | 演进序重排(O-4); 需回灌 §14 |
| **v0.4** | **test**(conformance 运行器 + **host 平台**) | ◐ | `brickie test` + `tests/gates.toml` 已交付(且能力远超"运行器": 含日志判据/脚本门禁); **host 平台插件仍未落地**(§3.2 第 2 行) ⇒ 该阶的入口条件未满足 |
| **v0.5** | **run**(host-native + QEMU 后端) | ◐ | `brickie run` 只有 **QEMU** 后端; host-native 后端未做(依赖 host 平台插件) |
| **v0.6** | **兼容性检查**(golden / api-dump / abidiff / 版本矩阵)+ 框架件/`svc-posix` 面纳入冻结 | ❌ | 完全未开始; 且被 v0.2 的符号面真值缺席前置 |
| 三语言分层 + JSON 协议 | `brickie-v0.1.md` §9.1(BRV-D3) | ✅ | `protocol_version = 1` 启动握手 |
| 分发形态 | v0.1 只承诺"POSIX 宿主 + `python3` ≥3.11 在位"; wheel/容器/静态链接顺延 v0.x(RV-14) | ◐ | 单文件自包含 ELF + 种子进库 ✅; **其余分发形态仍空** |
| 自举 | ADR-0002/0004: 受治理的宿主编译器 + 种子 | ◐ | 种子 ✅(`prebuilts/seed/…`); **受治理的宿主编译器仍缺**(设计 ADR-0003 §5-2 未解) |
| `abi_id` 指纹 | `2-02` §C5 / BR-D5 | ❌ | 未做(`br-wa-toolchain-001` 的还债动作里带这一条) |
| golden / IFACE-IR | `1-02` §2.6.4 | ◐ | `api/iface/**/*.toml` + `CHANGELOG.md` 有; **CI 层与兼容矩阵未落地** |
| **声明面本身的设计文本** | `brickie-v0.1.md` §8.1/§8.2 | ❌ | 设计侧 `[build]` 只有 `sources`/`includes` 且标"v0.1 只记录, v0.3 起被消费"; 产品 manifest **没有 `[build]`**; **`[build.target]` 与 `tests/gates.toml` 在设计里根本不存在** ⇒ 声明面需要成文(ADR-0006 §4 已列出回灌清单) |

---

## 4. 关闭路径(差距 → 可执行项)

### 4.1 同一根因归组(为什么 M2 是咽喉)

§3 的 ❌ 里, **大半不是彼此独立的缺口**, 而是同一条依赖链的下游:

```
dev-core(设备注册表/命名/子分类协议)
   ├── cdev-core(字符设备 + flash 子型)  ──┬── io/uart-pl011 的 tty 形态
   │                                      └── io/virtio-blk / io/virtio-hsm(M5)
   ├── bdev-core(块设备/几何/分区)        ──┬── fs/littlefs
   │                                      └── fs/erofs(v2)
   └── page cache(vx.0)
vfs-core(挂载表/四层 ops)
   ├── fs/tmpfs(rootfs) + fs/devfs(/dev)
   ├── svc-posix(fd/stdio) ── iface-posix ── 三方移植(sqlite 模式 A)
   └── debug bridge 的 /dev/uart0 ── service/dbg-bridge(M3)
```

**推论**: 没有 `vfs-core`, 就没有 `/dev`; 没有 `/dev`, 就没有 bridge 的通道、没有 `svc-posix` 的 fd、没有 sqlite 的落地 —— 于是 `1-03` §5 DoD 第 6 项(host 平台插件)、`br-wa-debug-002`、`br-wa-test-001` 的第三项(用例按 `6-01` 的 host 列打 PASS 行)都会被同一条链挡住。

### 4.2 建议的下一刀顺序(与 `1-03` §3 对齐)

| 序 | 一刀 | 关掉的差距 | 前置 |
|---|---|---|---|
| 1 | **`core.init` 独立入口 + 平台身份服务化** | `br-wa-boot-001` ①②(O-10/O-11); `1-03` §9 的启动序列归位 | 无(纯 core/platform 内部整理) |
| 2 | **tickless 超时框架**(平台 timer 比较器 + 单次触发) | O-18; `1-03` §3 M1 的最后一条 | 调度框架已在 |
| 3 | **bh / work queue**(`br_work_submit` + 白名单) | O-19; `TC-WORK-001…004`; SLOW 级联域(`TC-IRQ-102`) | 调度框架 + 同步原语已在 |
| 4 | **`vfs-core` + `fs/tmpfs` + `fs/devfs` + `dev-core`/`cdev-core`** | `1-03` §1 的存储与框架件两行; `/dev` 通道 | 上面 3 条(M2 的定义) |
| 5 | **`svc-posix` + `iface-min`/`iface-posix`** | 接口行; 删掉最后一条 `allow_edges`; 三方移植解锁 | 第 4 条 |
| 6 | **debug bridge + host 平台插件** | M3; ASan 白捡; DoD 第 6 项 | 第 5 条(`/dev/uart0`) |
| 7 | **`br-wa-test-001` 对齐**(用例 id ↔ `6-01` + 补 `TC-*` 缺口) | §3.5 的 16 条; `6-01` 成为真判据 | 可与 1–3 并行 |
| — | **设计侧并行**: 3-03/3-04/3-05/3-06/2-01 成文 + `6-01` 补 `TC-PLUG`/`TC-DBG` 组 + `brickie-v0.1.md` §14 回灌 | §3.3/§3.4/§3.7 的设计侧条目 | 三篇新 ADR 已给素材 |

### 4.3 不算"差距"的事(设计内形态, 避免误记账)

| 事项 | 为什么是设计内 |
|---|---|
| 早期 console 用**轮询**、不注册设备 | `1-01` §8 的 console 双形态第一形态; `1-03` M0 亦如此 |
| **SLOW 级联域在 Stage 1 不可用** | `3-02` §1.1.1 两段计: SLOW 域的存在前提就是 bh |
| **`br_fault_handler_register` 未实现** | `3-02` §10.5: 注册 API 归 v2 |
| **`board_irq.h` 是手写静态头** | `3-02` §3.1(IR-2)明示的退路(platform 导出同型宏头) |
| **`BR_IRQ_DISPATCH_BH/_THREAD` 接受但忽略** | `3-02` §3.4 原文如此(Stage 1 无 bh, trace 提示) |
| **描述符池/域池/trace 环是 core 内静态数组** | `3-02` §14.6 允许 manifest 裁剪; 生成链路未就位时取设计默认值 |
| **`Makefile` 未删除, 保留 12 条委派别名** | 设计 ADR-0003 §6: 缩减期两条路径必须并存 |

---

## 5. 诚实账本

### 5.1 WORKAROUND 台账(1 条已注销 + 7 条在册; `检查` = `tools/check-workarounds.sh` 7 ↔ 7 PASS)

| id | 欠的是什么 | 退出条件(还债动作) | 归属里程碑 |
|---|---|---|---|
| ~~`br-wa-entry-001`~~ | ~~Platform 插件化只做一半 + 启动链调用点是 APP 直调~~ | **已注销于 0.x.0**(四条退出条件全满足; 证据: 段内 8 描述符 / `[PLGCONF] 6/0` / `start.S` 无直调) | — |
| `br-wa-boot-001` | ① 无独立 `core.init`; ② APP 直读平台身份(还剩 1 条 `allow_edges`); ③ 日志/trace 未走服务注册表 | 三件都以"`iface-min` 服务化心跳"或"M3 debug bridge"为前提 | M2/M3 |
| `br-wa-isa-001` | ISA 共享库未独立(GICv3 方言 / 向量桩 / 页表构造暂居 platform; 异常帧布局在 core) | 建 `isa/aarch64/`(**非插件**)独立构建单元 + 在声明面表达"platform 使用该 ISA 单元" | M2 |
| `br-wa-toolchain-001` | 工具链用外部 gcc; 顺带欠 `abi_id` 指纹 | 内部工具链就绪后**只改声明面**; 补 `abi_id` | 持续 |
| `br-wa-mem-001` | 三池比例写死在平台 region 表, 未经 `[budget]`/`[[res]]` 生成 | 组合器把预算变成 region 表(或 core 侧由 manifest 裁剪池描述符); 判据 = 改 `product.toml` 预算即改池大小 | M2 |
| `br-wa-debug-001` | `memleak` 是归属标签, 不是 v2 per-plugin arena 记账(无预算/无强制归属/无 OOM 策略) | v2 落地 per-plugin arena; 报告面不变 | v2.0 |
| `br-wa-debug-002` | `dump`/`trace` 直写早期 console, 未经 bridge 成帧 | 呈现层改经 bridge; 判据 = 主机侧能收帧并解码 | M3 |
| `br-wa-test-001` | 三套目标侧套件的 `TC-*` id 自编号, 与 `6-01` **含义漂移** | ① id 逐条对齐(自编号部分移出 `TC-` 命名空间); ② 补齐 `TC-TASK-002/003/004/005`、`TC-SYNC-003`、`TC-TIME-002/003`; ③ 让宿主侧套件按 `6-01` 的 id 打 PASS 行 | 可与 M1 并行 |

### 5.2 未验证 / 不承诺(不许写成"将来会支持")

- **tickless 未做**(周期 tick, O-18); **bh/work queue 未做**; **SLOW 级联域不可用**。
- **抢占与 SMP、`sched-tt` 未做**: 三调度器矩阵 **1/3**; 无 PI、无 per-CPU、无 IPI。
- **per-plugin arena 记账未做**(归属标签近似); **栈 canary 与目标侧 ASan 未做**; **host 平台插件的 ASan 白捡未兑现**。
- **无 M4**: 没有真实 SoC 平台、没有 PMIC/GPIO 级联域驱动的实战。
- **无 MPU / Cortex-R 路径**(vx.0)。
- **原型仓库里没有 CI 配置**: "CI 换入口"(设计 ADR-0003 的 S3)**只在本地门禁层面成立**, 没有可验证的 CI 作业 —— 这是 S3 出口判据在原型侧的**唯一空白**。
- **接口面在原型里没有任何消费者**: 全树没有 `iface-*` 插件, "皮肤再导出"机制没有运行实例(§3.1 接口行)。注: 这是**负面断言** —— 只证明"当前不存在", 不证明"不可能"。

### 5.3 记账瑕疵(原型侧待修, 都不影响功能与判据)

| # | 瑕疵 | 具体位置 | 建议修法 |
|---|---|---|---|
| 1 | **版本串未统一**(§0.3) | `product.toml` `[product].version = 0.1.0.0`; `README.md`(整篇)+ `WORKAROUNDS.md` **标题** = `v0.1.0`; 而 `WORKAROUNDS.md` **正文**与原型 ADR-0005 = `v0.2.0` | 一次性对齐到 `0.x.0`(含 `brickie.lock` 里的插件版本串) |
| 2 | **WORKAROUND 台账排版错位** | `WORKAROUNDS.md`: "在册"那张表的表头下面**没有行**, 而 7 条**在册**行的物理位置落在 `## 已注销(还清了)` 标题之下的表里(该表表头是 `id/注销于/还清它的是哪一刀/证据`, 与 7 行的 5 列内容不匹配) | 把表头与行归位: "在册"表 = 7 行, "已注销"表 = 仅 `br-wa-entry-001`(语义不变, `check-workarounds` 的 7 ↔ 7 判定也不变) |
| 3 | **README 的"刻意不做"表已陈旧** | `README.md` 第 68 行仍写"运行期的插件管理器与描述符段仍未落地"; §5 的 `br-wa-entry-001` 行仍写"只完成了一半" | README 是 v0.1.0 的快照; 下一刀把这两处改成 0.x.0 实况(或整篇按 0.x.0 重写) |
| 4 | ~~原型 ADR 名与 `br_plugin.h` 引用不一致~~ **已闭合** | 原型 ADR-0005 自述"头文件把本 ADR 写成 `0005-plugin-manager-and-scheduler.md`, 需主控改"(该 ADR 第 11/270 行); 但 **HEAD `1d60a15` 的 `br_plugin.h:13` 已是正确文件名 `0005-plugin-manager.md`**(全仓 `grep and-scheduler` 只命中该 ADR 自身) | 无需再改; 若要彻底干净, 删掉 ADR-0005 里那句自述 |
| 5 | **`br-wa-test-001` 是"结构债"而非"功能债"** | 三套目标侧套件的自编号 `TC-*`(§3.5) | 见 §5.1 的还债动作 ①②③ |

---

## 6. 证据索引(可重跑)

> 全部在原型仓库根执行; `B = build/host/x86-64/linux/bin/brickie`。标记 **⟳** = 本篇撰写时(同一提交 `1d60a15`, 干净树)实测。

| # | 命令 | 期望(实测) |
|---|---|---|
| E-1 | `$B check` | 0 错 0 警; 闭包 **8 个插件, RAM 合计 16 KiB, 栈合计 4 KiB** ⟳ |
| E-2 | `$B test --list` / `$B test -j16` | 宿主 4 / QEMU 6 / 脚本 4(+`[build].post` 2)⟳(list); **全量 `test -j16` 全绿是上一轮的测量**(本轮只重跑了 `plugin-test`, 见 E-9) |
| E-3 | `$B gen --check` | 生成物与重算**逐字节一致**(8 个文件)⟳ |
| E-4 | `bash tools/check-build.sh` | 六条不变量 PASS(缺省目标=工具 / 无镜像字面量 / 委派 / 工具段零交叉依赖 / 宿主产物出树三件 / 种子进库三件) |
| E-5 | `$B size` | `text 117472 / data 101 / bss 121632`(`build/brick.elf`)⟳ |
| E-6 | `bash tools/check-workarounds.sh` | `源码标记 7 个 / 登记表 7 条` + PASS ⟳ |
| E-7 | `grep -rhoE '\bbr_[a-z0-9_]+ *\(' core/include/br/core/*.h \| sort -u \| wc -l`(**裸命令 = 141**)`\| grep -v '^br_u32 ('` → **140**; `ls core/include/br/core/*.h \| wc -l` → **17**; `find core platform sched service app -name '*.c' -o -name '*.S' \| wc -l` → **36** | 裸命令的 141 里多出 `br_u32 (`(来自 `br_pic.h:69/88` 的函数指针 typedef `br_u32 (*ack)(...)`, 不是函数); **剔除后 = 140**(§1.11 用的就是这个数) ⟳ |
| E-8 | `grep 'CONF\] SUMMARY' build/logs/plugin.log` | `[IRQCONF] 67/0`、`[MEMCONF] 35/0`、`[DBGCONF] 9/0`(+memleak 12/0)、`[PLGCONF] 6/0`、`[SVCCONF] 3/0`、`[TASKCONF] 12/0`、`[SYNCCONF] 12/0` ⟳ ⚠ `build/**` 不受版本控制(`.gitignore` 的 `/build/`), 且该 `plugin.log` 的 mtime 晚于 HEAD 提交时间(QEMU 门禁被重跑过) —— **内容与干净树一致, 但"某份日志属于某个提交"只能由同一棵树的构建流程保证, 不能由 git 证明**; 要绝对绑定就重跑 `$B test` |
| E-9 | `$B test plugin-test` | `门禁 plugin-test 全绿: require 3 / forbid 3 / 用例 tag 7 / 日志 617 行(PASS 169)` ⟳ |
| E-10 | `tail build/logs/{sched,sync,mem,string}-test.log` | 宿主 `[HOSTTEST] SUMMARY`: 8/0、36/0、81/0、6/0 |
| E-11 | 干净树三后端: `sha256sum build/brick{,-mk,-ninja}.{elf,bin}` | 三个 ELF 同一个 `sha256`、三个 BIN 同一个 `sha256` ⇒ **逐字节一致**(设计 ADR-0003 的 S2 出口)⟳(复核同一棵树的既有产物) |
| E-12 | `$B build --dry-run --json` / `$B build -j16`(干净树) | `steps_total=46`、`steps_todo=0`(热树 46 up_to_date)⟳; **"干净树全量 46 步"是上一轮的测量**, 本轮未重跑(会改写 `build/**`) —— 只读复核能确认的是 `steps_total = 46` 与热树的 `steps_todo = 0` |
| E-13 | `grep -c '' build/logs/plugin.log`; `grep 'manager:' build/logs/plugin.log` | 617 行; `.br_plugins 段 8 条描述符`、`5 条 init 边` ⟳ |
| E-14 | `git ls-files \| grep -iE '(\.github\|\.gitlab\|Jenkinsfile\|\.woodpecker\|\.drone\|azure-pipelines\|\.buildkite)'` | **无输出** ⇒ 原型无任何受版本控制的 CI 配置(§5.2 的"CI 入口切换只在本地门禁层面成立")⟳ |
| E-15 | `objdump -h build/brick.elf` / `objdump -t` | `.br_plugins` 独立成行(大小 `0x300`, `2**3` 对齐)、8 条描述符局部符号 ⟳ |

---

## 附录 A — 设计 ADR ↔ 原型 ADR 对照

| 设计侧决策记录 | 原型侧决策记录 | 主题 |
|---|---|---|
| — | `0001-platform-plugin-manifest.md` | platform 代码收敛为 `brickie` 管理的插件 |
| — | `0002-int-framework-and-gicv3.md` | 中断框架 Stage 1 + GICv3(`3-02` 的落点) |
| — | `0003-memory-map-heap-and-debug-plugins.md` | 4 KiB 恒等映射 + core 堆 + 调试插件(`3-04`/`5-01` 的落点) |
| [`0003-build-ownership-makefile-retirement.md`](../decisions/0003-build-ownership-makefile-retirement.md)(既有) | `0004-brickie-build-and-make-retirement.md` | 构建归属转移: S0–S4 的**执行**与出口证据 |
| **[`0006-build-declaration-surface.md`](../decisions/0006-build-declaration-surface.md)(本篇新增)** | 同上 | 构建**声明面**与判定归属(设计侧裁定) |
| **[`0007-plugin-manager-runtime.md`](../decisions/0007-plugin-manager-runtime.md)(本篇新增)** | `0005-plugin-manager.md` | 插件管理器运行期(段/相位机/拓扑/注册表) |
| **[`0008-sched-framework-and-sync-in-core.md`](../decisions/0008-sched-framework-and-sync-in-core.md)(本篇新增)** | `0006-scheduler-framework.md` + `0007-sync-primitives.md` | 调度框架 + 同步归 core + tick 粒度 |

> **分工**: 设计侧 ADR 记"设计因此定成什么样 + 哪些文档要回灌"; 原型侧 ADR 记"这一刀改了什么 + 出口判据跑成什么样"。**同一件事两侧都留痕, 但真值只有一处** —— 契约归设计仓库, 实现证据归原型仓库。

## 附录 B — 0.x.0 的欠债净变化

| 变化 | 条目 |
|---|---|
| **注销** | `br-wa-entry-001`(平台插件化 + 启动链调用点) |
| **新登记** | `br-wa-test-001`(用例 id 对齐) |
| **缩小作用域** | `br-wa-boot-001`(从"整条启动链靠替身"缩小到"三件" —— `core.init` 缺席 / 1 条 `allow_edges` / 日志未服务化) |
| **不变** | `br-wa-isa-001`、`br-wa-toolchain-001`、`br-wa-mem-001`、`br-wa-debug-001`、`br-wa-debug-002` |
| **净计数** | 1 注销 + 1 新登记 ⇒ **仍是 7 条在册**(但"欠的是什么"变了: 从运行期缺席变成生成链路与对齐债) |
