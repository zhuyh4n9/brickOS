# WORKAROUND 登记表 — brickOS-prototype v0.1.0

> **为什么要有这张表**: 原型允许走捷径, 但不允许**悄悄**走捷径。
> 每条捷径在这里登记 id / 欠的是什么 / 为什么先欠着 / **还债的具体动作**。
> 代码里对应位置标 `WORKAROUND(<id>)`, 两侧由 `make check-workarounds` 绑死
> (源码标记 ↔ 本表条目, 任一侧多/少即报红)。

**v0.1.0 的七条欠债** —— 根因分三类:
① **运行期缺席**: `brickie` v0.1 已交付声明期(发现/校验/描述符生成/接口发布/版本治理),
   但**运行期的插件管理器、阶段机与调度器仍未存在**(M0/M1), 所以"插件化"与"启动链"
   这两条主线各欠着一半;
② **分层未抽出**: 设计把中断控制器方言、异常向量桩与**页表构造**归 **ISA 共享库(非插件)**,
   而原型还没有这一层, 于是它们暂居 platform 插件目录内(靠**文件边界**保住层次);
③ **生成链路未抽出**: 设计的"池比例 = manifest 预算"(3-04 §2)与"debug 呈现经 bridge"
   (5-01 §2)都还没有生成/传输链路, 于是前者写死在平台数据里、后者直写 console。

