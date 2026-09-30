/*
 * brickOS prototype v0.1.0 — core 入口
 *
 * v0.1.0 的启动链(设计侧对应见 1-01 §9 启动序列, 在 brickOS-Design 分支):
 *
 *   设计的 M0+ 形态:  reset 汇编 -> platform.early_init -> core.init
 *                     -> plugin_manager(.br_plugins 拓扑) -> EARLY/CORE/LATE
 *                     -> 开中断 -> 各插件 start() -> app.start() -> br_sched_run()
 *
 *   v0.1.0 的形态:   platform Start.S -> br_plat_early_init() -> br_core_main()
 *                     └ 中间三层(plugin_manager / init 阶段 / 调度器)**整体缺席**,
 *                       由 MainLoop 一个死循环顶替 —— 见 WORKAROUNDS.md br-wa-boot-001。
 *
 * 因此 `br_core_main` 是一个**占位入口**: 它的最终归宿不是"变大", 而是
 * **被拆掉** —— 拆成 core.init + plugin_manager + 插件 start() + br_sched_run()。
 */
#ifndef BR_CORE_BR_MAIN_H
#define BR_CORE_BR_MAIN_H

#include <br/core/br_types.h>

/*
 * core 主循环: 延时 + 日志。
 * 由 Platform Entry(platform/src/aarch64/start.S)在 br_plat_early_init() 之后调用。
 * 不返回(没有可返回的地方 —— 没有调度器, 也就没有 idle 线程)。
 */
BR_NORETURN void br_core_main(void);

#endif /* BR_CORE_BR_MAIN_H */
