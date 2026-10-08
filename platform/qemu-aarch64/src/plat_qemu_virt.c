/*
 * brickOS prototype v0.2.0 — QEMU virt (aarch64) 平台实现
 *
 * 设计对应: 本文件是 Platform 插件 `platform/qemu-aarch64` 的实现主体
 * (1-03 §1 插件清单第一行: QEMU virt: EL1、GICv3、PL011、arch timer、恒等映射页表、
 *  region 表; 里程碑 M0)。取其中 MainLoop 需要的三件: PL011 + arch timer +
 *  GICv3 中断子系统(初始化链见 br_plat_early_init)。
 *
 * 已还清(ADR-0005): 插件化已完成(manifest = ../plugin.toml, 描述符由
 * `brickie gen` 生成, 调用点由 core 的插件管理器经 `.br_plugins` 段枚举驱动)。
 * 标记保留到 WORKAROUNDS.md 由主控同步删除。
 *
 * 本文件同时是**插件生命周期钩子**的落点(设计 1-01 §9): 钩子名 = `symbol_prefix(short)`
 * + 相 ⇒ `qemu_aarch64_early_init` / `qemu_aarch64_init` / `qemu_aarch64_start`,
 * 由生成物 `build/gen/platform/qemu-aarch64/plugin_desc.c` 引用。
 */
#include <br/core/br_console.h>
#include <br/core/br_log.h>
#include <br/board_irq.h>            /* BR_IRQ_TIMER(绑定表里的 virq) */
#include <br/platform/br_mmu.h>
#include <br/platform/br_plat.h>

/* platform 插件将来在描述符里的 name 字段(设计 1-01 §6.1) */
#define BR_PLAT_NAME_STR   "qemu-aarch64/virt"
#define BR_PLAT_ISA_STR    "aarch64"

/* 生命周期钩子自带原型满足 -Wmissing-prototypes(钩子名由生成器推导, 不是本插件的
 * API 面 —— 不写进 plugin.toml 的 [[export]], 否则会改接口 hash)。 */
int qemu_aarch64_early_init(void);
int qemu_aarch64_init(void);
int qemu_aarch64_start(void);

/*
 * EARLY 相(plugin_manager 的**第一步**, 显式取 plugin_type == platform):
 * console → PIC 注册/能力协商/绑定表 → region 表 → 三池认领 → 4 KiB 恒等映射 + 开 MMU。
 * 失败即启动失败: `br_plat_early_init()` 内部已经 [FATAL] + park(同"首败即停机"),
 * 它**返回**就说明三步全绿 ⇒ 这里恒 0。
 */
int qemu_aarch64_early_init(void)
{
    br_plat_early_init();
    return 0;
}

/*
 * CORE 相(类别决定的 ② 完成点: 非 Service/Interface ⇒ CORE, 设计 1-01 §9)。
 * 平台自身**没有额外动作**: console/PIC/绑定表/region 表/页表/MMU 都必须在任何插件
 * init 之前就位, 所以它们全在 EARLY 相。保留这个钩子是为了让"platform 的 ② 完成点在
 * CORE"(plugin.toml 的 `phase = "core"`)在启动序里**可见**, 而不是被省掉。
 */
int qemu_aarch64_init(void)
{
    return 0;
}

/*
 * START 相(**全局开中断之后**, 设计 §6.2 的表): 设备与中断一起开跑。
 *   ① 中断/内存映射一致性用例(启动期自检 —— 红了就该在第一时间看见);
 *   ② `br_plat_irq_start()`: 注册 timer PPI 的 ISR + 使能该线 + 装第一个 100 ms 期限
 *      + 全局开中断(已在管理器里开过, 这里幂等) —— 这是"心跳"的开始。
 *
 * ★ 为什么 ② 放在 ① 之后: 保持与 v0.1 完全相同的时序(那时 conformance 先跑,
 *   MainLoop 之前才 arm timer), 于是 irq-test 的逐用例判据**一个字都不用动**。
 *   F2/F3 要的"tick 已经在跑"由 ② 在 platform.start 内完成来保证 —— 它严格早于
 *   APP 的 start()(管理器把 APP 的 start 排在最后)。
 *
 * 一致性用例的失败**不**作为 start 的失败返回: 它由门禁([IRQCONF]/[MEMCONF] 的
 * FAIL 模式)判红, 而 start 的返回值只表达"设备 bring-up 成不成"(见 ADR-0005 §2.5)。
 */
