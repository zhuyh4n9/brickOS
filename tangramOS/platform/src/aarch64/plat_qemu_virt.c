/*
 * TangramOS prototype v0.1.0 — QEMU virt (aarch64) 平台实现
 *
 * 设计对应: 本文件将来整体成为 Platform 插件 `platform/qemu-aarch64`
 * (1-03 §1 插件清单第一行: QEMU virt: EL1、GICv3、PL011、arch timer、恒等映射页表、
 *  region 表; 里程碑 M0)。v0.1.0 只取其中 MainLoop 需要的两件: PL011 + arch timer。
 *
 * WORKAROUND(tg-wa-entry-001): 未做成插件, 由 start.S 直接调用。见 WORKAROUNDS.md。
 */
#include <tg/core/tg_console.h>
#include <tg/platform/tg_plat.h>

/* platform 插件将来在描述符里的 name 字段(设计 1-01 §6.1) */
#define TG_PLAT_NAME_STR   "qemu-aarch64/virt"
#define TG_PLAT_ISA_STR    "aarch64"

const char *tg_plat_name(void)
{
    return TG_PLAT_NAME_STR;
}

const char *tg_plat_isa(void)
{
    return TG_PLAT_ISA_STR;
}

void tg_plat_early_init(void)
{
    /*
     * 顺序有讲究(设计 1-01 §9): 早期 console 必须最先就绪, 否则 init 链上
     * 任何失败都是静默的 —— 原型阶段的排障成本几乎全在这里。
     *
     * v0.1.0 刻意不做的(等 M0/M2): GICv3 初始化、恒等映射页表、region 表、
     * cache 维护。MainLoop 不碰内存管理与中断, 做了也无法验证。
     */
    tg_console_init();

    /* arch timer 由固件/QEMU 预置 CNTFRQ_EL0, 无需初始化; 只需在 core 侧换算。 */
}

TG_NORETURN void tg_plat_park_forever(void)
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
 * 注意这里是"喊一声"级别, 不是设计的 `tg_panic_bare(fmt, ...)`: 那个函数
 * 属于 core 的跨模块观测契约(3-02 §17.4 要求登记), 随中断框架(M0+)落地。
 */
TG_NORETURN void tg_plat_unexpected_exception(tg_u64 vector)
{
    char digits[20];
    tg_u32 n = 0;

    tg_console_puts("\n[FATAL] unhandled exception -- vector=");

    if (vector == 0) {
        digits[n++] = '0';
    }
    while (vector != 0 && n < sizeof(digits)) {
        digits[n++] = (char)('0' + (tg_u32)(vector % 10u));
        vector /= 10u;
    }
    while (n > 0) {
        tg_console_putc(digits[--n]);
    }

    tg_console_puts(" -- parked\n");

    tg_plat_park_forever();
}
