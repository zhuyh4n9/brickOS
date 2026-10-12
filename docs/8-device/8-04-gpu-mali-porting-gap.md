# 8-04 — Mali GPU 与开源图形栈移植: 差距分析

> 章节: **8-device**。状态: **分析稿**(全景 + 差距清单 + 边界论证 + 落地顺序; 需拍板的项在 §6, 本篇不代替决策)。
> 定位: 回答两个问题 —— ① **基于 Mali 的开源 GPU 栈有哪些、各是什么形状**; ② **把它们搬进 brickOS 还差什么**。前者是外部事实(§2), 后者分**机制侧**与**设计契约侧**两半(§4)—— 后者才是本篇标题里"gap 到 Design"的意思。
> 方法: 与 `8-02`/`8-03` 同型(先定移植边界, 再列差距)。但本篇有一处**前提差异**必须开篇说清(§0): **brickOS 没有用户空间**, 而 Mali 的开源栈在 Linux 上是"内核 + 用户态"两半, 中间隔着一条系统调用边界 —— 这条边界在 brickOS 里不存在, 于是"移植哪个栈"这个问题必须先被改写。
> 基线: 设计侧 HEAD `6aae49a`(工作树有在途改动: `8-03`(USB)+ `8-01` 的 O-S9 由并行工作写入); 原型 HEAD **`e7cf0b5`**(`product.toml` 版本 `0.2.0.0`, 工作树有在途改动)。
> 依赖: `8-01`(设备体系/D20 子分类/形态 A·B·C/O-S*)、**`8-02`(netdev 移植差距: 本篇的方法样板)**、`1-01` §2(单 APP/单异常级)/§4.3(内存政策)/§14.1(域支撑矩阵)、`3-04`(内存)、`1-03` §1(版本清单/未排期)、`11-02`(POSIX 子集)、`2-toolchain/brickie/brickie-v0.1.md` §7.3(依赖方向禁则)。
> 外部事实基线: Linux **7.3-rc** / Mesa **25.2**(2025-08)。凡外部结论均给一手链接, 本机可重跑的取证另列 §8; 未能核实项集中在 §10。
> **范围**: 目标硬件代际 = **Midgard(T-6xx/T-7xx/T-8xx)+ Bifrost(G31/G51/G52)**, 即 **JM(Job Manager)** 硬件 —— **不需要任何 GPU 固件**(§2.1), 因此 CPU 侧完全避开 CSF 固件 EULA 这条法务线。

## 缩略词(abbreviations)

| 缩略词 | 英文全称 | 说明 |
|---|---|---|
| **APP** | Application | 插件类别(唯一业务逻辑, 恰一个) |
| **BO** | Buffer Object | 可被 GPU 访问的内存对象(句柄 + GPU VA + CPU 可见性) |
| **CSF** | Command Stream Frontend | Valhall gen2+(G310/G510/G610/G710…)的 GPU 内置 MCU, **必须加载固件** |
| **DRM** | Direct Rendering Manager | Linux 的图形子系统(`/dev/dri/*`, fd + ioctl + GEM/PRIME) |
| **GEM** | Graphics Execution Manager | DRM 的内存对象层(`drm_gem_shmem` 等) |
| **GLES / GL** | OpenGL ES / OpenGL | 图形 API |
| **ISA** | Instruction Set Architecture | 此处指 GPU 指令集(Midgard/Bifrost ISA) |
| **JD / jc** | Job Descriptor / job chain | 内核交给硬件执行的作业描述符链(`drm_panfrost_submit.jc`) |
| **JM** | Job Manager | Midgard/Bifrost/Valhall-JM 的硬件作业调度器(**无固件**) |
| **kbase** | Mali Kernel Driver | ARM 自家 DDK 内核驱动(`/dev/mali0` + 自有 UABI, 非 DRM) |
| **LPAE** | Large Physical Address Extension | Mali MMU 的页表格式 |
| **MMU(GPU)** | — | **Mali 自有的** MMU(把 GPU VA 翻译为 PA), 与 CPU 的 MMU 无关 |
| **NIR** | New Intermediate Representation | Mesa 的着色器中间表示 |
| **O-S\*** | — | `8-01`/`8-02`/`8-03` 的开放问题编号(本篇新增 O-S10) |
| **PanVK** | — | Mesa 的 Mali Vulkan 驱动(架构目录 `bifrost/valhall/csf/jm/fifthgen`) |
| **PRIME / dma-buf** | — | Linux 的跨设备/跨进程缓冲共享与 fence 导出机制 |
| **syncobj** | — | DRM 的同步对象(可等待/可信号/可过 fd); panfrost 的 `in_syncs`/`out_sync` 就是它 |
| **WTFPL** | Do What The Fuck You Want To Public License | 本仓库使用的许可证 |

## 0. 前提: brickOS 没有用户空间 —— 所以"移植一个栈"这个说法要改写

一个事实先摆出来: **Linux 上"Mali 开源栈"从来不是一个整块, 而是被一条系统调用边界切成两半。**
下面这张表是 `8-03`(USB/libusb)"边界在哪"论证的直接延续, 但结论更强: USB 那条边界在 brickOS 里**存在但形状不同**(cdev/bdev 出口); GPU 这条边界**在 brickOS 里根本不存在**。

| Linux 上的形状 | brickOS 里的对应物 | 判定 |
|---|---|---|
| 内核驱动(`lima`/`panfrost`/`panthor`/`kbase`) | **插件**(`io/mali-*` 器件驱动 + `framework/*-core` 契约) | ✅ 有对应物, 形状要重定 |
| 用户态库(Mesa / 专有 `libmali`) | **同一地址空间里的库** —— 不是"没有", 而是**没有边界** | ⚠️ 要按"层"拆(§5.1) |
| DRM UAPI / kbase UABI(fd + ioctl + mmap) | **无对应物, 且不该发明** | ❌ 它的存在理由是"进程隔离 + fd 生命周期 + 跨进程 buffer 共享"; 单 APP/单地址空间(`1-01` §2.1/§2.2)下这三条理由全部消失 |
| `/dev/dri/cardN`、`dma-buf` fd、`PRIME` | **无对应物** | ❌ brickOS 的等价物是 native API + 直接指针(恒等映射, `br_mem.h` 的 `dma_addr == vaddr`) |

