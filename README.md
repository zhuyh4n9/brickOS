# brickOS-prototype v0.1.0

brickOS 的**可运行原型**。分支 `brickOS-prototype-v0.1.0`(从 `main` 开出)。

> **仓库是"设计 / 原型"双线分行的**:
> - 设计文档在 **`brickOS-Design`** 分支(`docs/` + `comment/`);
> - 原型代码在本分支(仓库根 `brickOS/`), `main` 上只有 `LICENSE`。
>
> 所以本分支**没有** `docs/`。下文引用设计文档时写的是"章节号", 去 `brickOS-Design`
> 分支上读(如 `docs/3-os-core/3-01-core-api-list.md` = `3-01`)。
>
> **落点约定**: 开发中的原型树 = 仓库根 `brickOS/`(core / platform 两个顶层);
> 一个版本稳定后再冻结为 `brickOS-prototype-v<版本>/`(或按分支/标签冻结), 冻结件只读。

---

## 1. v0.1.0 做什么

**一条链, 三个环节:**

```
(Platform 插件) aarch64 Start.S ─► br_irq_cpu_init ─► br_plat_early_init ─► (Core+APP) br_core_main
                                                       │                        │
                    早期 console + GICv3 PIC + IRQ 绑定表                        ├─ 中断一致性用例(TC-IRQ-*, 67 项)
                    + region 表 + 4 KiB 恒等映射页表(开 MMU)                     ├─ 内存/MMU 一致性用例(TC-MEM/TC-MM)
                    + 三池认领(br_mem_init)                                      ├─ 调试域一致性用例(TC-DBG-*)
                                                                                ├─ 中断心跳(timer PPI INTID 30)
                                                                                └─ MainLoop(延时 + 日志 + 现场快照)
```

| 环节 | 内容 | 代码 |
|---|---|---|
| **Platform Entry**(`platform/qemu-aarch64` 插件) | reset、选核、EL2→EL1 降级、栈、**异常向量表**、BSS 清零 | `platform/qemu-aarch64/src/start.S` / `vectors.S` |
| | 早期 console(PL011 轮询)、arch timer 读数 | `platform/qemu-aarch64/src/console_pl011.c` / `timer_arch.c` |
| | **GICv3 驱动**(`br_pic_ops` 方言) + 板级 IRQ 绑定表 + 中断初始化/开跑 | `platform/qemu-aarch64/src/gicv3.c` / `board_irq.c` |
| | **4 KiB 恒等映射页表**(4 级 / TTBR0 / MAIR / 低 1 GiB Device 块 + 8 MiB RAM 页窗)+ **region 表** + 开 MMU | `platform/qemu-aarch64/src/mmu.c` / `memmap.c` |
| | **中断一致性用例**(target-only, 编进镜像) | `platform/qemu-aarch64/src/irq_conf.c` |
| | **内存/MMU 一致性用例**(target-only) | `platform/qemu-aarch64/src/mm_conf.c` |
| | 链接脚本(含 `.br_extable` 收集) | `platform/qemu-aarch64/src/link.ld` |
| **APP**(`app/hello` 插件) | MainLoop: 每秒打一行日志, 延时自带"不早醒"判据; 只**读**平台的心跳计数(纯 P0 消费者); 依次驱动三套一致性用例 | `app/hello/src/main.c` |
| **调试服务插件**(`service/*`) | `trace`(core 16B 事件环的唯一消费方)/ `backtrace`(x29 帧链捕获)/ `hexdump`(16 B/行契约格式)/ `memleak`(按归属标签出账)/ `dump`(现场编排 + `[DBGCONF]` 入口) | `service/*/src/*.c` |
| **Core**(内核本体, 不是插件) | **中断框架 Stage 1**: 号空间/描述符池、生命周期(ack→ISR→eoi 单出口)、三层屏蔽、优先级语义、级联域(FAST)、fault/extable、最小 trace 环 | `core/src/irq/*.c`, `core/src/{panic,trace}.c` |
| | **内存**: TLSF 堆(红区 + 毒化 + owner 记账)、contig/DMA 池、4 KiB 页位图池、region 表 + `br_mm_ops` 派发 + cache 维护 | `core/src/mem/*.c`, `core/src/mm/mm.c` |
| | 时钟换算(us)+ 忙等延时 | `core/src/time.c` |
| | 日志(格式化 + 等级过滤), 不走 libc printf | `core/src/log.c` |
| | **宿主侧内存语义门禁**(算法性质压测, 不需要 QEMU) | `tests/host/mem_test.c` |

**验证目标**(不是"能编译"): 镜像能在 QEMU virt 上从 reset 跑到 MainLoop, 日志时间戳单调,
且每次延时**实际不短于请求值**(设计 `3-01 §2.1` 的"不早醒"语义)。后一条是**自动化判据**,
不是人眼看着差不多 —— `make smoke` 会 grep 它。

三套一致性用例都有**自动化判据**, 而且是**真跑硬件/真跑页表路径**(不是静态检查):

- `make irq-test` —— 67 项逐用例判 PASS/FAIL(设计 `6-01 §3.7` 的 `TC-IRQ-*` + GICv3 方言事实);
- `make dbg-test` —— `[MEMCONF]`(设计 `6-01 §3.5/§3.6` 的 `TC-MEM-*`/`TC-MM-*` + 恒等映射/RO/NX/未映射 fault)
  与 `[DBGCONF]`(调试插件域 `TC-DBG-*`)两个摘要必须 `fail=0`, **且逐个用例 tag 一个不缺**;
- `make mem-test` —— 把 `core/src/mem/*` + `core/src/mm/mm.c` 编成**宿主可执行**, 跑 20 万次随机
  交错分配/释放 + 参考模型对拍(TLSF 的合并/分裂/碎片与页位图的 run 分配是算法性质, 宿主上几秒
  能跑上百万次操作);
- arch timer 的 PPI(INTID 30)在跑, 日志的 `irq_ticks` 每秒 +10 —— 真实中断投递的活证据。

## 2. v0.1.0 刻意不做什么

原型最容易失控的地方是"顺手多做一点"。以下是**明确不做**的, 连同理由:

