/*
 * brickOS prototype v0.1.0 — QEMU virt (aarch64) 平台实现
 *
 * 设计对应: 本文件是 Platform 插件 `platform/qemu-aarch64` 的实现主体
 * (1-03 §1 插件清单第一行: QEMU virt: EL1、GICv3、PL011、arch timer、恒等映射页表、
 *  region 表; 里程碑 M0)。v0.1.0 取其中 MainLoop 需要的三件: PL011 + arch timer +
 *  GICv3 中断子系统(初始化链见 br_plat_early_init)。
 *
 * WORKAROUND(br-wa-entry-001): 插件化已完成(manifest = ../plugin.toml, 描述符由
 * `brickie gen` 生成); **仍欠**的是"调用点由 .br_plugins 段枚举驱动"(M0 运行期)。
 */
#include <br/core/br_console.h>
#include <br/platform/br_plat.h>

/* platform 插件将来在描述符里的 name 字段(设计 1-01 §6.1) */
#define BR_PLAT_NAME_STR   "qemu-aarch64/virt"
#define BR_PLAT_ISA_STR    "aarch64"

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
     * v0.1.0 仍不做的(等 M0/M2): 恒等映射页表、region 表、cache 维护。
     */
    br_console_init();

    if (br_plat_irq_init() != 0) {
        br_console_puts("[FATAL] platform IRQ init failed\n");
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