三条推论(本篇的立论基础):

1. **可搬的是"层", 不是"栈"。** Mesa 不是整块 —— 它的**着色器编译器**(`src/compiler/nir` + `src/panfrost/compiler`)是不碰操作系统概念的**纯计算库**; 而它的 **winsys / EGL / GL 分发 / WSI** 整个是为"进程 + fd + 窗口系统 + 驱动边界"设计的。这两个半边的可移植性差一个数量级(§5.1 逐层表)。
2. **内核侧反而变简单了 —— 但要自己判断"哪些代码是给 DRM 的"。** Linux 的 `panfrost` 里有大量代码只为 DRM 服务(shrinker / coredump / fdinfo / PRIME / shmemfs 后备)。brickOS 没有这些概念 ⇒ 真正要搬的是**与 GPU 硬件相关的那部分**: Mali MMU 页表 / JM 提交 / 三条中断 / 上电时序 / cache 维护。量级远小于"搬一个 DRM 驱动"。
3. **最大的一块工作在"在 Linux 上没人让你做的部分"。** 证据直接在 UAPI 里: `struct drm_panfrost_submit.jc` 的注释逐字为 *"Address to GPU mapping of job descriptor"* —— 即 **JD 是 Mesa 在用户态构造好后, 只把一个 GPU 地址交给内核的**; 内核读它头部的少量字段(requirements 由 ioctl 单独传)后交给 JM。**brickOS 若不带 Mesa, 就没有 JD 的生产者**: 描述符与 Midgard/Bifrost 指令流都得自己生成。这是本移植真正的成本中心(§5.2)。

## 1. 结论

四句话:

1. **目标代际选对了 —— Midgard/Bifrost 是"零固件"的一档。** 这一档只有 `panfrost`(JM)一条开源内核路线, 且 Mesa 侧覆盖 GLES 3.1(Bifrost v7 另有 Vulkan 1.3)。**避开 CSF 就意味着不必碰 `mali_csffw.bin` 的 ARM EULA**(§2.1), 这是一个应在设计里写明的**红利**, 而不是顺手一提。
2. **`kbase` 与 CSF/Panthor 都不该进候选。** `kbase` 的内核源码虽是 GPL-2.0, 但它的消费面只有一个不可再分发的专有 `libmali`(§2.2) —— 在"无用户空间 + 要自己写消费面"的前提下, 它是**负资产**; Panthor(CSF)则要固件 + `DRM_GPUVM`/`DRM_EXEC` 级模型, 且目标硬件用不上。
3. **移植边界不能选在 DRM UAPI, 也不能选在"Mesa 整体" —— 正确的第一次切分是"编译器 vs 运行时"。** 编译器可搬(纯计算), 运行时不可搬(为进程/fd/驱动边界而生)。真正的缺口因此分两类: **机制侧**(FP/SIMD、时钟/电源/复位、BO/fence 契约、JD 生产者)与**设计契约侧**(D20 装不下 GPU、共享缓冲与同步对象缺契约、Interface 可达性、`api_type` 归类) —— 后一类必须在写代码前落盘, 否则 GPU 会各自发明一套 BO/fence。
4. **在拍板之前先问一个问题: 要 2D 还是要 3D/计算。** 因为 **Mali 没有开源的 2D-only 驱动, 且 Mali GPU 本身不含显示控制器** —— 若真实诉求是 2D 合成/扫描输出, 正确做法是 **CPU 渲染 + 显示控制器驱动(即 `io/display`), 完全不进 GPU**。这一步能把工作量降一个数量级; 它属于排期决策, 不属于技术决策(§5.3)。

## 2. 开源 Mali 栈全景(外部事实)

### 2.1 内核侧: 四条路线

| 栈 | 上游 / 许可 | 支持代际 | Linux | 需要固件? | 对 brickOS 的判定 |
|---|---|---|---|---|---|
| **`panfrost`** | [`drivers/gpu/drm/panfrost`](https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/panfrost/Kconfig), **GPL-2.0**; UAPI 头 `panfrost_drm.h` 是 **MIT** | Midgard T6xx/T7xx/T8xx、Bifrost G3x/G5x/G7x、Valhall-JM **G57/G68/G78** | 5.2 | **不需要**(无 fw 源文件、DT 无 `firmware-name`) | ★ **本篇主选**。内核侧要搬的是"与硬件相关的那半" |
| **`lima`** | [`drivers/gpu/drm/lima`](https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/lima/Kconfig), GPL-2.0 OR MIT | Utgard Mali-400/450(**470 不支持**) | 5.2 | 不需要 | 目标之外(用户已选 Midgard/Bifrost); 但其"最小可行切片"是先例 |
| **`panthor`**(旧称 PanCSF) | [`drivers/gpu/drm/panthor`](https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/panthor/Kconfig), GPL-2.0 or MIT | Valhall-**CSF**: G310/G510/G610/G710/G715/G720/G725/G1-* | **6.10** | **必须** `mali_csffw.bin` | ❌ 目标硬件用不上; 固件是 ARM **EULA**(非开源许可, 含"不得用于规避 ARM IP"条款, 禁逆向) |
| **`kbase`**(ARM DDK) | developer.arm.com 投放; 驱动源码**逐文件 SPDX GPL-2.0** | Midgard/Bifrost/Valhall 全系(按 release) | 永不上游 | 不适用 | ❌ **接口是 `/dev/mali0` + 自有 UABI(非 DRM), 而唯一能对接它的用户态是不可再分发的专有 `libmali`** ⇒ 在 brickOS 里要么自造消费面(等于重写 kbase UABI 的两套 dispatch), 要么被锁死在 blob 上 |