| 不做 | 理由 |
|---|---|
| **插件化(描述符 / manifest / 组合器)** | 这正是组合器 brickie 与插件管理器的产出(`4-02`/`4-03`, `3-05`)。**声明期 + 编译编排**已由 brickie v0.1 接管(插件发现 / 校验 / 描述符生成 / 接口发布 / 版本治理 / `brickie build`; 见 [`tools/brickie/README.md`](tools/brickie/README.md)), 但**运行期**的插件管理器与描述符段(`.br_plugins`)仍未落地 ⇒ `platform/qemu-aarch64` 与 `app/hello` 虽已是插件且源集合由声明面给出, 启动链的**调用点**仍是 APP 直调。**Platform Entry 因此仍挂 WORKAROUND, 见 §5** |
| 调度器 / 线程 / `br_sched_ops` | M1。没有调度器就没有"可让出的对象", 所以睡眠只能是忙等 |
| **中断的 Stage 2**(分发到 bh/线程、亲和性/均衡/IPI、`CAP_NEST` 嵌套、PM save/restore) | 设计 `3-02 §1.1` 的分期: Stage 1(无调度器世界)已落地, Stage 2 的前提是**调度器与 bh** —— 它们是 M1 |
| `br_fault_handler_register`(fault handler 链) | 设计 `3-02 §10.5`: 分槽/分类/extable/panic 属 Stage 1(已做), **注册 API 归 v2** |
| **SLOW 级联域**(状态需总线事务的 PMIC 型) | 设计 `3-02 §1.1.1`: 它的存在前提就是 bh; v0.1 明确**拒绝**(`br_irq_domain_create` 返回 NULL + 留痕), 不是假装支持 |
| MMU 的**重定位**(镜像加载任意 VA)/ MPU 目标(vx) | 设计 `3-04 §1` 的虚拟内存政策: v1 = **恒等映射 + 属性隔离**(已落地); 重定位归 v2, MPU 归无 MMU 目标的实验 |
| per-plugin arena **预算**(memleak 的 v2 形态) | 现在给的是 v1.x 便宜层(红区 + 毒化 + 双 free 拦截)加一个"归属标签"近似: 能报**谁没还、在哪分配的**, **不能**报"某插件超预算"(无上限/无强制归属/OOM 策略)—— 见 ADR-0003 §2.8 与 `br-wa-debug-001` |
| debug bridge(COBS + CRC16 成帧 + `MEMRD`/`TRACE_READ` 命令面) | `5-01 §2` 的 M3。现在 dump/trace **直写早期 console**(同类约束照守: 只用静态缓冲、不碰堆、只读), 见 `br-wa-debug-002` |
| 栈 canary / 目标侧完整 ASan | `5-01 §4` 的分层: 红区 + freelist 毒化已在; 栈 canary 需编译器插桩, 完整 ASan 是 host 平台插件的白捡项与 vx 实验 |
| `br_fault_handler_register` 的消费方(ramdump) | v2(`3-02 §10.5`); 挂接点与现场结构已在, 缺的是注册 API 与捕获插件 |
| `br_mm_map`/`br_mm_unmap` 的**运行期实现** | v1 按设计返回 `-ENOTSUP`(`6-01` 的 `TC-MM-003` 就是这个期望): 签名先行、实现随 v2 重定位; 运行期**改属性**走 `br_mm_set_attrs`(已可用, 用例用它做真 RO 保护验证) |
| 设备注册 / devfs / cdev | M2。早期 console 是**轮询**形态 —— 这是设计内的 M0 形态(`1-01 §8` console 双形态), **不是** workaround |
| libc / svc-posix | `-nostdlib -ffreestanding`; 只有 `stdarg.h`(编译器自带) |

## 3. 目录结构

```
brickOS/
├── Makefile                     只编工具 + 镜像侧的过渡别名(真身 = brickie build)
├── README.md                    本文件
├── WORKAROUNDS.md               WORKAROUND 登记表(欠债清单)
├── product.toml                 产品声明: app 选择 / 插件选择 / 预算 / M0 引导例外豁免
├── mk/
│   └── host.mk                  宿主三元组 + 宿主产物目录(build/host/… 与 prebuilts/…)
├── docs/decisions/              本原型的决策记录(如 0001-platform-plugin-manifest.md)
├── prebuilts/toolchain/                    外部工具链**下载缓存**(派生, 不进库; fetch-prebuilt.py)
├── prebuilts/                   宿主工具**自举种子**(进库; 见下, 详见其 README)
│   └── seed/brickie/<host-arch>/<host-os>/bin/{brickie,brickie-gen,brickie-core}
├── tools/
│   ├── host-detect.sh           宿主 arch/os 探测(布局映射的唯一真值)
│   ├── check-workarounds.sh     源码标记 ↔ 登记表 一致性检查
│   ├── check-build.sh           构建接线门禁(宿主产物出树 / 自举种子进库)
│   └── brickie/                 【组合期工具】brickie(原名 `br`; 详见其 README)
│       ├── Makefile             宿主 g++/cargo + python3, **不碰交叉工具链**
│       ├── freeze.py            打包器: Python 包 + 模板 + schema + 原生工具 → 嵌入 ELF 的载荷
│       ├── cxx/                 L2 渲染器 + L5 入口 ELF 的源码(产物出树, 见下)
│       ├── rust/                L0/L1 Rust 核心 brickie-core(判定 / 求解 / 版本 / IFACE-IR)
│       ├── python/brickie/      L5 前端(子命令 / 输出 / 退出码 / 文件编排)
│       ├── schema/*.schema.json 声明面形状(机器可读)
│       ├── docs/contract.md     跨语言接口契约(唯一权威)
│       ├── templates/           骨架模板(四类 × c)
│       └── tests/               端到端用例
├── app/hello/                   【APP 插件】M0 MainLoop(镜像唯一 app; M0 引导例外)
│   ├── plugin.toml              人写 ← 插件级唯一真值
│   └── src/main.c               MainLoop(延时 + 日志 + 读心跳计数; 原 core/src/startup/main.c)
├── core/                        【Core】内核本体(**不是插件**; 被 [compat].core 引用)
│   ├── include/br/core/
│   │   ├── br_types.h           基础类型(不用 <stdint.h>)
│   │   ├── br_error.h           负 errno 子集(设计 3-01 §11)
│   │   ├── br_version.h         版本标识
│   │   ├── br_console.h         早期 console 契约(接口在 core, 实现在 platform)
│   │   ├── br_log.h             日志契约
│   │   ├── br_time.h            时钟/延时契约
│   │   ├── br_irq.h             【中断】native API(9 件)+ 域 API + 绑定表 + 状态字
│   │   ├── br_pic.h             【中断】PIC 填表契约(core ↔ ISA 方言, 非 native 面)
│   │   ├── br_exc.h             异常帧布局(asm 桩 ↔ fault 路径的同一处真值)
│   │   ├── br_fault.h           fault 分类 / extable 宏 / panic 两入口
│   │   ├── br_trace.h           最小 trace 环(ISR-safe 留痕)
│   │   ├── br_mem.h             【内存】native 11 件 + 观测契约(堆遍历/自检)+ 归属记账
│   │   ├── br_mm.h              【内存】region 表/属性 + MMU 派发 + 平台侧 br_mm_ops_t
│   │   ├── br_plugin.h          【插件】元契约(描述符/依赖边/相位)+ 插件管理器入口
│   │   ├── br_svc.h             【插件】服务注册表(publish/lookup + 观测)
│   │   ├── br_sched.h           【调度】线程/任务 + br_sched_ops 注册点 + idle
│   │   ├── br_sync.h            【调度】mutex / sem / cond / spinlock
│   │   └── (br_main.h 已删: 入口由插件描述符的 start 钩子承担)
│   └── src/
│       ├── log.c                格式化 + 等级过滤
│       ├── time.c               时钟换算 + 忙等延时 + br_deadline_from_now
│       ├── plugin/              插件管理器(扫段/拓扑/相位驱动/失败停机)
│       ├── svc/                 服务注册表
│       ├── sched/               调度框架(TCB/切换 asm/超时表/idle)+ 等待链原语
│       ├── sync/                同步原语(mutex/sem/cond/spinlock + 一致性套件)
│       ├── panic.c              br_panic_bare / br_panic(无锁/无堆/无调度器)
│       ├── trace.c              16 B 定长事件的环形缓冲
│       ├── string.c             ★ 编译器支持例程(memcpy/memmove/memset/memcmp)
│       ├── string_internal.h    上面的原型(core 私有; 它们不是 native API)
│       ├── panic_internal.h     panic 重入护栏(fault 路径判定"正在 panic 打印")
│       ├── mem/                 【内存】三池 + 堆
│       │   ├── tlsf_internal.h  TLSF 参数字典 + 单实例接口(core 私有)
│       │   ├── tlsf.c           纯 TLSF(二级位图/分裂/立即合并)
│       │   ├── mem.c            br_mem_init + malloc 族 + contig/DMA + 红区/毒化 + owner 记账
│       │   └── page.c           页位图 + 连续 run 首次适配
│       ├── mm/                  【内存】region 表 + MMU 派发
│       │   └── mm.c             region_add/find + map/unmap(-ENOTSUP)+ cache + ops 派发
│       └── irq/                 【中断框架 Stage 1】
│           ├── irq_internal.h   描述符/运行期表/CPU-local/域 的结构(32 B / 8 B 静态断言)
│           ├── irq_pic.c        PIC 注册表 + 绑定表 + hwirq→virq(有序表 + 二分)
│           ├── irq_core.c       描述符池 / lock-unlock / register-enable-disable / 风暴 / 入口
│           ├── irq_domain.c     级联域池 + 窗口切片 + FAST demux + 逐子 ack
│           └── irq_fault.c      fault 入口 + extable 查找 + double-fault 兜底
├── service/                     【调试服务插件】(每个目录 = 一个插件; 顶层 = namespace)
│   ├── trace/                   core 16 B 事件环的唯一消费方(drain/解码/计数/呈现)
│   ├── backtrace/               x29 帧链捕获(符号化归 host 离线工具)
│   ├── hexdump/                 16 字节/行契约格式(地址 + hex + ASCII)
│   ├── memleak/                 按归属标签出泄漏账 + 红区/毒化违约计数
│   └── dump/                    现场编排(regions/heap/leaks/trace/backtrace)+ [DBGCONF] 入口
├── tests/host/                  【宿主侧用例】不需要交叉工具链/QEMU
│   ├── mem_test.c               TLSF/页位图/region 表: 20 万次随机操作 + 不变量
│   ├── string_test.c            编译器支持例程: 语义 + 越界哨兵 + 参考实现对拍
│   └── host_stubs.c             br_log_write / br_console_* 的宿主替身
└── platform/qemu-aarch64/       【Platform 插件】与平台/ISA 绑定的部分
    ├── plugin.toml              人写 ← 插件级唯一真值(声明两个接口单元: #plat / #gicv3)
    ├── include/br/
    │   ├── board_irq.h          板级 virq 名(设计 3-02 §3.1 的"退路": 手写静态头)
    │   └── platform/
    │       ├── br_plat.h        Platform Entry 契约(含 IRQ/内存初始化 + 两套一致性入口)
    │       ├── br_gicv3.h       GICv3 方言契约(ISA 层)
    │       └── br_mmu.h         页表/MMU 观测面 + 窗口常量(ISA 层)
    ├── src/
    │   ├── start.S              入口: reset / 选核 / BSS / br_irq_cpu_init / 交 core
    │   ├── vectors.S            16 槽异常向量表 + 保存/恢复桩(ISA 层)
    │   ├── link.ld              链接脚本(text/rodata/data + .br_extable 收集 + .stack)
    │   ├── plat_qemu_virt.c     平台身份 + early_init + 异常兜底
    │   ├── console_pl011.c      PL011 轮询 putc
    │   ├── timer_arch.c         CNTFRQ_EL0 / CNTPCT_EL0
    │   ├── gicv3.c              GICv3 方言(实现 br_pic_ops_t)
    │   ├── board_irq.c          绑定表 + 中断初始化 + timer PPI 心跳 + 触发/hwirq 查询
    │   ├── irq_conf.c          中断一致性用例(TC-IRQ-*, 编进镜像)
    │   ├── memmap.c            region 表声明(镜像/三池/MMIO/保留区)+ br_mem_init
    │   ├── mmu.c               4 KiB 恒等映射页表 + 开 MMU + br_mm_ops(ISA 层)
    │   └── mm_conf.c           内存/MMU 一致性用例(TC-MEM-*/TC-MM-*, 编进镜像)
    └── tests/smoke.toml         用例骨架(v0.1 不消费; 已登记 TC-IRQ 梗概)
```

