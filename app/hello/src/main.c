/*
 * brickOS prototype v0.2.0 — APP 插件 `app/hello`: MainLoop(中断驱动的心跳 + 延时 + 日志)
 *
 * 调用者: **core 的插件管理器**(`br_plugin_manager_run()`)。本文件实现三个生命周期钩子
 * (名字 = `symbol_prefix(short)` + 相, 由 `build/gen/app/hello/plugin_desc.c` 引用):
 *
 *   hello_early_init()  EARLY 相: 无动作(此相不使用堆/无线程/关中断, console 刚由
 *                       platform 的 early_init 建好; APP 在这里没有可做的事)。
 *   hello_init()        CORE 相(类别决定的 ② 完成点: 非 Service/Interface ⇒ CORE, 1-01 §9)。
 *   hello_start()       START 相(全局开中断之后, 且**排在最后** —— 管理器显式把 APP 的
 *                       start 放到所有非 APP 的 start 之后): 启动期自检调用序列 + MainLoop。
 *
 * 启动链(设计 1-01 §9; core 侧的四阶段见 `br/core/br_main.h` 与 ADR-0008):
 *   start.S(reset/BSS) → br_core_main:
 *     ① core 平台无关(时钟/日志/中断框架/插件管理扫段)
 *     ② platform 插件初始化(console/PIC/region 表/页表 ops)
 *     ③ core 依赖平台(堆 + 恒等映射的建立)
 *     ④ plugin_manager: EARLY(其余) → CORE → LATE → 全局开中断 → START(APP 最后)
 *          → [本文件] 一致性用例(main 线程) + **创建 APP 线程** → return 0
 *            → 管理器 `br_sched_run()` 首次调度 ⇒ APP 线程接管 MainLoop
 *
 * `br_core_main()` 与 `core/include/br/core/br_main.h` 在 ADR-0005 那一刀里曾被**拆掉**
 * (编排权当时收进了插件管理器); ADR-0008 把 `core.init` 抽成独立入口后又**重建**了它们
 * —— 现在的形态是"四阶段启动链", 与 M0 那个"在 APP 里直调各子系统"的旧 `br_core_main`
 * 不是一回事(旧入口的日志/自检/主循环仍归 platform.start / dump 的 LATE init / 本文件)。
 *
 * WORKAROUND(br-wa-boot-001): ① **已还清**(APP 线程 + `br_task_sleep`);
 * 仍欠的 ②(日志/trace 直写 console, 未经服务注册表)的标记在 `core/src/log.c`。
 * 本文件另有一条标记在 `hello_mainloop()` 上方(说明"谁占住 CPU"已归调度器)。
 */
#include <hello/hello.h>

#include <br/core/br_log.h>
#include <br/core/br_plugin.h>
#include <br/core/br_sched.h>
#include <br/core/br_svc.h>
#include <br/core/br_sync.h>
#include <br/core/br_time.h>
#include <br/core/br_version.h>

#include <br/platform/br_plat.h>

/* MainLoop 周期。选 1s 是因为它同时是"人能看清的节奏"与"计时误差能被
 * 日志一眼量化"的长度。 */
#define BR_MAINLOOP_PERIOD_MS   1000u

/*
 * EARLY 相: 无动作。
 * APP 在 EARLY 相没有"注册"要做(设计 1-01 §6.2: EARLY = 不用堆/无线程/关中断,
 * 是 platform 与调度插件的相); 这里必须有定义, 因为生成物对**全部插件**都发射
 * `early_init`(§6.2 的表: EARLY 相覆盖全部插件)。
 */
int hello_early_init(void)
{
    return 0;
}

/*
 * CORE 相: APP 的准备。
 * 日志等级在 core 的日志设施就绪之后统一放开 —— 之后各相(含 LATE 的 service init)
 * 的 DEBUG 行才不会丢。其余初始化(时钟/日志起点)由插件管理器的头部代做。
 */
int hello_init(void)
{
    br_log_set_level(BR_LOG_DEBUG);
    br_log_info("app: init (CORE 相; MainLoop 在 start 相)");
    return 0;
}

/*
 * APP 线程的栈(原型无动态分配 —— 栈由调用方给)。
 * 8 KiB: MainLoop 的调用链很浅, 但日志格式化要占几百字节; BR_STACK_MIN = 2 KiB 是硬下界。
 */
#define BR_APP_STACK_BYTES  8192u
static br_u8 s_app_stack[BR_APP_STACK_BYTES] BR_ALIGN(16);

/*
 * APP 线程体 = 原 MainLoop(**不返回**)。
 *
 * ★ 为什么是线程而不是在 start 相的函数体里直接循环: 设计 `1-01` §9 的启动序列是
 *   `app.start() 创建 APP 线程 → (全部 start 完毕) → br_sched_run() 首次调度`;
 *   coop 只在显式点换栈 ⇒ "谁占住 CPU"必须由调度器决定, 而不是由 start 相决定。
 *   这一段就是 WORKAROUND(br-wa-boot-001) 的"主循环托底"还债点。
 *
 * ⚠ 循环里的"睡眠"现在是 `br_task_sleep`(真阻塞切换): 唤醒精度 = **周期 tick 的
 *   100 ms**(平台 timer 周期; tickless 的"比较器按最近期限装弹"尚未做, 见 ADR-0006 §5)
 *   ⇒ 每拍实际约 1.00–1.10 s, 3 秒的 smoke 门禁里 `tick=2` 仍稳(实测 ~2.48 s)。
 *   这是 WORKAROUND(br-wa-boot-001) ① 的还债点(M1 的调度器接过 CPU 占用)。
 */