> **两条易错的事实**(调研中出现的口径纠正, 写进这里免得后人再错): ① **分界线是 CSF/JM, 不是"Valhall gen1/gen2"** —— `panthor` 的 Kconfig 自己写明 G68/G78 虽是 Valhall 但**不是 CSF**, 归 `panfrost`; ② **`lima` 与 `panfrost` 同在 Linux 5.2 合入**(不是 5.2/更晚的差别)。

### 2.2 用户态侧: 三条路线

| 路线 | 覆盖 | 关键事实 | 对 brickOS 的判定 |
|---|---|---|---|
| **Mesa gallium `panfrost`** | GLES 2.0(Midgard v4: T600/T620/T720)、GLES/GL 3.1(v5 T760–T880、Bifrost v6/v7) | 通过 libdrm 走 DRM ioctl; winsys/EGL/GL 分发层与进程模型强绑 | ⚠️ **只取编译器部分**(§5.1) |
| **Mesa `panvk`** | Bifrost v6/v7 → **Vulkan 1.3**; Valhall v9(G57/G68) **无 Vulkan**; CSF v10+ → 1.4(**需 panthor + 固件**) | 以 Mesa 25.2 为"可用"分界; 架构后端目录 `bifrost/valhall/csf/jm/fifthgen` | ⚠️ 同上, 但 Vulkan runtime 层要的 OS 面**更多**(timeline semaphore/syncobj/WSI) |
| **专有 `libmali`** | EGL/GLES/CL(+部分 Vulkan) | 只讲 kbase UABI; 无源码、无再分发许可、无 ABI 稳定承诺; 只随 SoC vendor BSP 交付 | ❌ 不可用, 也不该用 |
| (软件回退) **Mesa `softpipe`** | GLES 级别, **纯解释执行** | 无 JIT、无 LLVM 依赖、**架构无关** | ✅ 若只需要"有个 GL 面", 这是唯一零 GPU 的路; 但性能与 GPU 无关, 不该拿来顶替 GPU 需求 |
| (软件回退) **Mesa `llvmpipe`** | 同上, LLVM JIT | 官方文档只列 **x86/x86-64/ppc64le** JIT 目标(aarch64 未列, 未核实) | ⚠️ 待核实; 且引入 LLVM = 与裸机工具链假设冲突 |

### 2.3 决定成本的四条 UAPI 事实(逐条来自 `panfrost_drm.h`)