跨层边有两条, 都在 `product.toml [lint].allow_edges` 里**逐条列名豁免**(M0 引导例外;
设计的正解是 app → interface(iface-min, M2)/ 运行期插件管理器, 二者都还没落地):

- **Platform Entry → Core**: `start.S` 调 `br_irq_cpu_init()` + `br_plat_early_init()`
  再调 `br_core_main()`;
- **APP → Platform / 调试服务**: `app/hello` 直调 `platform/qemu-aarch64` 的
  `br_plat_name()` / `br_plat_irq_conformance()` / `br_plat_irq_start()` /
  `br_plat_mem_conformance()`, 以及 `service/dump` 的 `br_dump_conformance()` /
  `br_dump_all()` / `br_dump_trace()` —— APP 只认识 **dump 一个面**, 其余四个调试
  插件的 LATE 相 init 与 selftest 由 dump 按 `[[dep]]` 依赖序代调。

**中断的三层归属**(设计 `1-01 §8` / `3-02 §1.3`)在本原型的落点: 机制在 **core**、
GICv3 寄存器序列在 **`gicv3.c` + `vectors.S`**(设计归 ISA 共享库, 原型同目录 + 文件边界,
欠债 `br-wa-isa-001`)、绑定表/基址/静态 prio 在 **platform 数据**。

## 4. 构建与运行

构建的**入口是 `brickie build`**(ADR-0003 的 S1/S4 + ADR-0004), 顶层 `Makefile` 只编工具:

```
① tools/    工具段(顶层 `make`, 缺省目标)  宿主 g++ 编 C++ 渲染器 + Python 前端;
                                            cargo 编 Rust 核心 → 合并进同一个自包含入口 ELF
                                                          │  不碰交叉工具链(组合期零编译依赖)
                                                          ▼
② 声明面    product.toml [build] + 各插件 plugin.toml [build] + platform 的 [build.target]
            + tests/gates.toml(门禁)  ── 这是**镜像的唯一真值**
                                                          │
                                                          ▼
③ 镜像       `brickie build`(入口 ELF): 组合期 check → gen 生成物 → 编译/链接
            → `[build].post` 门禁; 步骤计划由 brickie-core 给, L5 只执行
```

**`make` 是"编工具", `make all` 是"编镜像"的过渡别名**: 顶层 Makefile 里
`all` / `run` / `smoke` / `irq-test` / `mem-test` / `string-test` / `dbg-test` /
`check-string` / `check-headers` / `size` / `disasm` / `clean-brickos` 都只是**薄委派**
(调 `$(BRICKIE) build|run|test …|size|disasm|clean`), 真身在 `brickie build` 与
`tests/gates.toml`。源码集合 / 编译标志 / 链接规则**不再**出现在顶层 Makefile 里 ——
它们由声明面决定(见 [docs/decisions/0004-brickie-build-and-make-retirement.md](docs/decisions/0004-brickie-build-and-make-retirement.md))。

这条"工具只编工具 / 镜像只委派 / 声明面是唯一真值"的纪律由 `make check-build` 把关 ——
接线坏起来通常是**静默**的(比如把镜像源码或标志又写回 Makefile, 构建就有了第二处真值),
所以钉成六条能真报红的机械判据(缺省目标=工具 / 无字面源码与标志 / 委派点齐全 /
工具段零交叉依赖 / 宿主产物出树 / 自举种子三件齐)。

