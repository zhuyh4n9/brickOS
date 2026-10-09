# 0008 — core 启动入口(`br_core_main`): 四阶段启动链

> 状态: **已落地**(v0.2.0, QEMU virt aarch64; `smoke`/`plugin-test`/`irq-test`/`dbg-test`/
> `sched-test`/`sync-test` 六道门禁全绿, `check-workarounds`/`check-headers`/`check-build` 一致)。
> 影响面: `core/src/main.c`(**新**)、`core/include/br/core/br_main.h`(**新**)、
> `core/include/br/core/br_plugin.h`(**只加不破**: 管理器拆成三个入口)、
> `core/src/plugin/plugin_mgr.c`(拆 `br_plugin_manager_init` / `_platform_init` / `run`);
> `platform/qemu-aarch64/src/start.S`(只 `bl br_core_main`)、
> `platform/qemu-aarch64/src/plat_qemu_virt.c`(early_init 只留平台侧两件)、
> `platform/qemu-aarch64/src/memmap.c`(只声明 region)、
> `platform/qemu-aarch64/src/mmu.c`(`br_plat_mmu_init` → `br_plat_mmu_ops_register`; 硬件摘要留痕搬进 `activate`);
> `core/include/br/core/br_irq.h` / `br_mem.h` / `br_mm.h` 与 platform 的 `br_plat.h` / `br_mmu.h`(注释同步)
> 设计依据: `1-01` §9(启动序列)/§6.2(生命周期四相)/§8(平台能力三层模式)、`3-04`(region 表 → 三池 → 恒等映射)、
> `3-02` §14.3(中断初始化单向链; P-IRQ-5 的描述符池初始化)、`3-05` §2(plugin_manager)
> 相关: ADR-0005(插件管理器; 本 ADR 还清它 §5 第 4 项"core.init 仍是权宜"这笔债)、
> ADR-0003(内存映射与 region 表)、ADR-0006(调度框架; 调度器的**注册**仍在 `coop_early_init`)
> 登记表: `WORKAROUNDS.md` 的 `br-wa-boot-001` ① —— **本刀还清**。

## 1. 背景

ADR-0005 把启动链的编排权收进插件管理器(`br_plugin_manager_run()`), 于是 v0.2.0 的形态是:

```
start.S(reset/BSS) → br_irq_cpu_init → br_plugin_manager_run()
                       └─ 头部代做: br_clock_init + br_log_init
                          → 扫段 → 找 platform → 跑它的 early_init → 拓扑 → 相位驱动
```

这留下三处**归属混乱**(ADR-0005 §2.6/§5 已如实登记):

1. **设计 `1-01` §9 的 `core.init` 没有落点** —— 时钟/日志塞在管理器头部, 中断框架的
   per-CPU 状态塞在 `start.S`, 堆塞在 platform 的 `memmap.c`, 而"注册表/调度框架对象"
   干脆是纯静态 BSS(没有显式一格)。
2. **堆的认领不在 core** —— `br_mem_init()` 藏在 platform 的 `br_plat_memmap_init()` 里。
   但设计 `3-04`/`1-01 §9` 把"堆"归 `core.init`: platform 应只声明"哪块 RAM 是什么"
   (region 表), **认领与分配**是 core 的事。
3. **地址映射的"机制"与"建立动作"混在一处** —— `br_plat_mmu_init()` 既
   `br_mm_register()`(平台机制)又 `br_mm_activate()`(建立 + 开 MMU)。三层的边界应是:
   platform 提供页表构造, core 提供抽象**并执行建立**。

## 2. 决策

### 2.1 `br_core_main()` = 四阶段启动链(唯一编排点)

`platform/…/src/start.S` 只做 reset(选核 / 屏蔽异常 / EL2→EL1 / 栈 / 向量表 / BSS 清零),
然后 `bl br_core_main()`; **汇编不再调任何子系统**。`br_core_main()`(`core/src/main.c`)按:

| 阶段 | 内容 | 对应设计 |
|---|---|---|
| ① core 平台无关 | `br_clock_init()` → `br_log_init()` → `br_log_set_level()` → `br_irq_cpu_init()` → `br_plugin_manager_init()` | `core.init` 的"中断框架/注册表" + 管理器"扫段" |
| ② platform 插件 | `br_plugin_manager_platform_init()` → platform 的 `early_init`(console / PIC + 绑定表 / region 表 / 页表 ops) | `platform.early_init` |
| ③ core 依赖平台 | `br_mem_init()`(region 表 → 三池) → `br_mm_activate()`(region 表 → 恒等映射 + 开 MMU) | `core.init` 的"堆" + "恒等映射的建立" |
| ④ 插件相位驱动 | `br_plugin_manager_run()`: 建边 → Kahn → EARLY(其余)/CORE/LATE → 开中断 → START(APP 最后) → `br_sched_run()` | `plugin_manager` + 四相 |

四阶段的**顺序都是硬的**, 逐条:

* ① 必须最先, 因为 `br_irq_cpu_init()` 把描述符池的 `pic_id/dom_id` 全置 `-1`(P-IRQ-5),
  而 platform 的 `br_irq_bindings_set()` 会填这两个字段 ⇒ **顺序颠倒会把绑定抹掉**
  (这不是新增约束: v0.1 用"`start.S` 里先调"表达同一条)。
* ① 全程**静默**: 早期 console 是 ② 的 platform `early_init` 才配好的, 在那之前写 PL011 的
  DR 会被 QEMU 丢掉(`CR.UARTEN = 0`)—— 与 ADR-0005 §2.6 的可观测性理由同源。
* ③ 的 `br_mm_activate()` 必须是**插件 init 之前的最后一格**: 一开 MMU, 每一次取指/访存都过
  翻译, 页表必须已覆盖当前 PC/SP。
* ④ 沿用 ADR-0005 的全部相位规则(EARLY 第一步是 platform、APP 的 start 最后、首败即停机、
  环检测报完整路径), **一个字都没改**; 变的是"platform 的 early_init 由 ② 显式跑", 于是
  ④ 的 EARLY 循环跳过它。

### 2.2 插件管理器拆成三个入口(与启动链三格一一对应)

`br_plugin_manager_run()` 之前"顺带"做的四件事各归其位:

| 原来在 `run()` 里 | 现在 |
|---|---|
| `br_clock_init` / `br_log_init` / 设日志等级 | `core/src/main.c` 阶段 ① |
| `scan()`(扫段 + 建表) + 找 platform | `br_plugin_manager_init()`(阶段 ①, **静默**) |
| 跑 platform 的 `early_init` | `br_plugin_manager_platform_init()`(阶段 ②) |
| 重名自证 / 建边 / 拓扑排序 / 相位驱动 | `br_plugin_manager_run()`(阶段 ④) |

* 三个入口都带**前置断言**(未调 `init()` 就调后两者 ⇒ `br_panic`), 于是"顺序"是运行期可
  自证的, 不靠注释。
* `br_plugin_manager_platform_init()` 是 TC-PLUG-003 判据
  ("第一个真的跑起来的 `early_init` 属于 platform")的**新取值点**; 判据本身不变
  (`s_first_early_name == platform->name`), 因为 platform 的 `early_init` 仍早于 ④ 里
  **任何**插件的 `early_init`。
* 重名自证/建边/拓扑排序仍排在 platform `early_init` **之后**(阶段 ④), 免得它们的
  `br_panic` 输出被 QEMU 丢掉 —— 与 ADR-0005 §2.6 的重排理由一致。

### 2.3 memory 与地址映射: platform 出数据与机制, core 出抽象与建立动作

* `br_plat_memmap_init()` **只声明** region 表(镜像/栈/五块池/MMIO)。
* `br_plat_mmu_init()` 更名 **`br_plat_mmu_ops_register()`**, **只** `br_mm_register(&s_mm_ops)`。
* `core/src/main.c` 阶段 ③ 调 `br_mem_init()` 与 `br_mm_activate()`; 失败 ⇒ `br_panic`
  (此时 console 已起, 报得出)。
* MMU 的**硬件摘要留痕**(SCTLR/pages/TTBR0/TCR/MAIR)搬进 platform 的 `mmu_activate()`
  —— 那是"平台对自己硬件事实"的留痕; core 的 `br_mem_init()` 另有"堆/池"摘要。两边各说
  自己的事实, 不互相代打(旧实现里 platform 读 `br_mem_layout` 打 core 的池大小, 是一处
  错位的耦合)。

### 2.4 调度框架对象: 不造 API, 如实说明

设计 §9 的 `core.init` 里写"调度框架对象(无线程)"。本原型里 `br_sched` 的状态
(TCB 池/`s_current`/`s_ops`/`s_all`)全是 BSS 静态量, **没有运行期构造**; 唯一的一次
"注册"(`br_sched_register()`)按设计归 **EARLY 相**(`coop_early_init()`, 3-01 §5.1 的
"EARLY 恰一次"), 不属阶段 ③。因此**不新增** `br_sched_init()` 之类的空 API —— 那会在
启动链上造一个"看起来有初始化、其实什么都不做"的假格。阶段 ③ 的职责是让"堆 + 地址映射"
就绪, 这正是调度器与插件 init 的前提。

## 3. 设计缺口与逐条裁定

1. **`br_core_main` 曾在 ADR-0005 被拆掉, 现在重建 —— 是不是又走回头路?**
   不是。被拆掉的是 M0 那个"在 APP 里直调各子系统 + 一致性用例 + 主循环"的**替身**;
   重建的是"四阶段**编排**", 它不碰任何子系统的实现, 也不做自检(那仍在 platform 的
   `start` 与 dump 的 LATE `init`)。同一个名字, 两种职责 —— ADR-0005 §2.5 拆它的理由是
   "编排权只有一处, 且应在 core", 本 ADR 正是把那处落成独立入口。
2. **`br_irq_cpu_init()` 放阶段 ① 而不是用户口径的"阶段 ③(int)"**: 见 §2.1 的硬约束 ——
   它清空描述符池, 必须早于 platform 的绑定表提交。它的**平台无关**属性也支持这个位置
   (它只碰 core 的静态池与 TPIDR_EL1)。阶段 ③ 的"int"因此没有新增调用点: PIC 的注册
   (platform 机制)在 ②, 描述符池/CPU-local 在 ①。
