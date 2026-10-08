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
                                    早期 console + GICv3 PIC + IRQ 绑定表       ├─ 中断一致性用例(TC-IRQ-*, 67 项)
                                                                              ├─ 中断心跳(timer PPI INTID 30)
                                                                              └─ MainLoop(延时 + 日志)
```

| 环节 | 内容 | 代码 |
|---|---|---|
| **Platform Entry**(`platform/qemu-aarch64` 插件) | reset、选核、EL2→EL1 降级、栈、**异常向量表**、BSS 清零 | `platform/qemu-aarch64/src/start.S` / `vectors.S` |
| | 早期 console(PL011 轮询)、arch timer 读数 | `platform/qemu-aarch64/src/console_pl011.c` / `timer_arch.c` |
| | **GICv3 驱动**(`br_pic_ops` 方言) + 板级 IRQ 绑定表 + 中断初始化/开跑 | `platform/qemu-aarch64/src/gicv3.c` / `board_irq.c` |
| | **中断一致性用例**(target-only, 编进镜像) | `platform/qemu-aarch64/src/irq_conf.c` |
| | 链接脚本(含 `.br_extable` 收集) | `platform/qemu-aarch64/src/link.ld` |
| **APP**(`app/hello` 插件) | MainLoop: 每秒打一行日志, 延时自带"不早醒"判据; 只**读**平台的心跳计数(纯 P0 消费者) | `app/hello/src/main.c` |
| **Core**(内核本体, 不是插件) | **中断框架 Stage 1**: 号空间/描述符池、生命周期(ack→ISR→eoi 单出口)、三层屏蔽、优先级语义、级联域(FAST)、fault/extable、最小 trace 环 | `core/src/irq/*.c`, `core/src/{panic,trace}.c` |
| | 时钟换算(us)+ 忙等延时 | `core/src/time.c` |
| | 日志(格式化 + 等级过滤), 不走 libc printf | `core/src/log.c` |

**验证目标**(不是"能编译"): 镜像能在 QEMU virt 上从 reset 跑到 MainLoop, 日志时间戳单调,
且每次延时**实际不短于请求值**(设计 `3-01 §2.1` 的"不早醒"语义)。后一条是**自动化判据**,
不是人眼看着差不多 —— `make smoke` 会 grep 它。

中断侧同样有**自动化判据**, 而且是**真跑硬件路径**(不是静态检查):

- `make irq-test` —— 67 项逐用例判 PASS/FAIL(设计 `6-01 §3.7` 的 `TC-IRQ-*` + GICv3 方言事实);
- arch timer 的 PPI(INTID 30)在跑, 日志的 `irq_ticks` 每秒 +10 —— 真实中断投递的活证据。

## 2. v0.1.0 刻意不做什么

原型最容易失控的地方是"顺手多做一点"。以下是**明确不做**的, 连同理由:

| 不做 | 理由 |
|---|---|
| **插件化(描述符 / manifest / 组合器)** | 这正是组合器 brickie 与插件管理器的产出(`4-02`/`4-03`, `3-05`)。**声明期**已由 brickie v0.1 接管(插件发现 / 校验 / 描述符生成 / 接口发布 / 版本治理; 见 [`tools/brickie/README.md`](tools/brickie/README.md)), 但**运行期**的插件管理器与描述符段(`.br_plugins`)仍未落地 ⇒ `platform/qemu-aarch64` 与 `app/hello` 虽已是插件, 调用点仍由 Makefile 直接编进镜像。**Platform Entry 因此仍挂 WORKAROUND, 见 §5** |
| 调度器 / 线程 / `br_sched_ops` | M1。没有调度器就没有"可让出的对象", 所以睡眠只能是忙等 |
| **中断的 Stage 2**(分发到 bh/线程、亲和性/均衡/IPI、`CAP_NEST` 嵌套、PM save/restore) | 设计 `3-02 §1.1` 的分期: Stage 1(无调度器世界)已落地, Stage 2 的前提是**调度器与 bh** —— 它们是 M1 |
| `br_fault_handler_register`(fault handler 链) | 设计 `3-02 §10.5`: 分槽/分类/extable/panic 属 Stage 1(已做), **注册 API 归 v2** |
| **SLOW 级联域**(状态需总线事务的 PMIC 型) | 设计 `3-02 §1.1.1`: 它的存在前提就是 bh; v0.1 明确**拒绝**(`br_irq_domain_create` 返回 NULL + 留痕), 不是假装支持 |
| MMU / 恒等映射 / region 表 / cache 维护 | MainLoop 不碰内存管理, 做了也无法验证 |
| 堆 / 内存分配 | 同上; 本原型**零动态分配** |
| 设备注册 / devfs / cdev | M2。早期 console 是**轮询**形态 —— 这是设计内的 M0 形态(`1-01 §8` console 双形态), **不是** workaround |
| libc / svc-posix | `-nostdlib -ffreestanding`; 只有 `stdarg.h`(编译器自带) |

## 3. 目录结构

```
brickOS/
├── Makefile                     构建 / 运行 / 冒烟 / 门禁
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
│   │   ├── br_main.h            core 入口契约(实现随 APP 走)
│   │   ├── br_irq.h             【中断】native API(9 件)+ 域 API + 绑定表 + 状态字
│   │   ├── br_pic.h             【中断】PIC 填表契约(core ↔ ISA 方言, 非 native 面)
│   │   ├── br_exc.h             异常帧布局(asm 桩 ↔ fault 路径的同一处真值)
│   │   ├── br_fault.h           fault 分类 / extable 宏 / panic 两入口
│   │   └── br_trace.h           最小 trace 环(ISR-safe 留痕)
│   └── src/
│       ├── log.c                格式化 + 等级过滤
│       ├── time.c               时钟换算 + 忙等延时
│       ├── panic.c              br_panic_bare / br_panic(无锁/无堆/无调度器)
│       ├── trace.c              16 B 定长事件的环形缓冲
│       └── irq/                 【中断框架 Stage 1】
│           ├── irq_internal.h   描述符/运行期表/CPU-local/域 的结构(32 B / 8 B 静态断言)
│           ├── irq_pic.c        PIC 注册表 + 绑定表 + hwirq→virq(有序表 + 二分)
│           ├── irq_core.c       描述符池 / lock-unlock / register-enable-disable / 风暴 / 入口
│           ├── irq_domain.c     级联域池 + 窗口切片 + FAST demux + 逐子 ack
│           └── irq_fault.c      fault 入口 + extable 查找 + double-fault 兜底
└── platform/qemu-aarch64/       【Platform 插件】与平台/ISA 绑定的部分
    ├── plugin.toml              人写 ← 插件级唯一真值(声明两个接口单元: #plat / #gicv3)
    ├── include/br/
    │   ├── board_irq.h          板级 virq 名(设计 3-02 §3.1 的"退路": 手写静态头)
    │   └── platform/
    │       ├── br_plat.h        Platform Entry 契约(含 IRQ 初始化/开跑/一致性入口)
    │       └── br_gicv3.h       GICv3 方言契约(ISA 层)
    ├── src/
    │   ├── start.S              入口: reset / 选核 / BSS / br_irq_cpu_init / 交 core
    │   ├── vectors.S            16 槽异常向量表 + 保存/恢复桩(ISA 层)
    │   ├── link.ld              链接脚本(text/rodata/data + .br_extable 收集 + .stack)
    │   ├── plat_qemu_virt.c     平台身份 + early_init + 异常兜底
    │   ├── console_pl011.c      PL011 轮询 putc
    │   ├── timer_arch.c         CNTFRQ_EL0 / CNTPCT_EL0
    │   ├── gicv3.c              GICv3 方言(实现 br_pic_ops_t)
    │   ├── board_irq.c          绑定表 + 中断初始化 + timer PPI 心跳 + 触发/hwirq 查询
    │   └── irq_conf.c          中断一致性用例(TC-IRQ-*, 编进镜像)
    └── tests/smoke.toml         用例骨架(v0.1 不消费; 已登记 TC-IRQ 梗概)
```

唯一的跨层边是 **APP/Platform → Core**: `start.S` 调 `br_irq_cpu_init()` +
`br_plat_early_init()` 再调 `br_core_main()`; `app/hello` 直调 `platform/qemu-aarch64`
的 `br_plat_name()` / `br_plat_irq_conformance()` / `br_plat_irq_start()`(M0 引导
例外, 在 `product.toml` 里显式豁免)。

**中断的三层归属**(设计 `1-01 §8` / `3-02 §1.3`)在本原型的落点: 机制在 **core**、
GICv3 寄存器序列在 **`gicv3.c` + `vectors.S`**(设计归 ISA 共享库, 原型同目录 + 文件边界,
欠债 `br-wa-isa-001`)、绑定表/基址/静态 prio 在 **platform 数据**。

## 4. 构建与运行

构建是**两段式**, 而且**工具与组合期校验在前**:

```
① tools/    brickie(组合期工具)   宿主 g++ 编 C++ 渲染器 + Python 前端; cargo 编 Rust 核心
                                         │  合并进同一个自包含入口 ELF
                                         ▼
⓪ 声明面    brickie check          用入口 ELF 校验 product.toml / 插件树(dev profile)
                                         │  不碰交叉工具链(v0.1 零编译依赖纪律)
                                         │  (Makefile 里标"⓪", 实际排在①之后 —— 校验要用刚编的入口 ELF)
                                         ▼
② brickOS   aarch64 裸机镜像      外部交叉 gcc
```

顺序由 `Makefile` 里一条 order-only 依赖钉死(`$(OBJS): | tools brickie-check`), 所以
`make -j` 也不会倒过来; 同时"工具/声明面变新"不会触发镜像重链。
这条纪律由 `make check-build` 把关 —— 顺序纪律坏起来通常是**静默**的(比如把 tools 段
写成文件里第一条规则, `make` 就只编工具然后 exit 0, 镜像根本没编却不报错), 所以钉成门禁。

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
make tools-test         # 工具自身用例: 渲染器自检 + 端到端 542 项(含 V-1…V-19 与自包含入口 ELF)
```

**镜像段**的工具链是外部的(内部工具链未就绪, 见 §5 的 `br-wa-toolchain-001`)。需要:
`aarch64-linux-gnu-gcc`(或带版本号的 `gcc-16`/`gcc-15`/`gcc-14`/`gcc-13`, Makefile 会自动探测)、
`binutils-aarch64-linux-gnu`、`qemu-system-aarch64`。

```bash
# 在仓库根执行(本分支根目录 = 原型树, 没有 brickOS/ 前缀)
make                    # ①编工具 → ⓪brickie check(dev) → ②构建 build/brick.elf + .bin
make brickie-check      # 只用 brickie 校验声明面(dev; 不编镜像)
make brickie-check-release  # 发布级门禁(brickie check --profile release)
make brickie-compose    # 用 brickie gen 重建 build/gen/** 生成物(不编译)
make run                # 在 QEMU virt 上跑(Ctrl-A X 退出)
make smoke              # 3 秒冒烟: 自动判定启动/延时/心跳/一致性摘要, 红绿可进 CI
make irq-test           # 中断子系统**逐用例**门禁: 67 项(TC-IRQ-*/GIC-*)必须全 PASS
make size               # 体积
make disasm             # 反汇编
make check-workarounds  # WORKAROUND 登记一致性
make check-build        # 构建接线门禁: 缺省目标/工具在前/工具段零交叉依赖/出树/种子三件
make tools-prebuilt     # 发布三件自举种子到 prebuilts/seed/brickie/<arch>/<os>/bin/
make clean              # 清掉①与②的产物(只清工具: make tools-clean; 不动 prebuilts/)
```

**两条与中断相关的构建/运行纪律**(都不是"可选优化"):

| 项 | 值 | 为什么 |
|---|---|---|
| `ARCHFLAGS += -mstrict-align` | 交叉 gcc 必须带 | **MMU 未开 ⇒ 全部访存按 Device-nGnRnE ⇒ 非对齐访问必取 Alignment fault**。而编译器按 Normal memory 的假设会对 4 字节对齐地址生成 8 字节 `stur`(实测: `stur xzr,[sp,#36]` 给 12 字节局部结构清零 ⇒ 开机 data abort)。开 MMU(恒等映射 + Normal 属性)后才可去掉, 在那之前它把"编译器的内存模型"对齐到真实环境 |
| `QEMUFLAGS = -M virt,gic-version=3` | 必须显式钉住 | QEMU virt 的**缺省是 GICv2**(`-M virt,dumpdtb` 的 compatible = `arm,cortex-a15-gic`)。镜像里是 GICv3 驱动, 配错型号的症状是 `mrs icc_sre_el1` 未定义指令 ⇒ panic, 或"PIC 初始化完毕却收不到中断" |

> ⚠ **工具只接管了镜像的"声明面", 还没接管"调用点"**: `make` 现在只多做一件事 ——
> 编译镜像前先跑 `brickie check`(dev)校验声明面; 描述符生成要显式 `make brickie-compose`。
> 镜像本身仍由 Makefile **手工组合** —— `.br_plugins` 段枚举驱动的启动链(插件管理器)
> 属 M0 运行期, 仍未落地。这正是 `br-wa-entry-001` 剩下的第 ③ 条欠债, 见 §5 与
> `WORKAROUNDS.md`。
>
> ⚠ 由于这条 order-only 依赖, **声明面校验不过时缺省 `make` 会止步在 ⓪**(组合期
> 校验红 ⇒ 退出码 1, 镜像不编)。这是**有意**的: 声明面自洽是镜像构建的前置。只编工具用
> `make tools` / `make tools-core`; 只想看结论用 `make brickie-check`。

换工具链前缀只需一个变量(这是 `br-wa-toolchain-001` 的还债口):

```bash
make CROSS_COMPILE=aarch64-none-elf-
```

实际输出(QEMU virt, `-cpu cortex-a53`, `gic-version=3`; 一致性用例的 67 行已省略):

```
[    0.000050] INFO  brickOS-prototype v0.1.0 -- core MainLoop (interrupt heartbeat + delay + logging)
[    0.000684] INFO  platform: qemu-aarch64/virt (aarch64)
[    0.000887] INFO  clock: 62500000 Hz (arch timer), 62500 ticks/ms (exact integer conversion)
[    0.001157] INFO  entry chain: start.S -> br_irq_cpu_init -> br_plat_early_init -> br_core_main
[    0.001395] INFO  [IRQCONF] int framework conformance (Stage 1, GICv3, no scheduler)
...                                                                    ← 67 项 PASS(见 make irq-test)
[    0.032564] INFO  int: conformance ALL PASS (failures=0)
[    0.032862] INFO  int: timer PPI armed by platform (virq=0 INTID=30, 100 ms)
[    1.033218] INFO  tick=1 uptime=1034999 us delay=1000054 us (>=1000000 us: ok) irq_ticks=9
[    2.033521] INFO  tick=2 uptime=2035324 us delay=1000001 us (>=1000000 us: ok) irq_ticks=19
```

`irq_ticks` 每拍 +10 就是"timer PPI 的 ISR 真的在跑"的活证据(100 ms 心跳);
`make irq-test` 会把 67 项用例逐条判红绿。

体积(aarch64 裸机 ELF): `.text` ≈ 34 KiB / 其余见 `make size`;
其中向量表占 2 KiB(每个入口 0x80 字节是 AArch64 的硬性间距)。
中断子系统的静态 RAM: 描述符池 64×32 B + 运行期表 64×8 B + 域池 + trace 环 256×16 B
≈ **8.3 KiB**(core 静态, 与 `product.toml` 的 64 KiB 预算同侧)。

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

v0.1.0 有**四条**欠债, 全部登记在 **[WORKAROUNDS.md](WORKAROUNDS.md)**:

| id | 一句话 |
|---|---|
| `br-wa-entry-001` | **Platform 的插件化只完成了一半**: `plugin.toml` + 描述符已就位, 但 `start.S` 的调用点仍由 Makefile 直接编进镜像 |
| `br-wa-boot-001` | 启动链仍被压缩成一个死循环(无 plugin_manager / 阶段机 / 调度器); 中断框架挂接的三处(core.init 的 TPIDR_EL1、"全部 init 之后开中断"、timer PPI 的 ISR 注册)都靠替身 |
| `br-wa-isa-001` | **ISA 共享库这一层还没有独立存在**: GICv3 方言与异常向量桩暂居 platform 插件目录(靠文件边界分层); 异常帧布局因 extable fixup 暂放 core |
| `br-wa-toolchain-001` | 工具链用外部 gcc |

**`br-wa-entry-001` 的退出条件**(`brickie`, 原名 `br`, 就绪后必须做的三件事):

1. ~~`br` 能按布局约定发现并校验插件(`4-02`)~~ ⇒ **已还**(`brickie check` / `gen`, 见
   [docs/decisions/0001-platform-plugin-manifest.md](docs/decisions/0001-platform-plugin-manifest.md));
2. ~~`platform/` 收敛为插件 `platform/qemu-aarch64`(`BR_PLUGIN` 描述符 + 声明片段, `4-03`)~~
   ⇒ **已还**(`platform/qemu-aarch64/plugin.toml` + 生成物
   `build/gen/platform/qemu-aarch64/plugin_desc.c`);
3. **仍欠**: `start.S` 的调用点从"Makefile 直编"改为 `.br_plugins` 段枚举驱动
   (`3-05 §2.2` 的 `__br_plugins_start/__br_plugins_stop`) —— 依赖插件管理器(M0 运行期)。

代码里的标记形如 `WORKAROUND(br-wa-entry-001)`, 与登记表由 `make check-workarounds` 绑死:
**任一侧多/少即报红** —— 欠债最怕的不是欠着, 是没人知道欠着。

## 6. 代码 ↔ 设计对应

| 本原型 | 设计出处(`brickOS-Design` 分支) | 形态差异 |
|---|---|---|
| `start.S` 的 reset/BSS | `1-01 §9` 启动序列 | 设计是 Platform **插件**的汇编; 此处直编(`br-wa-entry-001`) |
| `br_plat_early_init()` | `1-01 §9` 的 `platform.early_init`; `1-01 §8` 三层模式 | console + GICv3 PIC 注册 + 绑定表(§14.3 步 1–3); region/MMU 未做 |
| `br_console_*` | `1-01 §8` console 双形态; `3-01 §10` 平台侧接口表 | 形态一致(轮询早期 console) |
| `br_clock_now()` / `br_time_t` | `3-01 §4`(br-sched 组); `3-01 §14` CA-1(us) | 只实现读数; 超时表/唤醒属 M1 |
| `br_log_*` | `5-01`(trace 观测)/ `11-01`(日志 Service) | v0.1.0 是 core 内的最小打印设施, 不是那个服务 |
| `br_core_main()` | `1-01 §9` + `§6.2` 阶段表 | 顶替整条 core.init → plugin_manager → EARLY/CORE/LATE → `br_sched_run()`(`br-wa-boot-001`) |
| **`br_irq_*` 九件 + 域四件** | `3-01 §8/§8.1`(签名冻结)+ `3-02 §3–§9`(机制) | Stage 1 全量; `register/enable/disable` 的 thread-only 加了**运行期拒绝**(设计侧靠静态扫描) |
| **`br_pic_register` / `br_irq_bindings_set` / `br_pic_ops_t`** | `3-02 §4.1/§14.3`(platform 侧契约) | 新增的核心符号, 未回灌 `3-01 §10` 的登记(属 Design 仓库, 见 ADR-0002 §4) |
| **GICv3 方言(`br_gicv3_*`)** | `3-01 §8` 表"中断控制器 → GICv3 驱动"; `3-02 §4.1.1/§5.3/§7.1` | ISA 层代码暂居 platform 目录(`br-wa-isa-001`); 1020–1023 折算 / EOImode=0 / MSB 对齐量化 |
| **异常向量表 + 帧** | `3-02 §5.2/§10.2` | 每槽 0x80 B 放不下完整桩 ⇒ 槽内跳板 + 槽外桩体(`P-IRQ-ASM-1`); 帧布局放 core(`br-wa-isa-001`) |
| **fault 分类 + extable + panic** | `3-02 §10.3/§10.4/§10.6`; `§8.3.1` 的 bare 路径 | 分类/extable/double-fault 兜底齐; `br_fault_handler_register` 属 v2(签名在 `3-02 §14.5`) |
| **trace 环** | `5-01 §1`(16 B 定长事件, ISR 内可记) | core 内的最小替身(服务注册表未落地): 单核无锁环 + drain/overrun |
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
5. 调度器(M1) → 把 MainLoop 拆成 `app.start()` 的 APP 线程 + `br_sched_run()`;
   顺带把 timer PPI 的 ISR 与"全局开中断"从 platform 的替身挪回 `core.init`。

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
| `P-IRQ-16` | 一致性用例用**非对齐访存**造可恢复 fault | MMU-off ⇒ Device 内存 ⇒ 非对齐必取 Alignment fault; "没映射的地址"在 QEMU 上返回值 0, 不 fault |
| `P-IRQ-17` | timer PPI 的 ISR 与"全局开中断"归 **platform**, 不归 APP | 设计 `3-01 §13.6` 把"中断控制"归 **P3**、APP 是 **P0** ⇒ 放置避免越权 |
| `P-IRQ-ASM-1/2` | 槽内跳板 + 槽外桩体; 恢复路径不写 `msr daif`(无此编码, `eret` 从 SPSR 恢复) | 一槽 0x80 B 装不下 320 B 帧的存取 |
| `P-IRQ-PLAT-1` | `ICC_CTLR_EL1.PRIbits` 按 `field + 1` 解码并夹到 [5,8] | 实测 QEMU: field=4 ⇒ 5 位; 回读断言(0x01→0x08)是它的执法 |