**产物落点(参考 Android)**: 工具是**宿主**程序, 一律出树到
`build/host/<host-arch>/<host-os>/` 下, 与镜像产物(`build/obj`、`build/brick.*`)
分居 `build/` 两侧, 源码树里不留任何 `.o`/可执行文件:

```
build/host/<host-arch>/<host-os>/bin/brickie        # 单文件自包含 ELF(前端 + 模板 + schema + 两个原生工具)
                              …/bin/brickie-gen     # L2 渲染器(宿主可执行)
                              …/bin/brickie-core    # L0/L1 Rust 核心(宿主可执行)
                              …/lib/libbrickie-gen.a # 宿主静态库
                              …/obj/cxx/*.o          # 中间产物
```

`<host-arch>` = 处理器架构(`x86-64` / `aarch64` / …), `<host-os>` = 操作系统
(`linux` / `darwin` / `win`)。映射只在 `tools/host-detect.sh` 一处; `mk/host.mk`
把它变成 make 变量, `python/brickie/hostinfo.py` 是它的镜像(由用例断言同口径)。
查当前宿主:

```bash
make print-host-triple        # x86-64/linux
make print-host-bin-dir       # …/build/host/x86-64/linux/bin
make print-prebuilt-bin-dir   # …/prebuilts/seed/brickie/x86-64/linux/bin
```

**Python 前端 + 原生工具都在一个 ELF 里**: `brickie` 是**单文件自包含入口 ELF** ——
由 `tools/brickie/cxx/launcher.cpp` 把 Python 包、模板、`schema/**` 与**两个原生工具**
(`brickie-gen`、`brickie-core`)经 `tools/brickie/freeze.py` 打成未压缩 tar 后嵌进二进制,
运行时解包到临时目录再用系统 `python3` 解释, 并把 `BRICKIE_GEN` / `BRICKIE_CORE` 指到
解包出来的内嵌工具。于是**只拷 `brickie` 一个文件**就能跑: 不需要 `PYTHONPATH`、
不需要源码树、不需要同目录的原生工具、不需要 `g++`/`cargo`; 且**零新增第三方依赖**
(不用 PyInstaller/Nuitka)。详见 [tools/brickie/README.md](tools/brickie/README.md) 与 ADR `0004` §7。

**自举种子(进版本库, `prebuilts/`)**: 除"本机刚编的" `build/host/**` 外, 同一套
宿主三元组下还随源码提交预编译件:

```
prebuilts/seed/brickie/<host-arch>/<host-os>/bin/brickie        # ★ 单文件自包含(内含两个原生工具)
                                           /brickie-gen    # L2 渲染器(冗余副本, 供开发态直用)
                                           /brickie-core   # L0/L1 Rust 核心(冗余副本)
```

它让**没有 `g++`/`cargo` 的全新 checkout** 也能直接跑 `brickie`(`brickie` 自带代码、
模板、schema 与原生工具; 开发态的 Python 前端找不到 `build/` 也会退到种子), 也是将来
**用 brickie 自举管理 brickie 自身编译**的"第一块砖"。三个可执行必须**同时**在位
(缺 `brickie-core` ⇒ `make tools-prebuilt` FAIL)。改了工具源码后重新发布:

```bash
make tools-prebuilt          # 编工具(brickie/brickie-gen/brickie-core) → 发布种子(cmp 相同则不写盘)
make tools-prebuilt-check    # 只检查三件种子是否落后于源码
```

> **`prebuilts/` 下分两侧**(单一顶层目录, 见 ADR `0005`):
> `seed/`(**进库**, 就是上面这份自举种子)与 `toolchain/`(**派生、不进库**, 外部工具链)。
> 原先"两个顶层目录只差一个 `s`"的歧义已消除。详见 [prebuilts/README.md](prebuilts/README.md)。

### 配置开发环境(`setup.sh`)

把 `prebuilts/` 的工具链与 brickie 放进当前 shell(**必须 `source`** —— 子进程改不了父 shell):

```bash
source setup.sh              # 配置 PATH + 环境变量
source setup.sh --quiet      # 静默
source setup.sh --with-make  # 额外把 prebuilts 的 make 放到最前(会遮蔽宿主 make)
source setup.sh --unset      # 撤销(逐字节还原)
```

| 项 | 内容 |
|---|---|
| `PATH` 前置(左优先) | `build/host/<triple>/bin`(**本机新编优先**) → `prebuilts/seed/brickie/<triple>/bin` → `prebuilts/toolchain/<arm…>/bin` → `prebuilts/toolchain/ninja/bin` |
| 环境变量 | `BRICKOS_ROOT` · `BRICKOS_HOST_TRIPLE` · `BRICKIE_REPO_ROOT` · `BRICKIE_TOOL_ROOT` · `CROSS_COMPILE` |

设计取舍: **默认不加 `make`** —— 它会遮蔽宿主 `make`(构建内部本来就用 `prebuilt` 的
`$(MAKE)`, 见 ADR-0002 §7.5); **不设 `BRICKIE_GEN`** —— 那会盖掉"本机新编优先"的查找顺序。
CI 里不想 `source` 就用 `eval "$(make -s env)"`(同一份片段)。

> 工具链还没取件、工具还没编时, `setup.sh` 照样工作, 只是把缺失目录列出来并提示
> `make prebuilt` / `make tools`(PATH 里放不存在的目录无害)。

**工具段**只要宿主 `g++` + `python3`(≥3.11); L0/L1 的 Rust 核心另需 `cargo`, **都不需要
交叉工具链**:

```bash
make tools              # 只编工具 L5/L2(没装交叉编译器的机器/CI 工具作业可用)
make tools-core         # 只编 L0/L1 的 brickie-core(cargo; 出树到同一 bin 目录)
make tools-test         # 工具自身用例: 渲染器自检 + 端到端回归(含 V-1…V-19 / §V-B 构建族)
```

**镜像段**的工具链是外部的(内部工具链未就绪, 见 §5 的 `br-wa-toolchain-001`)。需要:
`aarch64-linux-gnu-gcc`(或带版本号的 `gcc-16`/`gcc-15`/`gcc-14`/`gcc-13`; **候选序与探测在
`brickie-core`**, 不在 Makefile)、`binutils-aarch64-linux-gnu`、`qemu-system-aarch64`。

**目标表**(`make <目标>`; 镜像侧的每个名字都是**薄委派**, 真身在右侧):