int qemu_aarch64_start(void)
{
    const int irq_fail = br_plat_irq_conformance();
    br_log_info("int: conformance %s (failures=%d)",
                (irq_fail == 0) ? "ALL PASS" : "HAS FAILURES", irq_fail);

    const int mem_fail = br_plat_mem_conformance();
    br_log_info("mem: conformance %s (failures=%d)",
                (mem_fail == 0) ? "ALL PASS" : "HAS FAILURES", mem_fail);

    const int r = br_plat_irq_start();
    if (r != 0) {
        br_log_error("int: platform irq start failed: %d", r);
        return r;   /* 首败即停机由 plugin_manager 执行(裁定 G6) */
    }
    br_log_info("int: timer PPI armed by platform (virq=%u INTID=%u, 100 ms)",
                (br_u32)BR_IRQ_TIMER, 30u);
    return 0;
}

const char *br_plat_name(void)
{
    return BR_PLAT_NAME_STR;
}

const char *br_plat_isa(void)
{
    return BR_PLAT_ISA_STR;
}

void br_plat_early_init(void)
{
    /*
     * 顺序有讲究(设计 1-01 §9 / 3-02 §14.3): 早期 console 必须最先就绪, 否则
     * init 链上任何失败都是静默的 —— 原型阶段的排障成本几乎全在这里。
     * 紧接着是中断子系统的平台侧三步(3-02 §14.3 步 1–3, 实现在 board_irq.c):
     *   PIC 注册(GICv3 初始化)→ [caps_get] → 绑定表提交。
     * ★ EARLY 相失败 = **启动失败**: 此时全局关中断、系统还没法跑, 明确停机比
     *   "降级到无中断"诚实(§14.3 的错误处理义务)。这里刻意不用 br_panic_bare,
     *   因为 fault 路径与 panic 属 core 的观测契约, 而停机入口已由平台提供。
     *
     * 最后是内存映射子系统(设计 3-04 §1「恒等映射 + 属性隔离」; 1-01 §9 的
     * "RAM / 恒等映射页表"两步), 顺序同样是硬的:
     *   ① `br_plat_memmap_init()`: 声明 region 表 → `br_mem_init()` 认领三池。
     *      必须先有 region 表: 它是"哪块 RAM 是什么"的唯一真值, 页表与 core 的池都从它取。
     *   ② `br_plat_mmu_init()`: `br_mm_register` → `br_mm_activate` 建 4 KiB 恒等映射
     *      页表并开 MMU。★ MMU 一开, 之后每一次取指/访存都过翻译 ⇒ 页表必须已经覆盖
     *      当前 PC/SP(页表自身也在 .bss 的映射范围内)。
     *   为什么放在 IRQ 之后: GIC 的配置在最前面的三步里完成, 此刻 MMU 还没开, 走的
     *   是"物理地址直取"; 开 MMU 之后 GIC 寄存器走 Device 背景映射(0x08000000/0x080A0000
     *   都在低 1 GiB 的 2 MiB 块里), 两种情形都被覆盖。
     */
    br_console_init();

    if (br_plat_irq_init() != 0) {
        br_console_puts("[FATAL] platform IRQ init failed\n");
        br_plat_park_forever();
    }

    if (br_plat_memmap_init() != 0) {
        br_console_puts("[FATAL] mem region declare failed\n");
        br_plat_park_forever();
    }

    if (br_plat_mmu_init() != 0) {
        br_console_puts("[FATAL] memory mapping init failed\n");
        br_plat_park_forever();
    }

    /* arch timer 由固件/QEMU 预置 CNTFRQ_EL0, 无需初始化; 频率换算见 board_irq.c
     * 的 br_plat_timer_*(装弹写 CNTP_TVAL_EL0 / CNTP_CTL_EL0)。 */
}

BR_NORETURN void br_plat_park_forever(void)
{
    for (;;) {
        /* WFE: 无调度器时的"最低功耗空转"。中断全屏蔽, 不会真的醒来。 */
        __asm__ volatile("wfe" ::: "memory");
    }
}

/*
 * 向量表统一落点。设计对应 3-02 §8.3.1 的 bare 路径: fault/双重 fault 下
 * **不调用任何可能已损坏的 core 设施**(不打日志、不取锁、不分配), 只用轮询 console
 * 输出最小信息后停机。
 *
 * 注意这里是"喊一声"级别, 不是设计的 `br_panic_bare(fmt, ...)`: 那个函数
 * 属于 core 的跨模块观测契约(3-02 §17.4 要求登记), 随中断框架(M0+)落地。
 */
BR_NORETURN void br_plat_unexpected_exception(br_u64 vector)
{
    char digits[20];
    br_u32 n = 0;

    br_console_puts("\n[FATAL] unhandled exception -- vector=");

    if (vector == 0) {
        digits[n++] = '0';
    }
    while (vector != 0 && n < sizeof(digits)) {
        digits[n++] = (char)('0' + (br_u32)(vector % 10u));
        vector /= 10u;
    }
    while (n > 0) {
        br_console_putc(digits[--n]);
    }

    br_console_puts(" -- parked\n");

    br_plat_park_forever();
}