| # | 事实 | 来源 | 对 brickOS 的含义 |
|---|---|---|---|
| 1 | **JD 由用户态构造** | `drm_panfrost_submit.jc` = *"Address to GPU mapping of job descriptor"* | 没有 Mesa 就没有 JD 生产者 ⇒ **必须自造**(§5.2 方案 A/C 的分水岭) |
| 2 | **tiler heap 是用户态 BO** | `PANFROST_BO_HEAP` 标志, 与 `PANFROST_BO_WB_MMAP` 互斥 | tiler heap 的尺寸/增长策略属消费面, 不是驱动内部事 |
| 3 | **同步是 syncobj** | `in_syncs`/`out_sync`(sync 对象句柄) | brickOS 需要一个"可等待/可信号/可被中断完成"的**同步对象契约**(现有 sem/cond 不够, §4.1 #5) |
| 4 | **cache 维护是一等 API** | `PANFROST_BO_SYNC_CPU_CACHE_FLUSH` / `..._FLUSH_AND_INVALIDATE`; `DRM_PANFROST_PARAM_SELECTED_COHERENCY` + 三态 `ACE_LITE/ACE/NONE`; imported BO **恒按非一致处理** | ★ **直接对上 brickOS 已有的 `br_mm_cache_flush/invalidate`**(§3)——这是"已有能力可复用"中最实在的一条 |

> 另有两条对设计有影响的细节: `create_bo.offset`(BO 的 GPU VA)**恒非零**("various HW units treat 0 specially"); `jm_ctx` = **地址空间 + 优先级**(LOW/MEDIUM/HIGH), 即"上下文"在 Mali 上就是"一个页表 + 一个优先级"。

## 3. 已有能力(可直接用, 不需新建)

| 能力 | 位置(原型) | 对 Mali 的意义 |
|---|---|---|
| **设备注册表 + 子分类协议** | `framework/dev-core`(`br_dev_add`/`br_dev_lookup`) | GPU 入册; `class_id` **append-only** 加一个值即可, dev-core 一行不改(`8-01` §2/§3) |
| **子分类框架的完整模板** | `framework/cdev-core/src/cdev_core.c` + `cdev_selftest.c` | 若判"第四子分类 `gpu-core`", 照它的骨架写(槽位池/注册校验/取回/自检) —— 与 `8-02` §2 对 netdev-core 的处置相同 |
| **ISR→线程交接** | `br_sem_give`(ISR-safe 白名单, `br_sync.h`)+ `br_work_submit`(bh, `br_work.h`) | JM 的 job/mmu 中断只需"给信号量 + 记状态"; 重的收尾在线程。**契约只依赖 `br_sem_give`, 不依赖 bh**(与 `8-02` §3#2 的结论同型 —— bh 形态再变也不动契约) |
| **DMA + 恒等映射 + cache 维护** | `br_dma_alloc`(`dma_addr == vaddr`)、`br_mm_cache_flush` / `br_mm_cache_invalidate`(`br_mem.h`/`br_mm.h`) | ★ **BO 不需要地址翻译, 且 `PANFROST_BO_SYNC` 的两档语义(flush / flush+invalidate)与既有 API 一一对应** —— 这是整篇里最省的一处 |
| **页池 / 连续池** | `br_page_alloc`(4 KiB 静态位图)、`br_mem_alloc_contig` | Mali LPAE 页表的载体(页池)与 BO 后备(连续池) |
| **IRQ 框架 + 余量** | `BR_IRQ_MAX = 64`、`BR_IRQ_BOARD_NR = 11u`(现用); FAST/SLOW 级联域 | 余量够绑 job/mmu/gpu 三条线(MMU fault 可走 SLOW 域语义: ISR 只清源, 重的走 bh/线程) |
| **region 表余量** | `BR_MM_MAX_REGIONS = 24`, 现声明 **11** 条 | 够加 Mali MMIO 窗口 + 帧缓冲预留区 |
| **自检机制(生成期开关)** | 原型 ADR-0010(`selftest` 钩子, 关掉即被 `--gc-sections` 裁掉) | ★ **没有 GL 面也能把驱动验收到可交付**(§7 阶段 2 的判据设计) |
| **三方隔离的正规通道** | `third_party/`(逐字节上游只读)+ `adapter/<插件>/{compat,glue}` + `tools/check-third-party-pristine.sh`(ADR-0024/0026) | 搬 Mesa 编译器代码走这条路: 上游原件 + 我们只写 `compat/`(它要的环境)与 `glue/`(我们的对接) |
| **POSIX 运行时** | `runtime/posix`(pthread/同步/fd/stdio 子集) | 若编译器用到 pthread/stdio, 有现成面; 但**没有 `math.h`/libm**(§4.1 #6) |

## 4. 差距

分两半列: **A = 机制/代码侧**(不解决无法开工), **B = 设计契约侧**(不解决会各自发明一套, 这才是"gap 到 Design")。

### 4.1 A. 机制侧

| # | 差距 | 证据 | 阻塞 |
|---|---|---|---|
| 1 | **FP/SIMD 从未使能** —— 这不止是 GPU 的问题, 是全系统级缺口 | `CPACR_EL1.FPEN` 无任何使能代码(全树 grep `cpacr` 只命中 fault 解码器的**注释**与字符串, `core/src/irq/irq_fault.c:52`); 编译 flag 是 **`-mgeneral-regs-only`**(`tools/brickie/docs/contract.md`); `core/src/sched/switch.S` 的 `ctx[13]` 只存 callee-saved, **无 `q0`–`q31`**; `br_fault.h:23` 把 FP/SIMD 陷阱定义为 **fatal** | 🔴 |
| 2 | **无时钟/电源/复位框架** | 全树按 API 形状 grep `clk_get`/`clk_prepare`/`devfreq`/`regulator`/`power_domain`/`reset_control` **零命中**(E-6); core 只有 arch timer(`br_time.h`)。而 `panfrost` 需要 **core/stacks/coregroup 三路时钟** + power domain + reset + (可选)devfreq/OPP/regulator | 🔴 |
| 3 | **JD 与 GPU 指令流没有生产者** | UAPI: `jc` = "Address to GPU mapping of job descriptor"(§2.3 #1)。brickOS 无 Mesa ⇒ 自造描述符 + Midgard/Bifrost ISA | 🔴 |
| 4 | **无"共享内存对象(BO)"契约** | `br_dma_alloc` 返回裸 `{vaddr, dma_addr, size}` **无句柄/无引用计数/无生命周期**; region 表是**静态**声明(`br_mm.h`); `br_mem.h` 已注明"帧缓冲类大固定块走 region 表静态预留, 不走运行时分配" | 🟡 |
| 5 | **无"同步对象(fence)"契约** | `panfrost` 的 `in_syncs`/`out_sync` 是 syncobj(§2.3 #3); brickOS 只有 sem/cond/`br_work` —— 缺"可跨引擎排序 + 可由中断一次性信号 + 可等待"的对象 | 🟡 |
| 6 | **无 libm** | `runtime/posix/include/` 无 `math.h` | 🟡(若走 Mesa 编译器则必需) |
| 7 | **Mali MMU 页表要自己建** | Mali 有**自有** MMU(LPAE), 与 CPU MMU 无关。恒等映射能把它退化成静态 1:1 大页表, 但 **AS/Domain/`PANFROST_BO_NOEXEC`→XN/BO 的 GPU VA 恒非零**这些仍需正确编程 | 🟡 |
| 8 | **无 `mmap` 等价面** | `runtime/posix/include/unistd.h`: `mmap/mprotect` 明确 **`-ENOTSUP`(P-7)** | 🟢 影响小(单地址空间下 BO 的 vaddr 直接可用), 但 BO 的 **CPU 可见性/缓存属性**需要一个 native 表达(目前只能借 region 的 `BR_MM_CACHED` 位) |
| 9 | **GPU 不在任何版本清单** | `1-03` §1 各版插件清单无 GPU 条目; "未排期"节只提 `io/display`/`io/can`/`io/lin` | 🟢(排期) |
| 10 | 无 `eventfd`/`pipe` | 全树零命中 | 🟢 **与 GPU 无关**(那是 `8-03` libusb 的缺口), 列此仅为切断"图形栈一定需要它"的误判 —— **单内核下事件用 `br_sem`/`br_cond`, 不需要 fd** |

### 4.2 B. 设计契约侧(本篇的核心产出)

| # | 缺口 | 为什么它是设计问题(而非实现细节) |
|---|---|---|
| 1 | **D20 的子分类模型装不下 GPU** | `8-01` §3 的子分类(cdev/bdev/netdev/USB)全是 **I/O 形状**: 字节流 / 扇区 / 帧 / 包。GPU 的消费面是"**提交作业 + 同步对象 + 地址空间**" —— 三者都不属于"读写一个设备"。硬按 cdev 建模的结果是契约退化成"一个 ioctl 通道", 组合期看不见任何东西(`R-S4` 的框架件漂移风险反向发作)。**答案空间**: (a) 第四子分类 `gpu-core`(对称 netdev/usb), (b) 明确为**新的一类(加速器/引擎)**, (c) 先走**形态 C**(平台内建、私有 ops, `8-01` §1.2), 等第二个消费者出现再升形态。**不拍 = 形状会先被写进 golden**(D14: 后补字段 = 二进制不兼容) |
| 2 | **"共享缓冲"与"同步对象"缺契约 —— 而它们比 GPU 更基础** | 这是本类需求的**公共前置**: display(扫描输出)/ camera / VPU / 网络零拷贝 / USB 等时传输**全都要**。若让 GPU 先发明一套, 后面每个域都会再发明一套。⇒ **建议单独立项**(相当于 Linux 的 `dma-buf` + `dma-fence` 两个契约的 brickOS 等价物), GPU 只是它的第一个消费者。**先立它, GPU 才不会各造一套** |
| 3 | **APP 可达性没写** | 按 `brickie` §7.3 的硬禁则, **`app` 不得依赖 `ability`/`platform`** ⇒ GPU 能力必须经 **Interface 皮肤**(`iface-gles` / `iface-vulkan` / `iface-gpu`)才对 APP 可见 —— 与 `iface/posix`/`iface/min` 同型(`1-01` §7.3/D18)。**没有这条, GPU 就没有合法的 APP 面** |
| 4 | **`api_type` 归类争议重演** | 若 Mesa 编译器作为携带上游源码的移植件进来, 按 §7.3 **只有 `ability` 能消费 `third_party`**(native 不得依赖 third_party)⇒ 必须有一个 `ability` 包装件持有它。这与 **`8-02` §5#2(lwip 的 `api_type`)** 与 `8-03` §5#3 是**同一个问题** ⇒ 建议**一次拍板成通则**: "携带上游源码的移植件 = `api_type = third_party`; 其消费方必须是 `ability` 包装件" |
| 5 | **`1-01` §14.1 域支撑矩阵的诚实账** | 该表"仪表 · display/CAN/LIN/touch/UDS/日志"格现在写"**无**"。若产品确需 3D/计算, 必须新增"GPU/3D"行并显式写缺口与版本归宿(D26 的资产边界纪律: 不许用"将来会支持"填空) |
| 6 | **BO 与内存政策的对齐** | `1-01` §4.3 的 v1 = **恒等映射 + region 属性**; `3-04`/`br_mem.h` 规定大固定块走 **region 表静态预留**、DMA 走 `br_dma_alloc`。⇒ BO 面必须与这条政策对齐, 否则会出现"BO 从 TLSF 堆来, 无法保证物理连续/对齐/非缓存"这类**在页表属性上无法表达**的坑。**建议在 BO 契约里把"后备来源"写成显式字段**(page/contig/reserved) |

## 5. 边界论证

### 5.1 把"Mesa"拆成层: 哪层该有对应物

| 层 | Linux 上的载体 | 碰 OS 概念? | brickOS 处置 |
|---|---|---|---|
| ① 上游 IR + 优化 | `src/compiler/nir` | 否(纯计算 + 分配) | ✅ **可搬**(`third_party`) |
| ② Mali ISA 代码生成 | `src/panfrost/compiler` | 否 | ✅ **可搬 —— 最有价值的一块**(着色器编译是最难自研的部分) |
| ③ 描述符 / 命令流构造 | `src/panfrost/lib`(`pan_cs`/`pan_desc`/`pan_pool`) | 半(要 VA + BO) | ⚠️ **逻辑可搬, 接口要重写**(它假设 `pan_kmod` 存在) |
| ④ 设备 / 提交抽象 | `pan_kmod`(**DRM ioctl**) | **是** | ❌ 换成 brickOS native 面(§0) |
| ⑤ winsys / EGL / GL 分发 | `src/egl`、`src/gallium/auxiliary` | **是** | ❌ 无对应物(无进程/无 fd/无窗口系统) |
| ⑥ Vulkan runtime | `src/vulkan/runtime` 等 | **是**(更多: timeline semaphore / WSI / syncobj) | ❌ 同上; 且目标代际里只有 Bifrost v7 有 Vulkan 1.3 |
| ⑦ display / WSI | `kmsro` + 显示驱动 | 是 | ❌ brickOS 的 display 是**另一件事**(§5.3) |

**一句话: 搬 ①②, 重写 ③④, 不要 ⑤⑥⑦。**

### 5.2 三条候选路线

| 方案 | 做法 | 评价 |
|---|---|---|
| **A. 只搬"编译器" + 自研运行时(推荐)** | `third_party/mesa` 只取 `src/util` + `src/compiler/nir` + `src/panfrost/compiler`; `adapter/mesa-shaderc/{compat,glue}` 提供它要的环境; 自研 JD 构造 / 提交 / Mali MMU | ✅ 把最难写的借来; ✅ 不引入进程/fd/mmap; ✅ 编译器本体**不依赖 LLVM**(LLVM 在 Mesa 里是 llvmpipe/rusticl 等后端的事; 预编译着色器路径的 `mesa-clc`/`precomp-compiler` 是**宿主侧离线工具**)。⚠️ **体量大**: 取哪几个子目录、裁剪到什么程度、能不能编过 `-ffreestanding` + 无 libc, **必须在阶段 0 用实测(cloc + 试编)定, 本篇不给数字**(§10) |
| **B. 搬整套 Mesa + 在单内核里造一个 DRM 等价面** | 实现约 30 个 DRM/panfrost ioctl, 让 Mesa 原样跑 | ❌ **与 §0 直接冲突**: 先在无进程/无 fd 的内核里发明进程语义与 fd 生命周期, 再用它们把 Mesa 接回来 —— 净增一层纯人造边界; 且 ⑤⑥⑦ 三层在无窗口系统下没有落点 |
| **C. 不搬 Mesa, 手写最小驱动** | 照 Panfrost 的寄存器/MMU/JD 知识手写一条最小管线(清屏 → 三角形) | ✅ **最小、最快见硬件**(`8-02` §4 "搬知识不搬代码"的同型); ✅ 阶段 2/4 的判据不依赖任何 GL 面; ❌ 顶到"要跑真实 GLES 应用"之前就停住 —— 它交付的是**驱动与契约**, 不是 API 面 |

**推荐组合: C 起步 → A 补齐。** C 把驱动与契约验收到可交付(阶段 2–5), A 在有真实 GLES 诉求时再接(阶段 6–7)。**两者不冲突: C 的运行时正是 A 需要的那个运行时。**

### 5.3 为什么"2D 需求不该碰 Mali"

- **Mali 没有开源的 2D-only 驱动** —— 开源的只有 `lima`(GLES2 3D)与 `panfrost`(GLES/Vulkan/CL)。
- **Mali GPU 本身不含显示控制器**: 扫描输出是 SoC 的**另一个 IP**, Linux 上由 `kmsro` 把两者桥接。⇒ "用 Mali 出画面"这件事天然是**两件事**: GPU 渲染 + display 扫描输出。后者是 `io/display` 的活, 与 GPU 无关。
- 推论: 若 `1-03` §1"未排期"里 `io/display` 的真实诉求是 **2D 合成 / 扫描输出 / 图层混合**, 那么正确做法是 **CPU 渲染 + 显示控制器驱动**, **完全不进 GPU**。**这是一个能把工作量降一个数量级、且风险最低的裁剪** —— 排期前必须先问清"要 2D 还是要 3D/计算"(§6 G1)。

### 5.4 三层各选各的参照(`8-02` §4 的同型结论)

| 要学/要搬的东西 | 参照 | 方式 |
|---|---|---|
| **契约形状**(加速器的对外面) | RTOS/微内核的加速器抽象(`8-02` §4 同款方法论); 以及本仓 `cdev-core`/`bdev-core` 的骨架 | **学设计**(不搬代码) |
| **硬件知识**(寄存器 / MMU / JD / 中断语义) | `panfrost`(`panfrost_regs.h`、`panfrost_mmu.c`、UAPI 注释) | **学知识**(寄存器序列可搬运, 代码不搬) |
| **着色器编译** | Mesa 的 ①② 层 | **搬代码**(走 `third_party` + `adapter` 正规通道) |

## 6. 需拍板(7 项)

| # | 事项 | 为什么必须现在拍 |
|---|---|---|
| **G1** | **要不要 GPU**(先过 §5.3 的"2D 还是 3D/计算"关) | 一切的前置。若答案是"2D", 本方向的正确处置是**关闭**, 改走 `io/display` + CPU 渲染 |
| **G2** | **GPU 的设备模型**(第四子分类 `gpu-core` / 新的"加速器域" / 先形态 C) | 形状进 golden(D14)⇒ 一次占齐(含 `ioctl`/`suspend`/`resume` 的 D22 预留)。与 `8-01` O-S5(netdev)、O-S9(USB)属同一族问题, **建议合并评审一次定"子分类的成立判据"** |
| **G3** | **共享缓冲(BO)+ 同步对象(fence)契约的归属与先后** | 它是 display/camera/VPU/网络/USB 的**公共前置**(§4.2 #2)。不先立, 每个域各造一套 |
| **G4** | **边界取 A/B/C**(§5.2) | 决定要不要在单内核里人造 DRM 边界(B 是本篇唯一明确不推荐的) |
| **G5** | **FP/SIMD 政策** | 二选一: (a) 开 `CPACR_EL1.FPEN` + 定义**上下文保存策略**(`switch.S` 的 `ctx[13]` 要不要扩到含 `q0`–`q31`, 以及是"全局懒保存"还是"每线程必存"); (b) **全系统禁浮点**并把这条写成红线。**选 (b) 等于封死 M2 方案 A/B** —— 所以必须先拍 |
| **G6** | **时钟/电源/复位契约的归属**(平台数据 / 新框架件 / 平台内建) | `panfrost` 的上电时序是三路时钟 + domain + reset; brickOS 零对应物。**建议与 `8-01` O-S6(PM 调用方)合并拍** |
| **G7** | **排期 + `api_type` 通则** | ① GPU 的版本归宿(现在不在任何清单); ② `third_party` 归类通则 —— 与 `8-02` §5#2、`8-03` §5#3 是同一问题, **一次拍掉三处** |

## 7. 落地顺序(每阶段有可独立验收的判据)

| 阶段 | 内容 | 判据 | 依赖 |
|---|---|---|---|
| **0. 设计定稿 + 实测** | 拍 G1–G7; 写 GPU ADR; 回灌 `8-01`(O-S10→决策)/`1-03`/`1-01` §14.1; **实测 Mesa ①②层的体量与可编译性**(cloc + 试编) | 设计落盘; `brickie check` 能表达新插件的依赖; 体量数字有出处 | — |
| **1. 平台数据 + 最小电源面** | Mali MMIO region + **job/mmu/gpu 三条 IRQ 绑定** + (按 G6)最小"时钟/复位开关"声明 | region 计数 +1(`[MEMCONF]` 全绿); `br_irq_register(mali_job)` 成功 | 阶段 0 |
| **2. `io/mali-panfrost` 骨架(形态 C, 私有 ops)** | 探测 / `GPU_PROD_ID`·features 读回 / 上电 / **Mali MMU AS 建-拆** / cache 维护往返 | ★ **复用自检机制(ADR-0010)**: `[GPUCONF]` 判据 = **prod_id/rev 与器件一致 + AS 建拆往返(页表项读回正确) + 一次 clean+invalidate 往返(读回一致) + 三条中断各自真的到过**。**无 GL、无应用即可交付** | 阶段 1 |
| **3. 最小提交** | 一个"空 JD / 清屏"作业走完 提交 → JM 中断 → 完成(含**超时路径**) | 中断到 + 完成计数 + **故意造超时不 hang 且有恢复路径**(`drm_sched` 在 Linux 上承担的活, 这里必须自己补: §9 R-4) | 阶段 2 |
| **4. 最小绘制** | 手写 Midgard/Bifrost 指令流 → 画一个三角形到 BO | **读回 BO 像素校验**(定色 + 位置 + 边界), 不是"人眼看着像" | 阶段 3 |
| **5. 契约化** | 按 G2/G3 落 `framework/gpu-core` + BO/fence 契约 + Interface 皮肤 | `[GPUCONF]` 用**假 ops 表**全绿(非法注册/重名/未注册取回/提交-等待往返)—— 与 `8-02` 阶段 1 同型, **不需要真硬件** | 阶段 4;G2/G3 已拍 |
| **6. 着色器编译器(仅若判 A)** | `third_party/mesa`(①②层)+ `adapter/mesa-shaderc/{compat,glue}` | 一段 GLSL → Mali ISA 的编译产物与 Mesa 同源工具**逐字节一致**(判据来自上游, 不是自证) | 阶段 5;G5 判"开 FP" |
| **7. GL 面(仅若真需要)** | 最小 GLES 子集(或按 G7 的其它选择) | 一个 APP 画出**带纹理**的三角形(经 `iface-*`, 不直调 ability) | 阶段 6 |

**阶段 2 的验收设计是关键**(与 `8-02` 阶段 3 同型): brickOS 现无 GL 面, 所以"能画"不能当第一个判据。正确做法是**复用已有的自检机制** —— 与其他子系统(`[IRQCONF]`/`[MEMCONF]`/`[VFSCCONF]`)的验收方式完全一致: 自带判据 + 门禁逐 tag 点名。**它同时给出"无 GL 也能交付"的路径**: 阶段 2 的产物本身就有价值(证明硬件活着、页表对、cache 纪律对、中断到)。

## 8. 证据索引(可重跑)

> **E-1–E-13 的路径相对 `brickOS/prototype`; E-14/E-15 相对 `brickOS/Design`; E-16 需联网。** **⟳** = 本篇撰写时实测(含对"假命中"的标注 —— 这几条陷阱是实测踩出来的, 保留在这里免得下一个人再踩)。

| # | 命令 | 期望 |
|---|---|---|
| E-1 | `git log --oneline -1` | `e7cf0b5`(本篇基线)⟳ |
| E-2 | `grep -rn 'mgeneral-regs-only' tools/brickie/ docs/contract.md 2>/dev/null` | 命中 arch 标志 ⇒ **无 FP/SIMD 的编译面**⟳ |
| E-3 | `grep -rn -i 'cpacr' core platform --include=*.c --include=*.h --include=*.S` | 只命中 `br_fault.h:23` 与 `irq_fault.c:52` 的**注释/字符串**, 无使能代码 ⟳ |
| E-4 | `grep -n 'BR_CTX_\|ctx\[' core/src/sched/sched_ctx.h core/include/br/core/br_sched.h` | `ctx[13]` = callee-saved only, **无 `q0`–`q31`** ⟳ |
| E-5 | `grep -rn 'eventfd\|pipe(' core runtime framework` | **空**(且与 GPU 无关: §4.1 #10)⟳ |
| E-6 | `grep -rnw -i -e clk_get -e clk_prepare -e clk_enable -e devfreq -e regulator -e power_domain -e reset_control core framework platform` | **空** ⇒ 无时钟/电源/复位框架 ⟳(★ 不能用 `clk_`: 会撞 `platform/.../console_pl011.c` 的 `BR_PL011_REFCLK_HZ`, 得到假命中) |
| E-7 | `grep -n 'BR_IRQ_MAX' core/include/br/core/br_irq.h`;`grep -n 'BR_IRQ_BOARD_NR' platform/qemu-aarch64/include/br/board_irq.h` | `64u` / `11u` ⟳ |
| E-8 | `grep -c '= memmap_add(' platform/qemu-aarch64/src/memmap.c`;`grep -n 'BR_MM_MAX_REGIONS' core/include/br/core/br_mm.h` | 现声明 **11** 条 / 上界 **24** ⇒ 余量够 ⟳(★ 用 `= memmap_add(` 而非 `memmap_add(`: 后者会把函数定义行也算进去, 得到虚高的 12) |
| E-9 | `grep -n 'br_mm_map\|br_mm_unmap' core/include/br/core/br_mm.h` | `:81` 注释明写 v1 **一律 -ENOTSUP**(TC-MM-003)⟳ |
| E-10 | `grep -n 'mmap' runtime/posix/include/unistd.h` | 头注释列 `mmap/mprotect` 为 **`-ENOTSUP`(P-7)** ⟳ |
| E-11 | `ls io framework sched runtime/posix/include` | `io/` 只有 `uart-pl011`; 无 GPU 件; runtime/posix **无 `math.h`** ⟳ |
| E-12 | `ls third_party adapter`;`bash tools/check-third-party-pristine.sh` | `third_party/` 只有 `uboot-imx`; 三方隔离门禁在 ⟳ |
| E-13 | `grep -rnw -i -e mali -e panfrost -e gpu core io framework runtime service app platform adapter` | **空** ⇒ 本篇是全新方向 ⟳(★ **必须用 `-w`**: 裸 `grep -i mali` 会命中 `memalign`(`me`**`mali`**`gn`), 在 `adapter/uboot-*` 里遍地假命中) |
| E-14 | `grep -n 'O-S10' docs/8-device/8-01-device.md` | 命中 §8 决策表的 O-S10 行 ⇒ 本篇回灌**已完成**(改前该 grep 为空)⟳ |
| E-15 | `grep -n 'display\|未排期' docs/1-architecture/1-03-roadmap.md`;`grep -n '仪表' docs/1-architecture/1-01-architecture.md` | `1-03` 未排期节列 `io/display`;`1-01` §14.1 仪表行 = **"无"** ⟳ |
| E-16 | 外部: `curl -sL https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/drm/panfrost_drm.h \| grep -n 'jc\|PANFROST_BO_HEAP\|in_syncs\|BO_SYNC\|SELECTED_COHERENCY'` | 命中 §2.3 的四条事实(逐字)⟳ |

## 9. 风险

| # | 风险 | 说明 |
|---|---|---|
| R-1 | **"GPU"是排期黑洞** | GPU 不在任何版本清单(`1-03` §1);Mesa ①②层体量未实测。**规避 = 阶段 0 先出实测数字, 再谈排期**;阶段 2 的自检验收保证"第一个可交付物"很小 |
| R-2 | **FP/SIMD 政策悬空会污染全局** | 若先按"禁浮点"写完大量代码, 后来为 GPU 开 FP ⇒ 需要回头定义上下文保存策略并重新评审所有 `SAFE_PREEMPT` 假设(§6 G5)。**它是 core 级决策, 不该由 GPU 需求倒逼** |
| R-3 | **BO/fence 契约被 GPU 抢跑** | GPU 先落地一套私有 BO ⇒ display/camera/VPU 各自再发明(§4.2 #2)。**规避 = G3 先于阶段 5** |
| R-4 | **无 GPU 抢占与 hang 恢复** | Linux 上 `drm_sched` 承担的活(同 VM 串行/优先级/超时检测/reset 恢复/`enable_signaling`)在 brickOS 必须自己补; 漏掉 = **GPU hang 后无恢复路径**。且 `1-01` §2.2 的"单异常级"意味着没有"复位整个进程"这种廉价兜底。**阶段 3 必须把超时路径当判据** |
| R-5 | **cache 一致性是最难查的故障** | `PANFROST_BO_SYNC` 的存在证明非一致集成是常态; `br_mm_cache_*` 只对**声明为 cached 的 region** 有效, 地址未声明即 `-EINVAL`(不猜属性, `br_mm.h`)。⇒ BO 的缓存属性必须在契约里显式, 不能靠"记得刷" |
| R-6 | **协作/抢占调度下的延迟** | GPU 作业完成靠中断唤醒; 与 `8-02` R-1 同源(无抢占时唤醒延迟受显式切换点限制), 归宿同为 v2 的 preempt |
| R-7 | **Mesa 上游漂移** | 若走方案 A, 三方件要钉 commit 并靠 `adapter/` 吸收差异(ADR-0024/0026 的现成纪律)。⚠️ Mesa 是**活跃大型上游**, 补丁面可能很大 —— **这是方案 A 的真实代价, 须在阶段 0 量化** |
| R-8 | **`kbase` 的诱惑** | "它不需要 DRM, 与单内核天然契合"这个论点**成立但无用**: 它的消费面只有专有 `libmali`(不可再分发), 或不在上游的第三方 PanVK 分叉(维护性/许可未评)。**结论: 不作为候选**(§2.1) |
| R-9 | **CSF 固件的法务线** | 本篇因目标代际是 JM 而**完全避开**; 但若未来代际迁移到 G310+/G610+, `mali_csffw.bin` 的 ARM EULA 含"不得用于**规避 ARM 在该软件中的 IP**"条款并禁逆向 ⇒ **换硬件代际 = 重开一次法务评估**, 不是工程顺延 |

## 10. 未能核实 / 免责

1. **Mesa ①②层的体量与可编译性未实测**(NIR + `src/panfrost/compiler` 取多少子目录、能否 `-ffreestanding` 编过、依赖 `src/util` 的哪些部分)。**这是方案 A 的唯一关键未知量**, 已排进阶段 0; 本篇**不给行数估计**(给了就是猜)。
2. **`panfrost` 内核侧对 JD 头部的读取范围**(哪些字段进内核、哪些纯属硬件)未逐字段核实。本篇只断言 UAPI 能证明的部分(`jc` = JD 的 GPU 地址; `requirements` 由 ioctl 单独传)。JD 布局的权威在 Mesa 的 `pan_desc.c` 一侧。
3. **tiler heap 在 panfrost(JM)侧的管理归属**未逐行核实。可确认的是**用户态可以创建 heap 型 BO**(`PANFROST_BO_HEAP` 与其 `WB_MMAP` 的互斥规则); 具体增长/回收策略属消费面。⇒ 阶段 0 应用 Mesa 的 `drm-shim`/`PAN_MESA_DEBUG=trace` 抓一次**真实 ioctl 序列**, 把"必须实现的清单"从 30 收敛到实际用到的十几个。
4. **缓存维护的具体 asm 序列**(`dc cvac`/`dc civac` 等)未从一手文档核实; 本篇只断言 UAPI 层的两档语义与内核层的 `dma_sync_*` 抽象。
5. **`llvmpipe` 在 aarch64 上的支持级别**未从一手源确认(官方文档只列 x86/x86-64/ppc64le JIT 目标)。**若不需要软件 GL, 此项与本篇无关**(§2.2)。
6. **Zephyr / RT-Thread / SerenityOS / Genode 的图形与 GPU 支持**未抓一手源 ⇒ 不作为任何结论的依据; §5.4 的"契约形状"参照因此只写方法(学设计), 不点具体实现。
7. **外部状态时点**: 调研依赖的搜索面受限(`web_search` 在本次环境不可用; Phoronix 被 Cloudflare 拦截), 因此**维护状态类结论**改用一手替代证据(kernel.org/Mesa 官方文档 + 上游 commit API)。Linux 7.3-rc / Mesa 25.2 之后的漂移未跟踪。
8. **`libmali` 的确切 EULA 与版本矩阵**未核实(ARM 站点为 JS 渲染)。本篇对它的结论只依赖两条可确证事实: 不在任何通用发行版/开源仓库、且只讲 kbase UABI。**该结论足以支撑 §2.2 的"不可用"判定**。

> **外部一手来源**(§2 的引用出处): [panfrost Kconfig](https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/panfrost/Kconfig) · [panfrost UAPI](https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/drm/panfrost_drm.h) · [panthor Kconfig](https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/panthor/Kconfig) · [lima Kconfig](https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/lima/Kconfig) · [Mesa panfrost 能力矩阵](https://docs.mesa3d.org/drivers/panfrost.html) · [Mesa lima](https://docs.mesa3d.org/drivers/lima.html) · [Mesa 25.2 relnotes](https://docs.mesa3d.org/relnotes/25.2.0.html) · [drm-shim](https://docs.mesa3d.org/drivers/panfrost/drm-shim.html) · [dma-buf](https://docs.kernel.org/driver-api/dma-buf.html) · [mali_csffw EULA](https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main/LICENSES/LICENCE.mali_csffw) · [kbase DDK 镜像(GPL-2.0 头)](https://raw.githubusercontent.com/LibreELEC/mali-midgard/TX011-SW-99002-r28p0-01rel0/driver/product/kernel/drivers/gpu/arm/midgard/mali_kbase_core_linux.c)