| 目标 | 做什么 | 真身(委派到) |
|---|---|---|
| `make`(缺省) | **只编工具**(不碰交叉工具链) | `make -C tools/brickie cxx` |
| `make all` | 编工具 → 编镜像 + `[build].post` 门禁 | `brickie build` |
| `make tools` / `tools-core` / `tools-test` / `tools-clean` | 工具段(L5/L2 / Rust 核心 / 用例 / 清理) | `tools/brickie/Makefile` |
| `make brickie-check` / `brickie-check-release` / `brickie-compose` | 组合期校验(dev/release)/ 重建生成物 | `brickie check` / `check --profile release` / `gen` |
| `make run` | QEMU 上跑(Ctrl-A X 退出) | `brickie run` |
| `make smoke` / `irq-test` / `dbg-test` | QEMU 门禁(冒烟 / 中断逐用例 / 内存+调试) | `brickie test <名>` |
| `make mem-test` / `string-test` | **宿主侧**用例(不需交叉/QEMU) | `brickie test <名> --no-build` |
| `make check-string` / `check-headers` | 支持例程自递归 / 对外头文件自洽 | `brickie test check-string` / `test check-headers --no-build` |
| `make size` / `disasm` | 体积 / 反汇编 | `brickie size` / `brickie disasm` |
| `make clean-brickos` / `clean` | 清镜像派生物 / 连工具一起清 | `brickie clean` / `+ tools-clean` |
| `make check-workarounds` / `check-build` | WORKAROUND 登记 / 构建接线门禁 | `tools/check-*.sh` |
| `make tools-prebuilt` / `tools-prebuilt-check` | 发布 / 检查三件自举种子 | `tools/brickie` Makefile |
| `make print-host-triple` / `print-host-bin-dir` / `print-prebuilt-bin-dir` / `print-cross-compile` / `env` | 查询口(宿主三元组 / 落点 / 种子 / 交叉前缀 / 环境片段) | `mk/host.mk` / `setup.sh` |

```bash
# 在仓库根执行(本分支根目录 = 原型树, 没有 brickOS/ 前缀)
make                    # 只编工具(缺省目标)
make all                # 编工具 → brickie build(镜像 + 构建后门禁)
# 也可以直调工具(推荐; 这就是"真身")
build/host/<triple>/bin/brickie build [--backend make|ninja] [--dry-run] [-j N]
build/host/<triple>/bin/brickie test [<门禁名>|--list]
build/host/<triple>/bin/brickie check --profile release
make check-workarounds  # WORKAROUND 登记一致性
make check-build        # 构建接线门禁: 缺省目标=工具/无字面源码/委派/零交叉依赖/出树/种子
make tools-prebuilt     # 发布三件自举种子到 prebuilts/seed/brickie/<arch>/<os>/bin/
make clean              # 清掉工具与镜像两侧的派生物(只清工具: make tools-clean)
```

**三条与内存/中断相关的构建/运行纪律**(都不是"可选优化"; 现在都写在**声明面**上):

| 项(声明位置) | 值 | 为什么 |
|---|---|---|
| `[build.target].arch_flags`(platform 插件) | `-mstrict-align` 必须带 | 它**曾经是硬要求**: MMU 未开 ⇒ 全部访存按 Device-nGnRnE ⇒ 非对齐访问必取 Alignment fault(实测: `stur xzr,[sp,#36]` 给 12 字节局部结构清零 ⇒ 开机 data abort)。**现在 MMU 已开**(4 KiB 恒等映射, RAM 是 Normal 属性、`SCTLR.SA/SA0` 显式清零), 非对齐访问不再 fault ⇒ 本项退化为**防御性旋钮**(Device 区的非对齐访问仍会 fault, 留着它把"编译器的内存模型"与"MMIO 的真实约束"钉在一起) |
| `product.toml [build].cflags` | `-fno-omit-frame-pointer` 必须 | **`service/backtrace` 的编译期前提**: 栈回溯靠 `x29` 帧链(设计 `5-01 §3` 的"各线程栈"捕获), `-O2` 默认把 fp 当普通寄存器省掉 ⇒ 链断在第一帧。代价是每函数多一对 `stp/ldp` |
| `[build.target.qemu].machine`(platform 插件) | `virt,gic-version=3` 必须显式钉住 | QEMU virt 的**缺省是 GICv2**(`-M virt,dumpdtb` 的 compatible = `arm,cortex-a15-gic`)。镜像里是 GICv3 驱动, 配错型号的症状是 `mrs icc_sre_el1` 未定义指令 ⇒ panic, 或"PIC 初始化完毕却收不到中断" |

> ⚠ **工具接管了镜像的"声明面"与"编译编排", 但还没接管"调用点"**: `brickie build`
> 现在真的按声明面组合镜像(哪些源、用什么标志、链哪个脚本都由 `product.toml` /
> 插件 `[build]` / `[build.target]` 决定)。但**启动链本身**仍是压缩的替身 ——
> `.br_plugins` 段枚举驱动的调用点(插件管理器)属 M0 运行期, 仍未落地。这正是
> `br-wa-entry-001` 剩下的那条欠债(直编那半条已注销), 见 §5 与 `WORKAROUNDS.md`。
>
> ⚠ 声明面不自洽时 `brickie build` 会**在组合期止步**(红 / 退出码非 0), 镜像不编。
> 这是**有意**的: 声明面自洽是镜像构建的前置。只编工具用 `make tools` / `make tools-core`;
> 只想看结论用 `make brickie-check`。

换交叉工具链前缀改**声明面**(这是 `br-wa-toolchain-001` 的还债口), 构建规则不动:

```bash
# platform/qemu-aarch64/plugin.toml 的 [build.target].cross
# 候选序与解析在 brickie-core(contract §9 R-14); 查询口仍可用:
make print-cross-compile
```

实际输出(QEMU virt, `-cpu cortex-a53`, `gic-version=3`; 逐用例的 67/35/31 行已省略):

```
[    0.000050] INFO  brickOS-prototype v0.1.0 -- core MainLoop (interrupt heartbeat + delay + logging)
[    0.000684] INFO  platform: qemu-aarch64/virt (aarch64)
[    0.002937] INFO  mem: heap=1048576 contig=262144 page=1048576(256 pages) dma=262144
[    0.003849] INFO  mem: identity map on (SCTLR=0x30d51825, regions=9, heap=1024 KiB, page pool=256 pages)
[    0.004479] INFO  mem: 4 KiB pages=2048, 2 MiB device blocks=512, TTBR0=0x400a3000 TCR=0x200803d19 MAIR=0x4400ff
[    0.001157] INFO  entry chain: start.S -> br_irq_cpu_init -> br_plat_early_init -> br_core_main
...                                                                    ← 67 项 PASS(见 make irq-test)
[    0.031887] INFO  [IRQCONF] SUMMARY pass=67 fail=0 total=67
[    0.032564] INFO  int: conformance ALL PASS (failures=0)
[    0.032862] INFO  int: timer PPI armed by platform (virq=0 INTID=30, 100 ms)
...                                                                    ← 35 项 PASS(见 make dbg-test)
[    0.054880] INFO  [MEMCONF] SUMMARY pass=35 fail=0 total=35
[    0.055056] INFO  mem: conformance ALL PASS (failures=0)
[    0.113357] INFO  [TRACE] total=461 drained=264 overrun=8 ids=3 live=197
[    0.118435] INFO  [LEAK] summary live=1 bytes=123 owners=1 corrupt=0 high_water=18352
[    0.122500] INFO  [DBGCONF] SUMMARY memleak pass=12 fail=0 total=12
[    0.127891] INFO  [DUMP] ===== snapshot start =====
[    0.130603] INFO  [DUMP] regions=9
[    0.132661] INFO  [LEAK] summary live=0 bytes=0 owners=0 corrupt=0 high_water=18352
[    0.133731] INFO  [BT] frames=4
[    0.133820] INFO  [DUMP] ===== snapshot end =====
[    0.135335] INFO  [DBGCONF] SUMMARY pass=9 fail=0 total=9
[    0.135498] INFO  dbg: conformance ALL PASS (failures=0)
[    0.141333] INFO  dbg: boot snapshot lines=24
[    1.033218] INFO  tick=1 uptime=1034999 us delay=1000054 us (>=1000000 us: ok) irq_ticks=9
[    2.033521] INFO  tick=2 uptime=2035324 us delay=1000001 us (>=1000000 us: ok) irq_ticks=19
[    2.149967] INFO  dbg: steady-state trace drained=40
```

