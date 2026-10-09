/*
 * brickOS prototype v0.2.0 — core 启动入口(br_core_main)
 *
 * 设计依据: `docs/1-architecture/1-01-architecture.md` §9(启动序列)/§8(平台能力三层模式)。
 * 逐条裁定与"为什么要把编排权从插件管理器收回这里": `docs/decisions/0008-core-main-boot-chain.md`
 * (本头文件是它的落地契约)。
 *
 * ======================= 启动链(唯一真值在这里) =======================
 *   platform 插件:  reset 汇编(向量表 / BSS 清零 / 选核 / SP)   [start.S]
 *          │
 *          └─ bl br_core_main ────────────────────────────────────────┐
 *                                                                     ▼
 *   core:  ① 平台无关初始化: 时钟 → 日志 → 中断框架内部状态 → 插件管理器扫段(静默)
 *          ② platform 插件初始化: plugin_type == platform 的 early_init
 *             (早期 console / PIC 注册与绑定表 / region 表声明 / 页表 ops 注册)
 *          ③ 依赖平台实现的 core 模块: br_mem_init()(region 表 → 三池)
 *             → br_mm_activate()(region 表 → 4 KiB 恒等映射 + 开 MMU)
 *          ④ br_plugin_manager_run(): 拓扑 + 相位驱动
 *             (EARLY 其余 → CORE → LATE → 全局开中断 → START) → br_sched_run()
 * ======================================================================
 *
 * 分工(与 1-01 §8 的三层模式对齐):
 *   platform 插件 提供"换型号/换架构就会变"的**机制与数据**(console/PIC/region 表/页表构造);
 *   core          提供**抽象与编排**(插件管理/日志/时钟/中断框架/堆/地址映射的建立)。
 *   一句话: platform 的 Start.S 只做 reset/BSS, 然后 `bl br_core_main`; 启动链的每一格
 *   顺序都落在本入口的四个阶段里(逐格与 1-01 §9 对应)。
 */
#ifndef BR_CORE_BR_MAIN_H
#define BR_CORE_BR_MAIN_H

#include <br/core/br_types.h>

/*
 * core 的启动入口(**不返回**)。
 *
 * 四阶段(顺序都是硬的, 逐条理由在 `core/src/main.c` 的注释里):
 *   ① core 平台无关: `br_irq_cpu_init()` 必须排在 platform 提交 PIC 绑定表**之前**
 *      (它把描述符池的 `pic_id/dom_id` 置 -1, 见 P-IRQ-5); 本阶段**全程静默** ——
 *      console 由阶段 ② 的 platform `early_init` 才配好, 在那之前写 PL011 的 DR 会被
 *      QEMU 丢掉(CR.UARTEN = 0)。
 *   ② platform 插件初始化: 显式取 `plugin_type == platform` 的那一条跑 `early_init`
 *      (不靠拓扑序的巧合 —— 此刻 PIC/页表/console 都还没就绪)。
 *   ③ core 依赖平台实现: `br_mem_init()`(按 region 表认领三池) → `br_mm_activate()`
 *      (按同一张表建恒等映射并开 MMU)。调度框架对象(`br_sched`)是纯静态 BSS, 没有
 *      运行期构造; 它的**注册**是 sched 插件在 EARLY 相做的事(`coop_early_init`)。
 *   ④ `br_plugin_manager_run()`: 建 init 边 → 拓扑排序 → 四相驱动 → `br_sched_run()`。
 *
 * 失败粒度 = 首败即停机(ADR-0005 裁定 G6): ②③ 失败走 `br_panic`(console 已可用);
 * ① 失败时 console 未起, 输出会被 QEMU 丢掉, 但仍停机而不是继续跑一个半初始化的系统。
 */
BR_NORETURN void br_core_main(void);

#endif /* BR_CORE_BR_MAIN_H */