| id | 位置 | 欠的是什么 | 为什么先欠着 | 还债动作(退出条件) |
|---|---|---|---|---|
| br-wa-entry-001 | platform/qemu-aarch64/plugin.toml<br>platform/qemu-aarch64/include/br/platform/br_plat.h<br>platform/qemu-aarch64/src/start.S<br>platform/qemu-aarch64/src/link.ld<br>platform/qemu-aarch64/src/console_pl011.c<br>platform/qemu-aarch64/src/timer_arch.c<br>platform/qemu-aarch64/src/plat_qemu_virt.c<br>platform/qemu-aarch64/src/memmap.c | **Platform 的插件化只完成了一半**。reset 汇编 / 向量表 / 链接脚本 / 早期 console / arch timer / GICv3 / **region 表与页表**仍由 Makefile 直接编进镜像; **manifest 与描述符已就位**(`plugin.toml` + 生成物 `build/gen/platform/qemu-aarch64/plugin_desc.c`, 由 `brickie gen` 重建), 但**调用点**还不是 `.br_plugins` 段枚举驱动 | 设计侧 Platform 类是"每 SoC 一个插件"(1-01 §6.3); 插件**发现与校验**已由 brickie v0.1 交付, 但**描述符段驱动的启动链**依赖插件管理器与调度器(M0 运行期), 它们不在 v0.1 | ① ~~`br` 能按布局约定发现并校验插件(4-02)~~ ⇒ **已还**(`brickie check`/`gen`, 见 `docs/decisions/0001-platform-plugin-manifest.md`); ② ~~本目录收敛为插件 `platform/qemu-aarch64`(带 `BR_PLUGIN` 描述符 + manifest)~~ ⇒ **已还**; ③ **仍欠**: `start.S` 的调用点从"Makefile 直编"改为 `.br_plugins` 段枚举驱动(3-05 §2.2 的 `__br_plugins_start/__br_plugins_stop`) —— 依赖插件管理器(M0) |
| br-wa-boot-001 | app/hello/src/main.c<br>core/src/time.c<br>core/src/log.c<br>core/src/irq/irq_core.c<br>core/src/trace.c<br>core/src/mem/mem.c<br>platform/qemu-aarch64/src/irq_conf.c<br>platform/qemu-aarch64/src/mm_conf.c | **启动链仍被压缩成一个死循环**。设计的 `core.init → plugin_manager(拓扑排序/环检测) → EARLY/CORE/LATE → 开中断 → start() → app.start() → br_sched_run()`(1-01 §9)在 v0.1.0 **仍无阶段机**; 中断与内存子系统(3-02 Stage 1 / 3-04)已落地, 但它们挂接的几处都靠"替身": `core.init` 的 TPIDR_EL1 初始化 = `br_irq_cpu_init()`(start.S 直接调)、"全部 init 之后开中断" = 一致性用例入口显式 `br_irq_cpu_enable()`、"core 注册 timer PPI 的 ISR" = APP 代做、**三池初始化 = `br_plat_early_init()` 直接调 `br_mem_init()`**、**五个调试服务插件的 init/调用 = APP 直调(靠 `product.toml` 的两条 `allow_edges` 豁免)**。此外"睡眠"退化为忙等, 日志/trace 直写 console/RAM 环而未经服务注册表 | 这条链的每一环都以"有插件"为前提(描述符段、注册表、调度器); MainLoop 明确归属 APP 插件 `app/hello`(1-03 §1 的 M0 引导例外), 但阶段机与调度器仍未落地 | **`br_core_main` 的归宿不是长大而是被拆掉**: 平台名打印 → 描述符枚举; `br_irq_cpu_init`/"开中断"/timer ISR 注册 → `core.init` 的三步; `br_mem_init` 与"开 MMU" → `core.init`/platform 相; 五个 service 插件的 init → 插件管理器按 `[[dep]]` 拓扑驱动(替掉 `allow_edges`); `br_log_init`/trace 环 → `core.init` + service/trace; 循环体 → `app.start()` 里的 APP 线程; 循环边的 `br_delay_ms` → `br_task_sleep`(M1) |
| br-wa-isa-001 | platform/qemu-aarch64/src/gicv3.c<br>platform/qemu-aarch64/src/vectors.S<br>platform/qemu-aarch64/src/mmu.c<br>core/include/br/core/br_exc.h | **ISA 共享库这一层还没有独立存在**。设计 1-01 §8 / 3-02 §1.3 把"控制器方言(GICv3 寄存器序列)+ 异常向量入口汇编"、3-04 §2 把"cache 一致性协议的 aarch64 实现"归 **ISA 共享库(可复用库, 非插件)**; 原型里它们与 platform 插件同目录(靠文件边界分层: `gicv3.c`/`mmu.c` 只有"换型号/换架构就变"的序列, 板级事实上在 `board_irq.c`/`memmap.c`/`plat_qemu_virt.c`); 异常帧布局(`br_exc.h`)因 core 的 extable fixup 必须改写现场而暂放 core | 抽成独立构建单元要动 Makefile 的源集合/包含路径, 而 **platform → isa 的依赖没有声明载体**(ISA 库不是插件, 无 `[[dep]]` 可表达); 收益是分层整洁, 不是本次任务的功能 | 建 `isa/aarch64/`(**非插件**)独立构建单元 + 把 `gicv3.c`/`vectors.S`/`mmu.c`/`br_exc.h` 迁入, 并在 `plugin.toml` 的声明面里表达"platform 使用该 ISA 单元"(机制待 4-02/2-01 定) |
| br-wa-toolchain-001 | Makefile | **工具链用外部 gcc**(宿主 `aarch64-linux-gnu-gcc`), 未经内部工具链 | 内部工具链未就绪; 原型阶段先验证"能不能跑起来" | 内部工具链就绪后**只改 `CROSS_COMPILE`**(或换 `CC`), 构建规则本身不动。同时补 `abi_id` 指纹(设计 2-02 §C5/BR-D5: 影响 ABI 的输入要可复现地记下来) |
| br-wa-mem-001 | platform/qemu-aarch64/src/memmap.c | **三池比例写死在 platform 的 region 表里**(heap 1 MiB / contig 256 KiB / page 1 MiB / DMA 256 KiB / 保留 16 KiB), 不是由 manifest 的 `[budget]`/`[[res]]` 预算生成; 页池位图与页表也都是编译期静态上界 | 设计 3-04 §2 说"三池划分…比例 = manifest 预算(主文档 §6.4 资源总账)", 但 v0.1 **没有"manifest → 平台数据"的生成链路**(brickie v0.1 只治理声明面, 编译编排属 v0.3) | 组合器把 `[budget]`/`[[res]]` 变成 region 表(或 core 侧引入由 manifest 裁剪的池描述符); 判据 = 改 `product.toml` 的预算就能改池大小, 且 `brickie check` 的预算域随之执法 |
| br-wa-debug-001 | service/memleak/src/memleak.c | **memleak 是"归属标签"近似, 不是 v2 的 per-plugin arena 记账**: 有红区/毒化/双 free 拦截与"分配时记归属"的标签, 但**没有**预算上限、没有强制归属、没有 OOM 策略 | 5-01 §4 把 "per-plugin arena 记账" 排在 v2.0, 并把 "TLSF 红区 + freelist 毒化 + 栈 canary" 列为 v1.x 的便宜替代; 本次先交付"能报谁没还、在哪分配的"这一半(另一半要插件的资源预算真值) | v2 落地 per-plugin arena(分配带插件归属 + 预算 + 超限策略); 本插件的报告面**不变**(它消费的是 core 的观测契约), 只多出"超预算"一栏 |
| br-wa-debug-002 | service/dump/src/dump.c | **dump/trace 直写早期 console**, 未经 5-01 §2 的 debug bridge(COBS + 16-bit CRC 成帧 + `MEMRD`/`TRACE_READ` 命令面) | bridge 排在 M3(v1.0 插件清单的 `service/dbg-bridge`), 依赖 vfs-core 的 `/dev/uart0` 与 trace 服务; v0.1 没有这两个 | 把呈现层改为经 bridge 成帧输出(内容与边界判定不变, 只换落点); 判据 = 主机侧 `brickie dbg` 能收帧并解码 |