几处值得看的证据: `identity map on (SCTLR=0x30d51825, ...)` 是 MMU 真的开了(读回 `SCTLR_EL1`);
`[LEAK] summary live=1 bytes=123` 是 memleak 用例**故意泄漏**那一块的现场(它随后自己释放);
`[BT] frames=4` 是栈回溯真的走了 4 帧(符号化归 host 离线工具);
`dbg: steady-state trace drained=40` 是**第 2 拍**才取走的 trace —— 里面是 timer PPI 在中断上下文
落下的事件, 即"trace 在 ISR 里可用"的活证据。

`irq_ticks` 每拍 +10 就是"timer PPI 的 ISR 真的在跑"的活证据(100 ms 心跳);
`make irq-test` 会把 67 项用例逐条判红绿, `make dbg-test` 再判 `[MEMCONF]`/`[DBGCONF]` 两套。

体积(aarch64 裸机 ELF): `.text` ≈ 78 KiB / `.bss` ≈ 59 KiB / `.data` 53 B(`make size`);
其中向量表占 2 KiB(每个入口 0x80 字节是 AArch64 的硬性间距)、**页表 28 KiB**(7 张 4 KiB 表:
L1 + Device L2 + RAM L2 + 4 张 L3, 静态放 .bss 因为"页池可用"以"映射已生效"为前提)。
静态 RAM 的其余大头: 中断描述符池/运行期表/域池 ≈ 8.3 KiB、trace 环 256×16 B = 4 KiB、
TLSF 堆与 contig/页池的控制块(池本体在 region 表划的 RAM 里, 不占 .bss)。

两个值得知道的实现取舍:

- **时钟换算不是 `ticks / (freq/1e6)`**: QEMU 的 arch timer 是 62.5 MHz, 每微秒 62.5 拍不是整数,
  取整会带来约 1% 的**系统性漂移**(跑 1000 秒差 8 秒)。改用**毫秒**为基准(`freq/1000 = 62500`,
  对 62.5 MHz 精确), 再把 `ticks*1000` 拆成整数+余数两步, 既无漂移也不溢出。
  延时换算**向上取整** —— 这是"不早醒"的落点。
- **向量表 16 个入口全走"喊一声 + 停机"**: v0.1.0 不开中断, 但 fault/SError 仍会来;
  没有向量表就会跳到地址 0 静默挂死。已实测: 注入 `brk #0` 会打出
  `[FATAL] unhandled exception -- vector=4 -- parked`(vector 4 = Current EL with SP_ELx, Synchronous)。
  这条路径**故意不打日志**(不走可能已损坏的 core 设施), 对应设计 `3-02 §8.3.1` 的 bare 路径。

## 5. WORKAROUND(欠债清单)

v0.1.0 有**七条**欠债, 全部登记在 **[WORKAROUNDS.md](WORKAROUNDS.md)**:

| id | 一句话 |
|---|---|
| `br-wa-entry-001` | **Platform 的插件化只完成了一半**: `plugin.toml` + 描述符已就位, 源集合也已改由插件的 `[build].sources` 声明(`brickie build` 消费), 但启动链的**调用点**仍是 APP 直调, 不是 `.br_plugins` 段枚举驱动 |
| `br-wa-boot-001` | 启动链仍被压缩成一个死循环(无 plugin_manager / 阶段机 / 调度器); 中断框架挂接的三处(core.init 的 TPIDR_EL1、"全部 init 之后开中断"、timer PPI 的 ISR 注册)都靠替身; **新增**: 五个调试服务插件的 init 与调用也靠 APP 直调(`allow_edges` 里的两条 M0 引导例外) |
| `br-wa-isa-001` | **ISA 共享库这一层还没有独立存在**: GICv3 方言、异常向量桩、**4 KiB 页表构造(`mmu.c`)** 暂居 platform 插件目录(靠文件边界分层); 异常帧布局因 extable fixup 暂放 core |
| `br-wa-toolchain-001` | 工具链用外部 gcc; 目标事实写在声明面(`product.toml` + platform 的 `[build.target]`), **工具候选序与解析在 `brickie-core`**(裁定 R-14) |
| `br-wa-mem-001` | **三池比例写死在 platform 的 region 表里**(heap 1 MiB / contig 256 KiB / page 1 MiB / DMA 256 KiB / 保留 16 KiB), 未经 manifest 的 `[budget]`/`[[res]]` 生成 —— 编译编排**已落地**(`brickie build`), 仍欠的是 `budget → region` 的**生成链路** |
| `br-wa-debug-001` | **memleak 的"归属标签" ≠ v2 的 per-plugin arena 记账**: 能报"谁没还、在哪分配的", 没有预算上限/强制归属/OOM 策略 |
| `br-wa-debug-002` | **dump/trace 直写早期 console**, 未经 `5-01 §2` 的 debug bridge(COBS + CRC16 成帧 + `MEMRD`/`TRACE_READ` 命令面, M3) |

**`br-wa-entry-001` 的退出条件**(`brickie`, 原名 `br`, 就绪后必须做的三件事):

1. ~~`br` 能按布局约定发现并校验插件(`4-02`)~~ ⇒ **已还**(`brickie check` / `gen`, 见
   [docs/decisions/0001-platform-plugin-manifest.md](docs/decisions/0001-platform-plugin-manifest.md));
2. ~~`platform/` 收敛为插件 `platform/qemu-aarch64`(`BR_PLUGIN` 描述符 + 声明片段, `4-03`)~~
   ⇒ **已还**(`platform/qemu-aarch64/plugin.toml` + 生成物
   `build/gen/platform/qemu-aarch64/plugin_desc.c`);
3. ~~`start.S` 的调用点从"Makefile 直编"改为声明面驱动~~ ⇒ **直编那半条已注销**
   (源集合改由 `platform/qemu-aarch64/plugin.toml` 的 `[build].sources` 声明、
   `brickie build` 消费 —— ADR-0003 的 S1/S4); **仍欠**: 启动链的调用点从"APP 直调"
   改为 `.br_plugins` 段枚举驱动(`3-05 §2.2` 的 `__br_plugins_start/__br_plugins_stop`)
   —— 依赖插件管理器(M0 运行期)。

代码里的标记形如 `WORKAROUND(br-wa-boot-001)`, 与登记表由 `make check-workarounds` 绑死:
**任一侧多/少即报红** —— 欠债最怕的不是欠着, 是没人知道欠着。

## 6. 代码 ↔ 设计对应

