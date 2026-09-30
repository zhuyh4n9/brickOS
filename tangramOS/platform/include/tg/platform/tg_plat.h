/*
 * TangramOS prototype v0.1.0 — Platform Entry 契约(aarch64)
 *
 * ============================ WORKAROUND 声明 ============================
 * 本头文件描述的是 **Platform Entry**, 设计上它应当是一个 **Platform 插件**
 * (设计侧 1-01 §6.3 / §8 三层模式: 每个 SoC 一个 platform 插件, 提供
 *  reset 汇编、早期 console、时钟、region 表、中断控制器实现)。
 *
 * v0.1.0 **还没有 `tg` 组合器, 也没有插件管理器**, 所以 Platform Entry 不是插件,
 * 而是被 Makefile 直接编进镜像的一组文件。这是**临时形态**, 标记为:
 *
 *     WORKAROUND(tg-wa-entry-001)
 *
 * 退出条件(设计侧工具就绪后必须改):
 *   1. `tg` 能发现/校验插件布局(设计 4-02)并生成描述符(4-03);
 *   2. 本目录收敨为插件 `platform/qemu-aarch64`(带 TG_PLUGIN 描述符 + manifest);
 *   3. Start.S 的调用点从"Makefile 直编"改为"tg add platform/qemu-aarch64"后的
 *      .tg_plugins 段驱动(设计 3-05 §2.2 的 __tg_plugins_start/__tg_plugins_stop)。
 * 详见 WORKAROUNDS.md。
 * ========================================================================
 *
 * 分层(设计 1-01 §8「平台能力三层模式」在本原型的落点):
 *   core 接口      ->  platform/include/tg/platform/tg_plat.h(本文件)
 *   ISA 共享实现   ->  直接用内联汇编读 arch timer(platform/src/aarch64/timer_arch.c)
 *   平台提供的数据 ->  PL011 基址 / 波特率 / 时钟频率 / region 表(QEMU virt 值)
 */
#ifndef TG_PLATFORM_TG_PLAT_H
#define TG_PLATFORM_TG_PLAT_H

#include <tg/core/tg_types.h>

/*
 * 平台早期初始化。**由 Platform Entry(start.S)调用**, 在 BSS 清零之后、
 * 进入 core 之前。v0.1.0 只做两件事:
 *   1. 早期 console 就绪(轮询 PL011)—— 必须最先, 之后才有可观测性;
 *   2. 记录平台参数(供 core 的时钟换算使用)。
 *
 * 设计侧它对应 platform.early_init(1-01 §9): 时钟 / 引脚 / RAM / 恒等映射页表
 * (tg_mm) / 早期 console。v0.1.0 只有 console —— 因为 MainLoop 不碰内存管理与 MMU。
 */
void tg_plat_early_init(void);

/* 平台身份, 用于启动横幅与诊断(设计侧对应 platform 插件的 name)。 */
const char *tg_plat_name(void);
const char *tg_plat_isa(void);

/* ---- arch timer(aarch64 通用, 由 platform 提供) ----
 * 为什么在 platform 而不是 core: 设计 1-01 §8 表 —— `tg_clock` 是 core 的
 * tickless 语义接口, 而"读哪个计数器、频率多少"归 platform(ISA 共享库 + 平台数据)。 */
tg_u64 tg_plat_ticks_freq(void);   /* CNTFRQ_EL0, Hz */
tg_u64 tg_plat_ticks_now(void);    /* CNTPCT_EL0, 单调递增计数 */

/* 停机(不可返回)。没有调度器, 所以没有"idle 线程", 只有 WFE 死循环。 */
TG_NORETURN void tg_plat_park_forever(void);

/*
 * 向量表里未被处理的异常统一落到这里(设计侧对应 3-02 §8.3.1 的 bare 路径:
 * 不走可能已损坏的 core 设施, 只轮询 console 打一行然后停机)。
 * `vector` = 异常向量下标 0..15。不返回。
 */
TG_NORETURN void tg_plat_unexpected_exception(tg_u64 vector);

#endif /* TG_PLATFORM_TG_PLAT_H */
