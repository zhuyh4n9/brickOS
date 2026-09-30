/*
 * TangramOS prototype v0.1.0 — core 入口: MainLoop(延时 + 日志)
 *
 * 调用者: Platform Entry(platform/src/aarch64/start.S), 在 tg_plat_early_init()
 * 之后。这是 v0.1.0 全镜像里唯一的一条跨层边: Platform -> Core。
 *
 * ======================= WORKAROUND(tg-wa-boot-001) =======================
 * 设计侧这里是一整条启动链(1-01 §9 / §6.2 阶段表):
 *
 *   core.init(堆/中断框架/注册表/调度框架对象, 无线程)
 *     -> plugin_manager 扫 .tg_plugins 段 + 拓扑排序(环 = 硬错误)
 *       -> EARLY(调度插件注册 tg_sched_ops, core 锁定)
 *         -> CORE(非服务插件 init)
 *           -> LATE(Service -> Interface 依序 init)
 *             -> 全局开中断 -> 各插件 start()
 *               -> app.start() 创建 APP 线程
 *                 -> tg_sched_run() 首次调度, idle 进 WFI
 *
 * v0.1.0 **整条链缺席**: 没有堆、没有注册表、没有插件描述符、没有调度器,
 * 所以没有 EARLY/CORE/LATE 可挂, 也没有 idle 线程可以让出 CPU。
 * 于是这条链被压缩成"一个死循环里做延时 + 日志"。
 *
 * 关键认识: **`tg_core_main` 的归宿不是长大, 而是被拆掉。**
 *   - 顶层那行 `tg_plat_name()` 打印 -> 变成插件描述符枚举
 *   - `tg_log_init()`               -> 变成 core.init 的一步
 *   - 循环体                        -> 变成 app.start() 里的 APP 线程
 *   - 循环边的延时                  -> 变成 tg_task_sleep(M1)
 * ==========================================================================
 */
#include <tg/core/tg_main.h>
#include <tg/core/tg_log.h>
#include <tg/core/tg_time.h>
#include <tg/core/tg_version.h>

#include <tg/platform/tg_plat.h>

/* MainLoop 周期。选 1s 是因为它同时是"人能看清的节奏"与"计时误差能被
 * 日志一眼量化"的长度。 */
#define TG_MAINLOOP_PERIOD_MS   1000u

TG_NORETURN void tg_core_main(void)
{
    tg_clock_init();          /* 日志时间戳依赖它, 必须最先 */
    tg_log_init();
    tg_log_set_level(TG_LOG_DEBUG);

    tg_log_info("%s %s -- core MainLoop (delay + logging)",
                TG_PROTOTYPE_NAME, TG_VERSION_STRING);
    tg_log_info("platform: %s (%s)", tg_plat_name(), tg_plat_isa());
    tg_log_info("clock: %lu Hz (arch timer), %lu ticks/ms (exact integer conversion)",
                tg_clock_freq_hz(), tg_clock_ticks_per_ms());
    tg_log_info("entry chain: start.S -> tg_plat_early_init -> tg_core_main");

    tg_u64 tick = 0;

    for (;;) {
        tick++;

        const tg_time_t t_before = tg_clock_now();
        tg_delay_ms(TG_MAINLOOP_PERIOD_MS);
        const tg_time_t measured = tg_clock_now() - t_before;

        /*
         * 自带判据的自检: 延时的唯一可验证性质就是"实际不短于请求"
         * (设计 3-01 §2.1: 到期唤醒不早醒, 晚到无上界)。
         * 把判据写进日志, 而不是靠人眼看节奏 —— QEMU 下这是能自动 grep 的。
         */
        const tg_bool delay_ok = (measured >= TG_MAINLOOP_PERIOD_MS * TG_US_PER_MS);

        tg_log_info("tick=%lu uptime=%lu us delay=%lu us (>=%lu us: %s)",
                    tick,
                    (tg_u64)tg_clock_now(),
                    (tg_u64)measured,
                    (tg_u64)(TG_MAINLOOP_PERIOD_MS * TG_US_PER_MS),
                    delay_ok ? "ok" : "EARLY");
    }
}