| 本原型 | 设计出处(`brickOS-Design` 分支) | 形态差异 |
|---|---|---|
| `start.S` 的 reset/BSS | `1-01 §9` 启动序列 | 设计是 Platform **插件**的汇编; 此处随插件 `[build].sources` 编进镜像, 调用点仍是 APP 直调(`br-wa-entry-001`) |
| `br_plat_early_init()` | `1-01 §9` 的 `platform.early_init`; `1-01 §8` 三层模式 | console + GICv3 PIC 注册 + 绑定表(§14.3 步 1–3)+ **region 表声明 + `br_mem_init()` 三池 + 4 KiB 恒等映射页表/开 MMU** |
| `br_console_*` | `1-01 §8` console 双形态; `3-01 §10` 平台侧接口表 | 形态一致(轮询早期 console) |
| `br_clock_now()` / `br_time_t` | `3-01 §4`(br-sched 组); `3-01 §14` CA-1(us) | 只实现读数; 超时表/唤醒属 M1 |
| `br_log_*` | `5-01`(trace 观测)/ `11-01`(日志 Service) | v0.1.0 是 core 内的最小打印设施, 不是那个服务 |
| `br_core_main()` | `1-01 §9` + `§6.2` 阶段表 | 顶替整条 core.init → plugin_manager → EARLY/CORE/LATE → `br_sched_run()`(`br-wa-boot-001`) |
| **`br_irq_*` 九件 + 域四件** | `3-01 §8/§8.1`(签名冻结)+ `3-02 §3–§9`(机制) | Stage 1 全量; `register/enable/disable` 的 thread-only 加了**运行期拒绝**(设计侧靠静态扫描) |
| **`br_pic_register` / `br_irq_bindings_set` / `br_pic_ops_t`** | `3-02 §4.1/§14.3`(platform 侧契约) | 新增的核心符号, 未回灌 `3-01 §10` 的登记(属 Design 仓库, 见 ADR-0002 §4) |
| **GICv3 方言(`br_gicv3_*`)** | `3-01 §8` 表"中断控制器 → GICv3 驱动"; `3-02 §4.1.1/§5.3/§7.1` | ISA 层代码暂居 platform 目录(`br-wa-isa-001`); 1020–1023 折算 / EOImode=0 / MSB 对齐量化 |
| **异常向量表 + 帧** | `3-02 §5.2/§10.2` | 每槽 0x80 B 放不下完整桩 ⇒ 槽内跳板 + 槽外桩体(`P-IRQ-ASM-1`); 帧布局放 core(`br-wa-isa-001`) |
| **fault 分类 + extable + panic** | `3-02 §10.3/§10.4/§10.6`; `§8.3.1` 的 bare 路径 | 分类/extable/double-fault 兜底齐; `br_fault_handler_register` 属 v2(签名在 `3-02 §14.5`) |
| **trace 环** | `5-01 §1`(16 B 定长事件, ISR 内可记) | 环与 ISR 发射点在 core(CA-3 白名单要求中断路径不依赖服务 init), `service/trace` 是它的**唯一消费方**(drain/解码/计数/呈现) |
| **`br_malloc` 族 / contig / page / dma / heap_usage** | `3-01 §6`(br-mem 11 函数, 签名逐字一致)+ `CA-6/CA-7/CA-8` | 三池由 platform 的 region 表按 `BR_MM_KIND_*` 划; TLSF + 红区/毒化是 `5-01 §4` 的 v1.x 便宜层; 归属标签只是 v2 arena 的近似(`br-wa-debug-001`) |
| **`br_mm_region_add` / `br_mm_map` / `br_mm_unmap` / `br_mm_cache_*`** | `3-01 §7`(br-mm 5 函数) | `map`/`unmap` 按 `TC-MM-003` 的期望返回 `-ENOTSUP`(v2 重定位); 运行期改属性走 `br_mm_set_attrs`(用例做真 RO 保护验证); region 种类位是原型新增载体 |
| **`br_mm_ops_t` / `br_mm_register` / `br_mm_activate`** | `3-01 §10`"平台侧接口"行(同 `br_pic` ops 手法)+ `1-01 §8` 三层模式 | 状态机(未注册→已注册→已激活)在 core, 页表构造在 platform; 描述符编码属 ISA 层 ⇒ `br-wa-isa-001` 范围扩大 |
| **`service/{trace,backtrace,hexdump,memleak,dump}`** | `1-03 §1`(v1.0 插件清单: `service/trace`)+ `5-01`(§1 trace / §2 bridge / §3 ramdump 捕获集 / §4 memleak) | `service/trace` 与清单同名; 另四件是本次补的调试服务(6-01 尚无 `TC-DBG-*` 组, 属回灌项); 调用点靠 `product.toml` 的两条 M0 引导例外(`br-wa-boot-001`) |
| **`tests/host/mem_test.c` + `make mem-test`** | `1-03` 的"host 平台插件: CI 秒级 + ASan 白捡" | 完整 host 平台插件未落地; 本原型先做到"内存实现编成宿主可执行跑算法压测"(无需交叉工具链/QEMU) |
| **`board_irq.h`** | `3-02 §3.1`(IR-2 生成头) | 走设计明示的**退路**: platform 导出静态头(brickie v0.1 不生成该头, 属 2-01 的 O-4) |

## 7. 下一步(往 M0 走)

按设计 `1-03 §3`, M0 = **启动链 + 插件管理**, 验收 = "hello + init 链打印 + 故意造环看组合器报错"。
本原型离它还差以下几步, 顺序有依赖关系(状态按当前 checkout):

1. **组合器 `brickie` 的声明面** —— **已交付**(三语言: L5 Python 前端 23 条叶子命令、
   L0/L1 Rust 核心、L2 C++ 渲染器, 入口 = 单文件自包含 ELF)。`platform/qemu-aarch64` 与
   `app/hello` 已被它接管(manifest + `check` + 快照 + lock + 描述符生成物)。这一段是
   `br-wa-entry-001` 第 ③ 条的前提, 前提已具备。
2. 插件描述符段(`.br_plugins` + `__br_plugins_start/stop`)与**声明面唯一真值**(`2-02` BR-D2)。
   `platform/qemu-aarch64` 与 `app/hello` 的 manifest / 描述符生成物已在 `build/gen/**`,
   缺的是**运行期按段枚举**。
3. `start.S` 的调用点改为段枚举驱动 → 兑掉 `br-wa-entry-001` 的第 ③ 条
   (第 ①② 条已随插件化还清)。
4. ~~中断框架(M0/M1 的 Stage 1)~~ ⇒ **已交付**(设计 `3-02` 的 Stage 1 全量: 号空间/PIC 抽象/
   生命周期/屏蔽三层/优先级/触发/FAST 级联域/fault+extable, 加 GICv3 方言与向量桩;
   `make irq-test` 的 67 项在 QEMU 上全绿)。**仍欠的**是它的 Stage 2(要 bh 与调度器)
   与 `br_fault_handler_register`(v2)。
5. ~~内存: 恒等映射 + 三池 + 堆~~ ⇒ **已交付**(设计 `3-04`/`3-01 §6/§7`: 4 KiB 恒等映射页表 +
   region 表 + TLSF 堆 + contig/DMA 池 + 页位图池 + cache 维护; `make dbg-test` 的
   `[MEMCONF]` 与 `make mem-test` 在目标/宿主两侧全绿)。**仍欠的**是重定位(v2)、
   MPU 目标(vx)、池比例由 manifest 生成(`br-wa-mem-001`)。
6. ~~调试插件: trace / dump / hexdump / backtrace / memleak~~ ⇒ **已交付**(五个 service 插件,
   声明面由 brickie 治理; `make dbg-test` 的 `[DBGCONF]` 全绿)。**仍欠的**是 debug bridge(M3)、
   per-plugin arena 记账(v2)、栈 canary 与目标侧 ASan。
