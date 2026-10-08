/*
 * brickOS prototype v0.1.0 — APP 入口(M0): MainLoop(中断驱动的心跳 + 延时 + 日志)
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
 *   core.init(TPIDR_EL1/中断框架等, 无线程)
 *     -> plugin_manager 扫 .br_plugins 段 + 拓扑排序(环 = 硬错误)
 *       -> EARLY(调度插件注册 br_sched_ops, core 锁定)
 *         -> CORE(非服务插件 init)
 *           -> LATE(Service -> Interface 依序 init)
 *             -> 全局开中断 -> 各插件 start()
 *               -> app.start() 创建 APP 线程
 *                 -> br_sched_run() 首次调度, idle 进 WFI
 *
 * v0.1.0 仍**没有**堆/注册表/插件描述符/调度器, 所以 EARLY/CORE/LATE 无从挂起。
 * 中断子系统(3-02 的 Stage 1)现在**已经在**:
 *   - `core.init` 的 TPIDR_EL1 这一步  -> `br_irq_cpu_init()`(start.S 调, 见其声明)
 *   - PIC/绑定表/能力协商               -> `br_plat_early_init()` 内的平台步骤 1–3
 *   - "全部插件 init 之后开中断"         -> 一致性用例入口里显式 `br_irq_cpu_enable()`
 *   - "core 在 core.init 注册 timer PPI 的 ISR" -> **本文件**(APP 是 v0.1 唯一有
 *     init/thread 上下文的角色)注册 + 使能, 设计 3-02 §11.1 的"路径四"(timer PPI)
 *   - `app.start()`                    -> `br_core_main()` 的死循环(仍未拆)
 * 也就是说: **中断框架这一半已经落地, 启动链这一半仍然欠着**。
 * ==========================================================================
 */
#include <br/core/br_main.h>
#include <br/core/br_log.h>
#include <br/core/br_time.h>
#include <br/core/br_version.h>

#include <br/platform/br_plat.h>
#include <br/board_irq.h>
#include <br/debug/br_dump.h>

/* MainLoop 周期。选 1s 是因为它同时是"人能看清的节奏"与"计时误差能被
 * 日志一眼量化"的长度。 */
#define BR_MAINLOOP_PERIOD_MS   1000u

/*
 * 中断心跳的**开跑**交给 platform(`br_plat_irq_start()`: 注册 timer PPI 的 ISR +
 * 使能 + 装弹 + 全局开中断)。
 *
 * ★ 为什么 APP 不自己 `br_irq_register`(P-IRQ-17): 设计 3-01 §13.6 的特权分级把
 *   "中断控制"归 **P3**(仅调度类 ability 与 platform 可声明), APP 是 **P0**。
 *   在 P0 的位置上做 P3 的事是一处**声明面与实现不一致**, 而 `brickie check` 的
 *   priv 域(声明合法性)看不到源码、不会报红 ⇒ 只能靠**放置**避免。
 *   ⇒ APP 只读平台的心跳计数, 始终是纯 P0 消费者。
 */
static void mainloop_irq_setup(void)
{
    const int r = br_plat_irq_start();
    if (r != 0) {
        br_log_error("int: platform irq start failed: %d", r);
        return;
    }
    br_log_info("int: timer PPI armed by platform (virq=%u INTID=%u, 100 ms)",
                (br_u32)BR_IRQ_TIMER, 30u);
}

BR_NORETURN void br_core_main(void)
{
    br_clock_init();          /* 日志时间戳依赖它, 必须最先 */
    br_log_init();
    br_log_set_level(BR_LOG_DEBUG);

    br_log_info("%s %s -- core MainLoop (interrupt heartbeat + delay + logging)",
                BR_PROTOTYPE_NAME, BR_VERSION_STRING);
    br_log_info("platform: %s (%s)", br_plat_name(), br_plat_isa());
    br_log_info("clock: %lu Hz (arch timer), %lu ticks/ms (exact integer conversion)",
                br_clock_freq_hz(), br_clock_ticks_per_ms());
    br_log_info("entry chain: start.S -> br_irq_cpu_init -> br_plat_early_init -> br_core_main");

    /*
     * 中断子系统一致性用例(设计 6-01 §3.7 的 TC-IRQ-*, target-only)。
     * 放在 MainLoop 之前: 它是启动期的自检, 红了就该在第一时间看见。
     * 入口内部会执行"全局开中断"(设计 §14.3 的最后一步)。
     */
    const int conf_fail = br_plat_irq_conformance();
    br_log_info("int: conformance %s (failures=%d)",
                (conf_fail == 0) ? "ALL PASS" : "HAS FAILURES", conf_fail);

    /*
     * 内存映射子系统一致性用例(设计 6-01 §3.5/§3.6 的 `TC-MEM-*` 与 `TC-MM-*`)。
     * 放在 MainLoop 之前同上: 启动期自检。此时 MMU 已由 platform early_init 打开
     * (恒等映射 + region 属性), 所以用例里的"真正未映射地址"才取翻译 fault。
     */
    const int mem_fail = br_plat_mem_conformance();
    br_log_info("mem: conformance %s (failures=%d)",
                (mem_fail == 0) ? "ALL PASS" : "HAS FAILURES", mem_fail);

    /*
     * 调试域一致性用例(设计 5-01 §3 的捕获集: region 表/堆账/泄漏/trace/回溯)。
     * ★ 这条 app → service/dump 的边是 `product.toml [lint].allow_edges` 里的第二条
     *   **M0 引导例外**(设计 §7.3 的表里 app ✗ ability; Interface 层 `iface-min` 属 M2,
     *   运行期插件管理器属 M0)⇒ 属 WORKAROUND(br-wa-boot-001) 的欠债, 不是静默放行。
     *   APP 只认识 dump 一个面: 其余四个调试插件的 LATE 相 init 与 selftest 由
     *   `br_dump_conformance()` 按声明面依赖序代调(它们都是 dump 的 `[[dep]]`)。
     */
    const int dbg_fail = br_dump_conformance();
    br_log_info("dbg: conformance %s (failures=%d)",
                (dbg_fail == 0) ? "ALL PASS" : "HAS FAILURES", dbg_fail);

    /* 启动现场一份(三套门禁截取证据的地方; 行数口径见 br_dump.h) */
    const br_u32 dump_lines = br_dump_all();
    br_log_info("dbg: boot snapshot lines=%lu", (br_u64)dump_lines);

    mainloop_irq_setup();

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

        br_log_info("tick=%lu uptime=%lu us delay=%lu us (>=%lu us: %s) irq_ticks=%lu",
                    tick,
                    (br_u64)br_clock_now(),
                    (br_u64)measured,
                    (br_u64)(BR_MAINLOOP_PERIOD_MS * BR_US_PER_MS),
                    delay_ok ? "ok" : "EARLY",
                    (br_u64)br_plat_timer_ticks());

        /*
         * 第 2 拍再取一次 trace: 此时 timer PPI 已经跑了 ~20 次, 环里是**中断上下文**
         * 落下的事件(IRQ_ENTER/EXIT)—— 这是"trace 在 ISR 里可用"(5-01 §1 + CA-3 白名单)
         * 在真机上的活证据, 也是调试服务在稳定态可用的证明(启动那一刻的现场已由
         * `br_dump_all()` 取过)。
         */
        if (tick == 2u) {
            const br_u32 drained = br_dump_trace(0u);
            br_log_info("dbg: steady-state trace drained=%lu", (br_u64)drained);
        }
    }
}
