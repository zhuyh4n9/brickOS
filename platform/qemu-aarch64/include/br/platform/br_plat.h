/*
 * brickOS prototype v0.1.0 — Platform Entry 契约(aarch64)
 *
 * ============================ WORKAROUND 声明 ============================
 * 本头文件是 **Platform 插件 `platform/qemu-aarch64` 的对外面**(设计侧 1-01 §6.3 /
 * §8 三层模式: 每个 SoC 一个 platform 插件, 提供 reset 汇编、早期 console、时钟、
 * region 表、中断控制器实现)。
 *
 * 还债进度(`WORKAROUND(br-wa-entry-001)`):
 *   ① `brickie` 已能发现/校验插件布局(设计 4-02)并生成描述符(4-03) —— **已还**;
 *   ② 本目录已收敛为插件 `platform/qemu-aarch64`(manifest = `plugin.toml`,
 *      `BR_PLUGIN` 描述符 = 生成物 `build/gen/platform/qemu-aarch64/plugin_desc.c`)
 *      —— **已还**;
 *   ③ **仍欠**: Start.S 的调用点还没有改由 `.br_plugins` 段枚举驱动
 *      (设计 3-05 §2.2 的 __br_plugins_start/__br_plugins_stop)——那属 M0 运行期
 *      (插件管理器 + 调度器就位后), 现在仍由 Makefile 直编。
 * 详见 WORKAROUNDS.md 与 docs/decisions/0001-platform-plugin-manifest.md。
 * ========================================================================
 *
 * 分层(设计 1-01 §8「平台能力三层模式」在本原型的落点):
 *   core 接口      ->  platform/qemu-aarch64/include/br/platform/br_plat.h(本文件)
 *   ISA 共享实现   ->  直接用内联汇编读 arch timer(src/timer_arch.c)
 *   平台提供的数据 ->  PL011 基址 / 波特率 / 时钟频率 / region 表(QEMU virt 值)
 */
#ifndef BR_PLATFORM_BR_PLAT_H
#define BR_PLATFORM_BR_PLAT_H

#include <br/core/br_types.h>

/*
 * 平台早期初始化。**由 Platform Entry(start.S)调用**, 在 BSS 清零之后、
 * 进入 core 之前。现在做三件(按此顺序):
 *   1. 早期 console 就绪(轮询 PL011)—— 必须最先, 之后才有可观测性;
 *   2. 中断子系统的平台侧三步(PIC 注册 / caps / 绑定表, `br_plat_irq_init`);
 *   3. 内存映射: 声明 region 表 → `br_mem_init()` 认领三池 → 建 4 KiB 恒等映射
 *      页表并开 MMU(`br_mm_register` + `ops.activate`)。
 *
 * 设计侧它对应 platform.early_init(1-01 §9): 时钟 / 引脚 / RAM / 恒等映射页表
 * (br_mm) / 早期 console。第 3 步落地后, "MMU 用于 region 属性"的 v1 政策生效,
 * `-mstrict-align` 从"硬要求"退化为"防御性旋钮"(见 Makefile 的注释)。
 */
void br_plat_early_init(void);

/* 平台身份, 用于启动横幅与诊断(设计侧对应 platform 插件的 name)。 */
const char *br_plat_name(void);
const char *br_plat_isa(void);

/* ---- arch timer(aarch64 通用, 由 platform 提供) ----
 * 为什么在 platform 而不是 core: 设计 1-01 §8 表 —— `br_clock` 是 core 的
 * tickless 语义接口, 而"读哪个计数器、频率多少"归 platform(ISA 共享库 + 平台数据)。 */
br_u64 br_plat_ticks_freq(void);   /* CNTFRQ_EL0, Hz */
br_u64 br_plat_ticks_now(void);    /* CNTPCT_EL0, 单调递增计数 */

/* ---- 中断子系统(设计 3-02 §14.3 的初始化单向链) ----
 *
 * `br_plat_early_init()` 内部的步骤 1–3(平台侧): 
 *   br_pic_register(driver) → caps_get → br_irq_bindings_set(绑定表)
 * 任一步失败 ⇒ 返回负 errno 且**不降级**(EARLY 相失败 = 启动失败:
 * 此时全局关中断, "静默返回 0 但没配好"会让症状远离根因, 3-02 §14.3 错误处理义务)。
 *
 * 为什么这几件在 platform: 送的是**数据与特化**(PIC 实例/基址/优先级位数/
 * virq↔hwirq 绑定/板级静态 prio&trigger), 机制与语义在 core(3-02 §1.3)。
 */