## 相关但**不算** workaround 的事(避免误登记)

以下看起来像捷径, 实际都是**设计内**的形态, 所以不登记:

| 事项 | 为什么是设计内行为 |
|---|---|
| 早期 console 用**轮询**、不注册设备 | 设计 1-01 §8 明确 console 双形态: platform 早期 console(轮询) → I/O 插件完整 tty(中断驱动)。1-03 §1 里 M0 的 `io/uart-pl011` 也不注册设备 |
| **SLOW 级联域在 v0.1 不可用**(`br_irq_domain_create` 返回 NULL) | 设计 3-02 §1.1.1 的两段计: SLOW 域的**存在前提就是 bh**(Stage 2 / v1.0 起可用)。拒绝并留痕正是"分期边界的定义", 不是缺陷 |
| **`br_fault_handler_register` 未实现**(仅有签名) | 设计 3-02 §1.1/§10.5: fault 的"分槽/分类/extable/panic"属 Stage 1, **注册 API 归 v2** |
| **`board_irq.h` 是手写静态头**(不是组合器生成物) | 设计 3-02 §3.1(IR-2)明示的**退路**: "若生成头机制未落地: platform 插件导出静态头, 内含同型宏。语义等价, 仅'谁生成'不同" |
| **`BR_IRQ_DISPATCH_BH/_THREAD` 接受但忽略** | 设计 3-02 §3.4 原文如此: "Stage 1 置位不报错但忽略(无 bh), trace 提示" |
| 恒等映射/MMU/cache 维护全无 | MainLoop 不碰内存管理; region 表与页表是 platform 插件在其后的事 |
| 描述符池/运行期表/域池/trace 环是 **core 内的静态数组**(非 manifest 裁剪) | 设计 3-02 §14.6 允许 manifest 裁剪 `BR_IRQ_MAX` 等; v0.1 没有 manifest→core 的生成链路, 故取设计默认值(64 线) |

## 检查

```bash
make check-workarounds      # 源码标记 ↔ 本表, 不一致则非零退出
```
