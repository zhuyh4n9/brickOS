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

**一条链, 两个环节:**

```
(Platform Entry) aarch64 Start.S  ──►  (Core) MainLoop(延时 + 日志)
```

| 环节 | 内容 | 代码 |
|---|---|---|
| **Platform Entry** | reset、选核、EL2→EL1 降级、栈、向量表、BSS 清零 | `platform/src/aarch64/start.S` |
| | 早期 console(PL011 轮询)、arch timer 读数 | `platform/src/aarch64/console_pl011.c` / `timer_arch.c` |
| | 链接脚本 | `platform/src/aarch64/link.ld` |
| **Core** | MainLoop: 每秒打一行日志, 延时自带"不早醒"判据 | `core/src/startup/main.c` |
| | 时钟换算(us)+ 忙等延时 | `core/src/time.c` |
| | 日志(格式化 + 等级过滤), 不走 libc printf | `core/src/log.c` |

**验证目标**(不是"能编译"): 镜像能在 QEMU virt 上从 reset 跑到 MainLoop, 日志时间戳单调,
且每次延时**实际不短于请求值**(设计 `3-01 §2.1` 的"不早醒"语义)。后一条是**自动化判据**,
不是人眼看着差不多 —— `make smoke` 会 grep 它。

## 2. v0.1.0 刻意不做什么

原型最容易失控的地方是"顺手多做一点"。以下是**明确不做**的, 连同理由:

| 不做 | 理由 |
|---|---|
| **插件化(描述符 / manifest / `br`)** | 这正是 `br` 组合器与插件管理器的产出(`4-02`/`4-03`, `3-05`)。工具不在, 插件化只能手工假装。**Platform Entry 因此挂了 WORKAROUND, 见 §5** |
| 调度器 / 线程 / `br_sched_ops` | M1。没有调度器就没有"可让出的对象", 所以睡眠只能是忙等 |
| 中断 / GICv3 / 中断框架 | M0。v0.1.0 全程关中断(DAIF 全屏蔽) |
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
├── tools/check-workarounds.sh   源码标记 ↔ 登记表 一致性检查
├── core/                        【Core】与平台无关的部分
│   ├── include/br/core/
│   │   ├── br_types.h           基础类型(不用 <stdint.h>)
│   │   ├── br_version.h         版本标识
│   │   ├── br_console.h         早期 console 契约(接口在 core, 实现在 platform)
│   │   ├── br_log.h             日志契约
│   │   ├── br_time.h            时钟/延时契约
│   │   └── br_main.h            core 入口契约
│   └── src/
│       ├── startup/main.c       MainLoop(延时 + 日志)
│       ├── log.c                格式化 + 等级过滤
│       └── time.c               时钟换算 + 忙等延时
└── platform/                    【Platform Entry】与平台/ISA 绑定的部分
    ├── include/br/platform/
    │   └── br_plat.h            Platform Entry 契约(含 WORKAROUND 声明)
    └── src/aarch64/
        ├── start.S              入口: reset / 选核 / 向量表 / BSS / 交 core
        ├── link.ld              链接脚本(显式 text/rodata/data 三段权限)
        ├── plat_qemu_virt.c     平台身份 + early_init + 异常兜底
        ├── console_pl011.c      PL011 轮询 putc
        └── timer_arch.c         CNTFRQ_EL0 / CNTPCT_EL0