int br_plat_irq_init(void);

/*
 * EL1 物理 timer(PPI INTID 30)作为 M0 的**实际中断源**(3-02 §11.1 路径四)。
 *
 * ★ `br_plat_irq_start()` 是"设备与中断一起开跑"的单一入口: 注册 timer 的 ISR →
 *   使能该线 → 装第一个期限 → **全局开中断**(设计 §14.3 初始化链的最后一步)。
 *   为什么这些在 platform 而不在 APP: 设计 3-01 §13.6 把"中断控制"归 **P3**、
 *   "IRQ 线"归 P1, 而 APP 是 **P0** ⇒ APP 不得碰 `br_irq_register/enable`。
 *   返回 0 或负 errno(失败即启动失败: 不静默降级)。
 */
int br_plat_irq_start(void);

/* 心跳计数(timer PPI 的 ISR 维护)。APP 只**读**这个计数 —— 于它仍是纯 P0 消费者。 */
br_u32 br_plat_timer_ticks(void);

/* 停掉 timer(诊断/将来 PM 用)。 */
void br_plat_timer_stop(void);

/*
 * 中断一致性用例入口(设计 6-01 §3.7 的 TC-IRQ-*, 在 QEMU 上跑)。
 * 返回**失败项数**(0 = 全绿), 并把每项结论打成 `[IRQCONF] PASS/FAIL <用例名>` 行 ——
 * 于是"红绿"可被 `make smoke` / CI 的 grep 判定, 不靠人眼。
 */
int br_plat_irq_conformance(void);

/*
 * 软件触发一个"绑定为 SGI"的 virq(自测/一致性用例通道, 设计 3-02 §8.4)。
 * 走**完全相同的入口路径**(ack → ISR → eoi), 因此它是中断子系统的自检通道。
 * 返回 0 或负 errno(未绑定/非 SGI/控制器不支持软件触发)。
 */
int br_plat_irq_trigger(br_u32 virq);

/*
 * virq → 绑定表里的 hwirq(EARLY 阶段已提交的**静态**映射)。
 * 用途受限: 只允许 conformance/debug 做"影子 vs 硬件"的交叉校验
 * (设计 3-02 §4.3 第 5 条: 可回读控制器允许在 conformance 里交叉校验)。
 * 返回 0 或 -ENODEV。
 */
int br_plat_irq_hwirq(br_u32 virq, br_u32 *out);

/* ---- 内存映射子系统(设计 3-04; 页表/region 表/三池) ----
 *
 * 平台侧的三件事(顺序有讲究, 见 `br_plat_early_init` 的注释):
 *   ① 声明 region 表(`br_mm_region_add`: 镜像/栈/三池/MMIO/保留区);
 *   ② `br_mem_init()`(core 按 region 种类认领池);
 *   ③ `br_mm_register(ops)` + `ops.activate()` 建 4 KiB 恒等映射页表并开 MMU。
 *
 * `br_plat_mem_conformance()` = 内存/MMU 一致性用例入口(设计 6-01 §3.5/§3.6 的
 * `TC-MEM-*` 与 `TC-MM-*`, 在 QEMU 上跑)。返回**失败项数**(0 = 全绿), 每项打
 * `[MEMCONF] PASS/FAIL <用例名> <描述>` 行, 末尾打 `[MEMCONF] SUMMARY pass=N fail=0 total=N`
 * —— 于是红绿由 `make dbg-test` / CI 的 grep 判定, 不靠人眼。
 */
int br_plat_mem_conformance(void);

/* 停机(不可返回)。没有调度器, 所以没有"idle 线程", 只有 WFE 死循环。 */
BR_NORETURN void br_plat_park_forever(void);

/*
 * 向量表里未被处理的异常统一落到这里(设计侧对应 3-02 §8.3.1 的 bare 路径:
 * 不走可能已损坏的 core 设施, 只轮询 console 打一行然后停机)。
 * `vector` = 异常向量下标 0..15。不返回。
 */
BR_NORETURN void br_plat_unexpected_exception(br_u64 vector);

#endif /* BR_PLATFORM_BR_PLAT_H */
