/*
 * brickOS prototype v0.1.0 — APP 入口(M0): MainLoop(延时 + 日志)
 *
 * 调用者: Platform 插件(platform/qemu-aarch64/src/start.S), 在
 * br_plat_early_init() 之后。这是 v0.1.0 全镜像里唯一的一条跨层边:
 * Platform -> APP(app → platform 是 `§7.3` 的**M0 引导例外**, 已显式登记在
 * product.toml 的 [lint].allow_edges; 见 app/hello/README.md)。
 *
 * 声明在 core 头文件 br/core/br_main.h(M0 入口契约), 实现随 APP 走 —— 按设计
 * 1-03 §1 的 "app/hello: 启动链演示(M0: 直接主循环, 不依赖 iface —— M0 引导例外)"。
 *
 * ======================= WORKAROUND(br-wa-boot-001) =======================
 * 设计侧这里是一整条启动链(1-01 §9 / §6.2 阶段表):
 *
 *   core.init(堆/中断框架/注册表/调度框架对象, 无线程)
 *     -> plugin_manager 扫 .br_plugins 段 + 拓扑排序(环 = 硬错误)
 *       -> EARLY(调度插件注册 br_sched_ops, core 锁定)
 *         -> CORE(非服务插件 init)
 *           -> LATE(Service -> Interface 依序 init)
 *             -> 全局开中断 -> 各插件 start()
 *               -> app.start() 创建 APP 线程
 *                 -> br_sched_run() 首次调度, idle 进 WFI
 *
 * v0.1.0 **整条链缺席**: 没有堆、没有注册表、没有插件描述符、没有调度器,
 * 所以没有 EARLY/CORE/LATE 可挂, 也没有 idle 线程可以让出 CPU。
 * 于是这条链被压缩成"一个死循环里做延时 + 日志"。
 *
 * 关键认识: **`br_core_main` 的归宿不是长大, 而是被拆掉。**
 *   - 顶层那行 `br_plat_name()` 打印 -> 变成插件描述符枚举
 *   - `br_log_init()`               -> 变成 core.init 的一步
 *   - 循环体                        -> 变成 app.start() 里的 APP 线程
 *   - 循环边的延时                  -> 变成 br_task_sleep(M1)
 * ==========================================================================
 */
#include <br/core/br_main.h>
#include <br/core/br_log.h>
#include <br/core/br_time.h>
#include <br/core/br_version.h>

#include <br/platform/br_plat.h>

/* MainLoop 周期。选 1s 是因为它同时是"人能看清的节奏"与"计时误差能被
 * 日志一眼量化"的长度。 */
#define BR_MAINLOOP_PERIOD_MS   1000u

BR_NORETURN void br_core_main(void)
{
    br_clock_init();          /* 日志时间戳依赖它, 必须最先 */
    br_log_init();
    br_log_set_level(BR_LOG_DEBUG);

    br_log_info("%s %s -- core MainLoop (delay + logging)",
                BR_PROTOTYPE_NAME, BR_VERSION_STRING);
    br_log_info("platform: %s (%s)", br_plat_name(), br_plat_isa());
    br_log_info("clock: %lu Hz (arch timer), %lu ticks/ms (exact integer conversion)",
                br_clock_freq_hz(), br_clock_ticks_per_ms());
    br_log_info("entry chain: start.S -> br_plat_early_init -> br_core_main");

    br_u64 tick = 0;

    for (;;) {
        tick++;

        const br_time_t t_before = br_clock_now();
        br_delay_ms(BR_MAINLOOP_PERIOD_MS);
        const br_time_t measured = br_clock_now() - t_before;

        /*
         * 自带判据的自检: 延时的唯一可验证性质就是"实际不短于请求"
         * (设计 3-01 §2.1: 到期唤醒不早醒, 晚到无上界)。
         * 把判据写进日志, 而不是靠人眼看节奏 —— QEMU 下这是能自动 grep 的。
         */
        const br_bool delay_ok = (measured >= BR_MAINLOOP_PERIOD_MS * BR_US_PER_MS);

        br_log_info("tick=%lu uptime=%lu us delay=%lu us (>=%lu us: %s)",
                    tick,
                    (br_u64)br_clock_now(),
                    (br_u64)measured,
                    (br_u64)(BR_MAINLOOP_PERIOD_MS * BR_US_PER_MS),
                    delay_ok ? "ok" : "EARLY");
    }
}