```

唯一的跨层边是 **Platform → Core**: `start.S` 调 `br_plat_early_init()` 再调 `br_core_main()`。

## 4. 构建与运行

**工具链是外部的**(内部工具链未就绪, 见 §5 的 `br-wa-toolchain-001`)。需要:
`aarch64-linux-gnu-gcc`(或带版本号的 `gcc-14`/`gcc-13`, Makefile 会自动探测)、
`binutils-aarch64-linux-gnu`、`qemu-system-aarch64`。

```bash
cd tangramOS
make                    # 构建 build/brick.elf + .bin
make run                # 在 QEMU virt 上跑(Ctrl-A X 退出)
make smoke              # 3 秒冒烟: 自动判定启动/延时判据, 红绿可进 CI
make size               # 体积
make disasm             # 反汇编
make check-workarounds  # WORKAROUND 登记一致性
make clean
```

换工具链前缀只需一个变量(这是 `br-wa-toolchain-001` 的还债口):

```bash
make CROSS_COMPILE=aarch64-none-elf-
```

实际输出(QEMU virt, `-cpu cortex-a53`):

```
[    0.000093] INFO  brickOS-prototype v0.1.0 -- core MainLoop (delay + logging)
[    0.000985] INFO  platform: qemu-aarch64/virt (aarch64)
[    0.001140] INFO  clock: 62500000 Hz (arch timer), 62500 ticks/ms (exact integer conversion)
[    0.001446] INFO  entry chain: start.S -> br_plat_early_init -> br_core_main
[    1.001694] INFO  tick=1 uptime=1002353 us delay=1000054 us (>=1000000 us: ok)
[    2.001878] INFO  tick=2 uptime=2002554 us delay=1000002 us (>=1000000 us: ok)
```

体积(aarch64 裸机 ELF): `.text` 7060 B / `.rodata` 496 B / `.data` 16 B / `.bss` 16 B;
`brick.bin` ≈ 12 KiB。其中向量表占 2 KiB(每个入口 0x80 字节是 AArch64 的硬性间距)。

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

v0.1.0 有三条欠债, 全部登记在 **[WORKAROUNDS.md](WORKAROUNDS.md)**:

| id | 一句话 |
|---|---|
| `br-wa-entry-001` | **Platform Entry 不是插件**, 被 Makefile 直接编进镜像 —— 这就是你要求的标记项 |
| `br-wa-boot-001` | 启动链被压缩成一个死循环(无 plugin_manager / init 阶段 / 调度器) |
| `br-wa-toolchain-001` | 工具链用外部 gcc |

**`br-wa-entry-001` 的退出条件**(`br` 工具就绪后必须做的三件事):

1. `br` 能按布局约定发现并校验插件(`4-02`);
2. `platform/` 收敛为插件 `platform/qemu-aarch64`(`BR_PLUGIN` 描述符 + 声明片段, `4-03`);
3. `start.S` 的调用点从"Makefile 直编"改为 `.br_plugins` 段枚举驱动(`3-05 §2.2`)。

代码里的标记形如 `WORKAROUND(br-wa-entry-001)`, 与登记表由 `make check-workarounds` 绑死:
**任一侧多/少即报红** —— 欠债最怕的不是欠着, 是没人知道欠着。

## 6. 代码 ↔ 设计对应

| 本原型 | 设计出处(`brickOS-Design` 分支) | 形态差异 |
|---|---|---|
| `start.S` 的 reset/向量表/BSS | `1-01 §9` 启动序列 | 设计是 Platform **插件**的汇编; 此处直编(`br-wa-entry-001`) |
| `br_plat_early_init()` | `1-01 §9` 的 `platform.early_init`; `1-01 §8` 三层模式 | 只做 console; 时钟/region/MMU 未做 |
| `br_console_*` | `1-01 §8` console 双形态; `3-01 §10` 平台侧接口表 | 形态一致(轮询早期 console) |
| `br_clock_now()` / `br_time_t` | `3-01 §4`(br-sched 组); `3-01 §14` CA-1(us) | 只实现读数; 超时表/唤醒属 M1 |
| `br_log_*` | `5-01`(trace 观测)/ `11-01`(日志 Service) | v0.1.0 是 core 内的最小打印设施, 不是那个服务 |
| `br_core_main()` | `1-01 §9` + `§6.2` 阶段表 | 顶替整条 core.init → plugin_manager → EARLY/CORE/LATE → `br_sched_run()`(`br-wa-boot-001`) |
| 异常兜底打印 | `3-02 §8.3.1` bare 路径 | 未实现 `br_panic_bare(fmt, ...)`, 只打固定行 |

## 7. 下一步(往 M0 走)

按设计 `1-03 §3`, M0 = **启动链 + 插件管理**, 验收 = "hello + init 链打印 + 故意造环看组合器报错"。
本原型离它有四步, 顺序有依赖关系:

1. **`br` 最小形态 = 求解器 + 可解释报错**(`2-02 §3` BR-D2 / §7: M0 的 `br check` 先于完整 CLI);
   这一步是 `br-wa-entry-001` 的前提。
2. 插件描述符段(`.br_plugins` + `__br_plugins_start/stop`)与**声明面唯一真值**(`2-02` BR-D2)。
3. `platform/` 收敛为插件 → 兑掉 `br-wa-entry-001`。
4. 中断框架 + 调度器(M1) → 把 MainLoop 拆成 `app.start()` 的 APP 线程 + `br_sched_run()`。

在 4 之前, `br_core_main` 会一直在那里 —— 但它的归宿是**被拆掉, 不是长大**(见 `main.c` 顶部注释)。