7. 调度器(M1) → 把 MainLoop 拆成 `app.start()` 的 APP 线程 + `br_sched_run()`;
   顺带把 timer PPI 的 ISR 与"全局开中断"从 platform 的替身挪回 `core.init`,
   并把五个调试插件的 init 从"APP 直调 + allow_edges"改由插件管理器按 `[[dep]]` 驱动。

⇒ 于是剩下的主线其实是**同一条**: **插件管理器 / 阶段机 / 调度器**(它们互为前提)。
在它们到位之前, `br_core_main` 会一直在那里 —— 但它的归宿是**被拆掉, 不是长大**
(见 `main.c` 顶部注释); 中断框架这一半已经可以先独立验收了。

### 中断侧的实现裁定(与设计文档的偏差都记在这里, 细节见 ADR-0002)

| # | 裁定 | 一句话 |
|---|---|---|
| `P-IRQ-1` | 绑定表只覆盖直连线 | 域子中断的描述符由 `domain_create` 的窗口切片填充 |
| `P-IRQ-2` | extable 线性扫描 | 段顺序 = 链接顺序, 没有无运行期初始化的排序点 |
| `P-IRQ-3` | core 的量化判据假定 MSB 对齐 | LSB 对齐的方言(NVIC)需方言侧自行处理 |
| `P-IRQ-5` | `br_irq_cpu_init()` 兼初始化描述符池的 `pic_id/dom_id = -1` | 0 是合法编码(BSS 零不等于"无效") |
| `P-IRQ-15` | GICv3 拒绝"量化后落在最低可实现档"的请求(`-ENOTSUP`) | 那档与 PMR 相等 ⇒ 永远收不到; 降到上一档是**提权**, IR-7 禁止 |
| `P-IRQ-16` | 一致性用例用**非对齐访存**造可恢复 fault | ~~MMU-off ⇒ Device 内存 ⇒ 非对齐必取 Alignment fault~~ **已被 `P-MM-3` 取代**: MMU 开 + Normal 属性 + `SA/SA0=0` 之后非对齐不再 fault, 改用**窗口外未映射地址**取翻译 fault |
| `P-IRQ-17` | timer PPI 的 ISR 与"全局开中断"归 **platform**, 不归 APP | 设计 `3-01 §13.6` 把"中断控制"归 **P3**、APP 是 **P0** ⇒ 放置避免越权 |
| `P-IRQ-ASM-1/2` | 槽内跳板 + 槽外桩体; 恢复路径不写 `msr daif`(无此编码, `eret` 从 SPSR 恢复) | 一槽 0x80 B 装不下 320 B 帧的存取 |
| `P-IRQ-PLAT-1` | `ICC_CTLR_EL1.PRIbits` 按 `field + 1` 解码并夹到 [5,8] | 实测 QEMU: field=4 ⇒ 5 位; 回读断言(0x01→0x08)是它的执法 |

### 内存/调试侧的实现裁定(与设计文档的偏差都记在这里, 细节见 ADR-0003)

| # | 裁定 | 一句话 |
|---|---|---|
| `P-MEM-1` | **native 面零增量**: br-mem 11 + br-mm 5 与 `3-01 §6/§7` 逐字一致 | 新增的初始化/观测/记账/平台面共 16 个符号**分类登记**在 ADR-0003 §2.1, 不动 CA-5 的 48 预算 |
| `P-MEM-2` | TLSF 二级位图(32 × 32), `BR_MALLOC_ALIGN = 16` | AAPCS64 的 SP 16 对齐 ⇒ 结构体自然对齐上界是 16; 取 4/8 会让 `aligned(16)` 对象放不进 malloc 的 buffer |
| `P-MEM-3` | 用户块 = `[头 32B][用户区][红区 0xA5][尾 8B]`; 双 free 检出后**直接返回** | `5-01 §4` 的 "TLSF 红区 + freelist 毒化" 落地; 二次入链会毁堆, 拦截比报错重要 |
| `P-MEM-4` | region 种类位 `attrs[11:8]`(`BR_MM_KIND_*`)= `3-01 §13.6` regions 轴的**可执行载体** | 设计只给了取值域没给载体; core 按种类认领池(机制), platform 只声明数据 |
| `P-MEM-5` | 镜像 + 启动栈声明为**一条** `IMAGE` region | region 重叠禁则 + 页对齐: 镜像尾与栈底只差十几字节, 拆两条必然重叠 |
| `P-MEM-6` | 页池位图按 `BR_PAGE_MAX_PAGES = 4096` 静态预置(**16 MiB** 池上界, 位图 512 B) | 无运行期分配; 超出上界在 `br_mem_init` 就 `-ENOMEM`, 不静默截断 |
| `P-MM-1` | 4 KiB 粒度 / 4 级 / 仅 TTBR0 / T0SZ=25 / IPS=40 位; 7 张静态页表(28 KiB) | 逐页 4 KiB 映射是 `set_attrs`(RO/NX)与 v2 重定位能按页生效的前提 |
| `P-MM-2` | 低 1 GiB 整段 512 条 2 MiB **Device 块** | QEMU virt 的外设都在低 1 GiB, 且 GICR 的第 4–8 帧排到 `0x081A0000` —— 只映射 GICD 附近会漏 |
| `P-MM-3` | RAM 开 8 MiB 的 4 KiB 页窗; **窗口外 invalid** ⇒ 可恢复翻译 fault | 取代 `P-IRQ-16` 的"非对齐 fault"锚点(SA/SA0 已清零, Normal 上的非对齐不再 fault) |
| `P-MM-4` | `br_mm_map`/`unmap` = `-ENOTSUP`; 属性改写走 `br_mm_set_attrs` | `TC-MM-003` 的期望就是 `-ENOTSUP`(签名先行); `set_attrs` 是同一条重定位通路在 v1 的演练 |
| `P-MM-5` | `br_mm_query` **只报页表真实属性**, "这块是什么"归 region 表 | 两处不互相冒充; 否则"属性"就有两个真值 |
| `P-MM-6` | NX 只做**描述符读回**验证, 不做真的取指测试 | executing-NX 走 instruction abort, 而 `3-02 §10.3` 的 extable 只覆盖 data abort ⇒ 会 panic; 不假装测过 |
| `P-DBG-1` | trace 的环与 ISR 发射点留 core, 解码/消费在插件 | CA-3 白名单要求中断/fault 路径不依赖服务 init; "可整层移除"由消费侧满足 |
| `P-DBG-2` | backtrace **只捕获不符号化**(host 离线解码), `-fno-omit-frame-pointer` 是前提 | `5-01 §3` 把栈回溯分析划给 host; 镜像内符号表要两遍链接, 归 v1.x |
| `P-DBG-3` | hexdump 格式是契约(87 B/行, 自己渲染不依赖 `br_log`) | 否则"格式契约"会随 log 实现漂移; 用例逐字节断言 |
| `P-DBG-4` | dump 的边界判定 = region 表; `BR_DUMP_F_FORCE` 是 P4/machine 语义 | 未声明地址默认 `-EINVAL`; 同时暴露分级模型缺口: ops 轴没有"读/inspect" ⇒ dump 只能声明 P0(回灌项) |
| `P-DBG-5` | 调试域用例编号 `TC-DBG-*`(6-01 无此组) | 与 `TC-IRQ-1xx` 同型: 域特有组用百位编号; 回灌 `6-01` 是留待项 |
