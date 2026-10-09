/*
 * brickOS prototype v0.2.0 — core 启动入口(br_core_main): 四阶段启动链的编排
 *
 * 设计依据: `1-01` §9(启动序列)/§6.2(生命周期四相)、`3-05`(plugin_manager)、
 *           `3-04`(内存: region 表 → 三池 → 恒等映射)、`3-02` §14.3(中断初始化单向链)。
 * 逐条裁定与替代方案: `docs/decisions/0008-core-main-boot-chain.md`。
 *
 * ## 为什么要这个入口(还的是哪笔债)
 *   v0.1/v0.2 的启动链**混在插件管理器里**: `start.S` 调 `br_irq_cpu_init` 之后直接进
 *   `br_plugin_manager_run()`, 而后者在相位驱动之前顺手做了"时钟/日志/扫段/platform.
 *   early_init/堆/MMU"五件事(见 ADR-0005 §2.6)。后果:
 *     - 设计 `1-01` §9 的 `core.init` 一格**没有落点**(登记为 br-wa-boot-001 ①);
 *     - `br_mem_init()` 藏在 platform 的 memmap 里 —— 但"按 region 表认领池"是 core 的事;
 *     - `br_plat_mmu_init()` 既注册 ops 又 activate(建表 + 开 MMU) —— 机制与编排混在一处。
 *   本刀把编排权收到这里, 插件管理器回归"只管插件"(ADR-0008)。
 *
 * ## 四阶段(逐格对应 1-01 §9; 顺序都是硬的)
 *
 *   ① core 平台无关初始化 —— `core_init_platform_agnostic()`
 *      `br_clock_init()` → `br_log_init()` → `br_irq_cpu_init()` → `br_plugin_manager_init()`
 *      ★ `br_irq_cpu_init()` 必须在 **platform 提交 PIC 绑定表之前**: 它把描述符池的
 *        `pic_id/dom_id` 全置 -1(P-IRQ-5), 排在绑定表之后会把已落的绑定抹掉。
 *      ★ 本阶段**全程静默**: console 属阶段 ②, 此刻写 PL011 的 DR 会被 QEMU 丢掉。
 *
 *   ② platform 插件初始化 —— `br_plugin_manager_platform_init()`
 *      显式取 `plugin_type == platform` 的插件跑 `early_init`: 早期 console → GICv3
 *      PIC 注册 + 绑定表 → region 表声明 → 页表 ops 注册。
 *
 *   ③ core 依赖平台实现的模块 —— `core_init_platform_dependent()`
 *      `br_mem_init()`(按 region 表认领 TLSF 堆/连续池/页池) →
 *      `br_mm_activate()`(按同一张 region 表建 4 KiB 恒等映射并开 MMU)。
 *      调度框架对象(`br_sched`)是纯静态 BSS、**没有运行期构造**; 它的注册是 sched 插件
 *      在 EARLY 相做的事(`coop_early_init()` → `br_sched_register()`), 不属本阶段。
 *
 *   ④ `br_plugin_manager_run()` —— 建 init 边 → Kahn 拓扑 → EARLY(其余)/CORE/LATE
 *      → 全局开中断 → START(APP 最后) → `br_sched_run()`。**不返回**。
 *
 * ## 失败粒度(与 ADR-0005 裁定 G6 一致)
 *   首败即停机, 不降级。②③ 失败时 console 已可用 ⇒ `br_panic()`(带 `[PANIC]` 行);
 *   ① 失败时 console 未起(输出被 QEMU 丢掉), 但仍停机 —— "继续跑一个半初始化的系统"
 *   只会让症状离根因更远。
 */
#include <br/core/br_fault.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_main.h>
#include <br/core/br_mem.h>
#include <br/core/br_mm.h>
#include <br/core/br_plugin.h>
#include <br/core/br_time.h>

/* ==================================================================== 阶段 ① */

static void core_init_platform_agnostic(void)
{
    /* 时钟 → 日志: 日志的时间基点依赖 br_clock_init, 顺序不能反(与 v0.1 相同)。 */
    br_clock_init();
    br_log_init();
    br_log_set_level(BR_LOG_DEBUG);

    /* 中断框架的 per-CPU 状态 + 描述符池静态初始化(P-IRQ-5)。
     * ★ 排在阶段 ② 之前是**硬约束**: platform 的 `br_plat_irq_init()` 会经
     *   `br_irq_bindings_set()` 填描述符的 pic_id/dom_id, 而本函数把**全部**描述符
     *   置 -1 ⇒ 顺序颠倒会把绑定表抹成"无绑定"(未绑定 = -ENODEV 的判据随之失真)。 */
    br_irq_cpu_init();

    /* 插件管理: 扫 `.br_plugins` + 显式找出 platform 插件(静默 —— 见文件头注)。
     * 建 init 边/拓扑排序/相位驱动是阶段 ④ 的事, 此处只做"段 → 表"。 */
    br_plugin_manager_init();
}

/* ==================================================================== 阶段 ③ */

static void core_init_platform_dependent(void)
{
    /* memory: core 按 platform 声明的 region 表认领三池(3-04 §2)。
     * 失败 = region 表与三池口径不一致(缺必需池/粒度不合/池太小), 属启动失败。 */
    if (br_mem_init() != 0) {
        br_panic("core.init: br_mem_init() 失败 —— platform 的 region 表与三池不一致");
    }

    /* address mapping: core 提供抽象并执行**建立动作**(`br_mm_activate()`);
     * 页表构造的机制由 platform 在阶段 ② `br_mm_register()` 进来。这一步建 4 KiB
     * 恒等映射并开 MMU —— 之后每次取指/访存都过翻译, 所以它必须是插件 init 之前的
     * 最后一格(此刻的 PC/SP 所在页必须已被新页表覆盖)。 */
    if (br_mm_activate() != 0) {
        br_panic("core.init: br_mm_activate() 失败(页表构造 / 开 MMU)");
    }
}

/* ==================================================================== 入口 */

BR_NORETURN void br_core_main(void)
{
    /* ① core 平台无关初始化(时钟/日志/中断框架内部状态/插件管理扫段) */
    core_init_platform_agnostic();

    /* ② platform 插件初始化(早期 console/PIC 与绑定表/region 表/页表 ops) */
    br_plugin_manager_platform_init();

    /* ③ core 依赖平台实现的模块(memory + 地址映射的建立) */
    core_init_platform_dependent();

    /* ④ 各插件的初始化与启动: 拓扑 + EARLY/CORE/LATE → 开中断 → START → 调度。
     * br_plugin_manager_run() 是 BR_NORETURN ⇒ 下面不可达。 */
    br_plugin_manager_run();
}