3. **日志归属**: `br_log_init` 的时间基点必须在阶段 ①(否则 ② 起的日志没有时基); 但
   ① 不能**输出**。于是"置等级"放 ①(纯状态), 实际输出从 ② 之后才有。
4. **入口名与命名空间**: `br_core_main`(`br_` 前缀 = core 独占命名空间, 设计 §7.2)。
   汇编里 `bl br_core_main` 与旧形态同形, 但与 `br_plugin_manager_run` 的语义边界清楚:
   "core 的入口" vs "插件管理器的入口"。
5. **`core/src/main.c` 会不会被 `[build].core_sources` 自动纳入?** 会:
   `product.toml` 写的是 `core/src/*.c`, 新增顶层 `.c` 无需改声明面(实测 `core` 单元由
   18 → 19 个源)。**不改** `product.toml` 的构建段。
6. **`br_plat_mmu_init` 改名的代价**: 它不在任何 `[[export]]` 里(是 platform 的内部启动
   入口), 所以改声明面/接口 hash 都不受影响(核对过 `plugin.toml`/`plat.toml`)。

## 4. 被否决的替代方案

| 方案 | 为什么不取 |
|---|---|
| 继续让处理器管理器"代做 core.init"(ADR-0005 §2.6 的权宜) | 设计 `1-01` §9 的 `core.init` 永远没有落点; 堆/地址映射的归属长期错位(platform 做 core 的事) |
| 把 `br_core_main` 声明塞进 `br_plugin.h` | 插件**元契约**头会掺入"core 启动入口", 两件事的读者与冻结面不同 |
| 用普通 `main()`(而非 `br_core_main`) | 裸机没有 C runtime 的 `main` 约定; 且 `br_*` 是 core 的命名空间纪律(设计 §7.2) |
| 保持 `br_mem_init()` 在 platform 内, 阶段 ③ 只做地址映射 | "按 region 表认领池"是 core 的职责(3-04/1-01 §9); 留在 platform 等于把 core.init 的"堆"继续挂在 platform 上 |
| 把 `br_mm_activate()` 也留在 platform | 激活是**状态机**的一步(未注册→已注册→已激活), 状态归 core; platform 只该填 `ops.activate` 的机制 |
| 把 `br_irq_cpu_init()` 挪到阶段 ③(改语义为"只初始化未绑定项") | 需要改 P-IRQ-5 的冻结语义与"绑定前必须清池"的判据, 收益只是"字面顺序好看", 风险不对称 |
| 新增 `br_sched_init()` 让阶段 ③"看起来"初始化了调度框架 | 造一个什么都不做的 API, 违反"面小/不造假格"的纪律; 注册点本就属 EARLY 相 |
| 让 `br_core_main` 也调一致性用例(回到 M0 形态) | 自检的落点已按相位归位(platform 的 `start` / dump 的 LATE `init`), 拉回 core 入口会让"启动"与"自检"重新纠缠 |

## 5. 后果与遗留项(诚实清单)

**本刀还清的债**
* `br-wa-boot-001` ①: **独立的 `core.init` 入口**(堆/中断框架/地址映射的建立各自归位)。
* ADR-0005 §5 第 4 项("管理器头部的 core.init 代做是权宜, 真正的落点应是独立函数")。
* ADR-0005 §5 第 7 项里的一部分:`start.S` 的调用点从 `br_irq_cpu_init` +
  `br_plugin_manager_run` 收敛为单一 `bl br_core_main`。

**仍在欠的(不要误报为已还)**
1. `br-wa-boot-001` ②(APP 直读平台身份 ⇒ 一条 `allow_edges` 豁免)与 ③(日志/trace 直写
   console/RAM 环)仍欠 —— 前提分别是 M2 的 iface-min / M3 的 debug bridge。
2. `br-wa-isa-001`: `mmu.c` 的页表构造仍暂居 platform 插件目录; 本刀只把"注册/激活"的
   边界划清, 没有把文件迁去 ISA 共享库。
3. `br-wa-mem-001`: 池比例仍写死在 `memmap.c`, 没有 manifest 预算 → region 的生成链路。
4. `core/include/br/core/br_plugin.h` 文件头引用的 ADR 文件名
   (`0005-plugin-manager-and-scheduler.md`)与实际不一致 —— 旧账, 冻结件由主控改。

## 6. 需要主控执行/注意的动作

* `WORKAROUNDS.md`: `br-wa-boot-001` 行已在本刀改写(① 标记还清, ②③ 保留); 源码里的
  `WORKAROUND(br-wa-boot-001)` 标记**保留**(②③ 仍欠), `check-workarounds` 保持绿。
* 本文档登记在 `docs/decisions/0008-core-main-boot-chain.md`; 若设计侧对 `1-01` §9 的
  `core.init` 有新口径, 以设计文档为准并回来改本 ADR(本刀只按现状落地)。
