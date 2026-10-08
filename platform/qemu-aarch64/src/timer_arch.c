/*
 * brickOS prototype v0.1.0 — aarch64 arch timer(CNTFRQ/CNTPCT)
 *
 * 设计对应(1-01 §8 三层模式):
 *   core 的 `br_clock` tickless 语义  <- 契约
 *   arch timer 读法                   <- ISA 公共部分(本文件)
 *   频率/校准                         <- 平台数据(CNTFRQ_EL0 由平台/固件设好)
 *
 * 为什么读 CNTPCT_EL0 而不是 CNTVCT_EL0: 物理计数器在 EL1 无需使能、
 * 与虚拟偏移无关, QEMU virt 上恒可用。tickless 的"比较器/中断"是 M1 的事。
 *
 * 已还清(ADR-0005): 本文件是 Platform Entry 的一部分(未做成插件 —— 这是**设计内**的形态,
 */
#include <br/platform/br_plat.h>

br_u64 br_plat_ticks_freq(void)
{
    br_u64 freq;

    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq;
}

br_u64 br_plat_ticks_now(void)
{
    br_u64 ticks;

    /* isb: 保证读计数器之前的所有指令已生效(乱序/预取不越过读数)。
     * 不读时钟的中断安全路径不经过这里 —— 设计侧那是 3-02 §8.3.1 的讨论。 */
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(ticks));
    return ticks;
}