static void hello_mainloop(void *arg)
{
    (void)arg;

    br_u64 tick = 0;

    for (;;) {
        tick++;

        const br_time_t t_before = br_clock_now();
        (void)br_task_sleep(BR_MAINLOOP_PERIOD_MS * BR_US_PER_MS);
        const br_time_t measured = br_clock_now() - t_before;

        /*
         * 自带判据的自检: 延时的唯一可验证性质就是"实际不短于请求"
         * (设计 3-01 §2.1: 到期唤醒不早醒, 晚到无上界)。
         * 把判据写进日志, 而不是靠人眼看节奏 —— QEMU 下这是能自动 grep 的。
         * ⚠ 这条日志里的 "EARLY" 是 smoke 门禁的判据之一(延时短于请求); 插件管理器
         *   的相名因此取小写, 免得误触(见 plugin_mgr.c 的文件头注)。
         */
        const br_bool delay_ok = (measured >= BR_MAINLOOP_PERIOD_MS * BR_US_PER_MS);

        br_log_info("tick=%lu uptime=%lu us delay=%lu us (>=%lu us: %s) irq_ticks=%lu",
                    tick,
                    (br_u64)br_clock_now(),
                    (br_u64)measured,
                    (br_u64)(BR_MAINLOOP_PERIOD_MS * BR_US_PER_MS),
                    delay_ok ? "ok" : "EARLY",
                    (br_u64)br_plat_timer_ticks());
    }
}

/*
 * START 相: 启动期自检 + MainLoop(**不返回**)。
 *
 * 自检的分工(设计 6-01 的用例组):
 *   - `[PLGCONF]` 插件管理器(本镜像的 `.br_plugins` / init-DAG / 相位);
 *   - `[SVCCONF]` 服务注册表(core 公地);
 *   - `[TASKCONF]` / `[SYNCCONF]` 调度框架与同步原语(F2/F3 的实现; 调用点先接线)。
 * 中断(`[IRQCONF]`)/内存(`[MEMCONF]`)/调试域(`[DBGCONF]`)三套**已按相位归位**:
 * 前两者在 platform 的 start, 后者在 service/dump 的 LATE init —— APP 不再直调它们
 * (这正是 `product.toml [lint].allow_edges` 里 app → service/dump 那条豁免可以删掉的依据)。
 *
 * ★ 为什么 APP 不自己碰中断控制(P-IRQ-17): 设计 3-01 §13.6 的特权分级把"中断控制"
 *   归 P3(仅调度类 ability 与 platform 可声明), APP 是 P0 ⇒ 只能**读**平台的心跳计数。
 *
 * ★ 本函数现在**返回 0**: 它创建 APP 线程(体 = 上面的 `hello_mainloop`), 然后回到
 *   插件管理器; 由管理器在全部 start 之后调 `br_sched_run()` 做首次调度。
 *   (自检跑在"main 线程"里 —— `br_sched_register()` 在 EARLY 相就把调用者所在的启动
 *    上下文物质化成了 main, 见 ADR-0006 §1 的 I1 ⇒ create/join 在调度循环之前可用。)
 */
int hello_start(void)
{
    br_log_info("%s %s -- core MainLoop (interrupt heartbeat + delay + logging)",
                BR_PROTOTYPE_NAME, BR_VERSION_STRING);
    br_log_info("platform: %s (%s)", br_plat_name(), br_plat_isa());
    br_log_info("clock: %lu Hz (arch timer), %lu ticks/ms (exact integer conversion)",
                br_clock_freq_hz(), br_clock_ticks_per_ms());
    br_log_info("entry chain: start.S -> br_core_main "
                "(core.init -> platform.init -> core.plat.init -> plugin_manager: "
                "EARLY/CORE/LATE -> irq on -> START) -> app thread -> br_sched_run");

    /* 插件管理器一致性用例(TC-PLUG-*): 段条数/拓扑序/相位单调/环检测负例。 */
    br_plugin_conformance();

    /* 服务注册表一致性用例(TC-SVC-*)。 */
    br_service_conformance();

    /*
     * 调度框架与同步原语的一致性用例。这两个入口必须由 APP 的 start 调用
     * (与其余 conformance 同一位置、同一形态); 它们的实现分别属 F2/F3 ——
     * 调用点先接线, 实现后补。此刻 timer 已在 platform 的 start 里 armed,
     * 所以里面的真超时用例(20 ms / 不早醒)有节拍可用。
     */
    br_sched_conformance();
    br_sync_conformance();

    /* 创建 APP 线程: 此后 CPU 的占用由调度器决定(coop: 只在显式点换栈)。 */
    const br_task_attr_t attr = {
        .name       = "app",
        .stack      = (void *)(s_app_stack + BR_APP_STACK_BYTES),   /* 栈顶(高地址) */
        .stack_size = BR_APP_STACK_BYTES,
        .prio       = 0u,
        .flags      = 0u,
    };
    br_thread_t *app = BR_NULL;
    const int rc = br_task_create(&app, &attr, hello_mainloop, BR_NULL);
    if (rc != 0) {
        br_log_error("app: br_task_create(APP 线程) 失败 rc=%d", rc);
        return rc;
    }
    br_log_info("app: APP 线程已创建(name=%s state=%u); 交给 br_sched_run() 首次调度",
                br_task_name(app), (unsigned)br_task_state(app));
    return 0;
}
